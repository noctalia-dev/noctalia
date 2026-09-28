// Drives the data-control callbacks with a fake compositor, covering two things
// that are otherwise only observable against a live compositor: what happens to
// the selection when the client owning it exits, and which entries history gives
// up when it runs out of budget.

#include "security/secret_store.h"
#include "security/storage_key_provider.h"
#include "wayland/clipboard_poll_source.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <poll.h>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

  int gFailures = 0;

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "clipboard_service_test: {}", message);
      ++gFailures;
    }
    return condition;
  }

  class UnavailableSecretBackend final : public security::SecretStoreBackend {
  public:
    security::SecretStoreBackendResult probe(security::SecretStoreCancellation&) override { return unavailable(); }

    security::SecretStoreBackendResult
    lookup(const security::SecretStoreAttributes&, security::SecretStoreCancellation&) override {
      return unavailable();
    }

    security::SecretStoreBackendResult store(
        const security::SecretStoreAttributes&, std::span<const std::uint8_t>, const std::string&,
        security::SecretStoreCancellation&
    ) override {
      return unavailable();
    }

    security::SecretStoreBackendResult
    erase(const security::SecretStoreAttributes&, security::SecretStoreCancellation&) override {
      return unavailable();
    }

  private:
    static security::SecretStoreBackendResult unavailable() {
      return {
          .status = security::SecretStoreStatus::Unavailable,
          .errorCategory = security::SecretStoreErrorCategory::ProviderUnavailable,
      };
    }
  };

  // Stands in for the compositor: serves the payload a client would write into
  // the receive pipe, and records the selections the shell claims.
  struct FakeCompositor {
    std::vector<std::uint8_t> payload;
    std::size_t written = 0;
    int writeFd = -1;
    std::vector<std::string> offeredMimeTypes;
    int claims = 0;
    int sources = 0;
  };

  FakeCompositor* gFake = nullptr;
  int gManager = 0;
  int gDevice = 0;
  int gSource = 0;
  wl_seat* const gSeat = reinterpret_cast<wl_seat*>(&gManager);

  void* fakeGetDataDevice(void*, wl_seat*) { return &gDevice; }
  void fakeDestroyDevice(void*) {}
  int fakeAddDeviceListener(void*, const void*, void*) { return 0; }

  void* fakeCreateDataSource(void*) {
    ++gFake->sources;
    gFake->offeredMimeTypes.clear();
    return &gSource;
  }

  void fakeDestroySource(void*) {}
  int fakeAddSourceListener(void*, const void*, void*) { return 0; }
  void fakeSourceOffer(void*, const char* mimeType) { gFake->offeredMimeTypes.emplace_back(mimeType); }
  void fakeDeviceSetSelection(void*, void*) { ++gFake->claims; }
  void fakeDestroyOffer(void*) {}
  int fakeAddOfferListener(void*, const void*, void*) { return 0; }

  // Serves the payload the way a real client does: the pipe is non-blocking and
  // holds 64 KiB, so anything larger has to be written as the reader drains it.
  // EOF is signalled by closing this side once everything is out.
  void pumpWrites() {
    FakeCompositor& fake = *gFake;
    if (fake.writeFd < 0) {
      return;
    }
    while (fake.written < fake.payload.size()) {
      const ssize_t n = ::write(fake.writeFd, fake.payload.data() + fake.written, fake.payload.size() - fake.written);
      if (n > 0) {
        fake.written += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      }
      break;
    }
    ::close(fake.writeFd);
    fake.writeFd = -1;
  }

  // The service closes its own copy of the write end right after this returns,
  // so the duplicate here is what keeps the pipe open until the payload is out.
  void fakeOfferReceive(void*, const char*, int fd) {
    gFake->written = 0;
    gFake->writeFd = ::dup(fd);
    pumpWrites();
  }

  const DataControlOps& fakeOps() {
    static const DataControlOps ops = {
        .managerInterfaceName = "fake_data_control",
        .bindManager = nullptr,
        .destroyManager = nullptr,
        .getDataDevice = fakeGetDataDevice,
        .destroyDevice = fakeDestroyDevice,
        .addDeviceListener = fakeAddDeviceListener,
        .createDataSource = fakeCreateDataSource,
        .destroySource = fakeDestroySource,
        .addSourceListener = fakeAddSourceListener,
        .sourceOffer = fakeSourceOffer,
        .deviceSetSelection = fakeDeviceSetSelection,
        .destroyOffer = fakeDestroyOffer,
        .addOfferListener = fakeAddOfferListener,
        .offerReceive = fakeOfferReceive,
    };
    return ops;
  }

  std::vector<std::uint8_t> bytesOf(std::string_view text) { return {text.begin(), text.end()}; }

  // filler distinguishes payloads: identical bytes are deduplicated against the
  // most recent entry, so same-content images would never reach the budget.
  std::vector<std::uint8_t> pngPayloadOfSize(std::size_t bytes, bool complete, std::uint8_t filler = 0x40) {
    std::vector<std::uint8_t> data = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    data.resize(bytes, filler);
    if (complete) {
      const std::vector<std::uint8_t> trailer = {0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
                                                 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
      data.insert(data.end(), trailer.begin(), trailer.end());
    }
    return data;
  }

  std::vector<std::uint8_t> pngPayload(bool complete) { return pngPayloadOfSize(256, complete); }

  void drainReads(ClipboardService& clipboard) {
    for (int attempt = 0; attempt < 20000; ++attempt) {
      std::vector<pollfd> fds;
      const std::size_t count = clipboard.addPollFds(fds);
      if (count == 0) {
        if (gFake->writeFd >= 0) {
          ::close(gFake->writeFd);
          gFake->writeFd = -1;
        }
        clipboard.dispatchPollEvents(fds, 0, 0);
        return;
      }
      const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 500);
      if (ready <= 0) {
        return;
      }
      clipboard.dispatchPollEvents(fds, 0, count);
      pumpWrites();
    }
  }

  void flushDeferredAdopt(ClipboardService& clipboard) {
    std::vector<pollfd> fds;
    clipboard.dispatchPollEvents(fds, 0, 0);
  }

  // Plays out one copy: a client offers a type, becomes the selection owner and
  // serves its bytes.
  void simulateCopy(ClipboardService& clipboard, void* offer, const char* mimeType, std::vector<std::uint8_t> payload) {
    gFake->payload = std::move(payload);
    gFake->written = 0;
    clipboard.handleDataOffer(offer);
    clipboard.handleOfferMimeType(offer, mimeType);
    clipboard.handleSelection(offer);
    drainReads(clipboard);
  }

  struct Harness {
    security::SecretStore store{std::make_unique<UnavailableSecretBackend>()};
    security::StorageKeyProvider keys{store};
    ClipboardService clipboard{keys};

    Harness() { (void)clipboard.bind(&gManager, &fakeOps(), gSeat); }
  };

} // namespace

int main() {
  FakeCompositor fake;
  gFake = &fake;

  int offerIds[8] = {};

  {
    // A complete image whose owner exits is claimed back by the shell.
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[0], "image/png", pngPayload(/*complete=*/true));
    expect(fake.claims == 0, "claimed the selection while the owner was still alive");

    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    expect(fake.claims == 1, "did not claim the selection after the owner exited");
    expect(
        fake.offeredMimeTypes == std::vector<std::string>{"image/png"},
        "adopted selection did not offer exactly the type it holds bytes for"
    );
  }

  {
    // Text keeps its aliases, because those are the same bytes.
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[1], "text/plain;charset=utf-8", bytesOf("hello"));
    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    expect(fake.claims == 1, "did not adopt an orphaned text selection");
    expect(
        std::ranges::find(fake.offeredMimeTypes, "text/plain") != fake.offeredMimeTypes.end(),
        "adopted text selection did not offer the text/plain alias"
    );
  }

  {
    // A payload cut short by the owner dying mid-transfer must not be re-offered.
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[2], "image/png", pngPayload(/*complete=*/false));
    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    expect(fake.claims == 0, "adopted a truncated image payload");
  }

  {
    // Off means off.
    Harness harness;
    harness.clipboard.setKeepFromClosedApps(false);
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[3], "image/png", pngPayload(/*complete=*/true));
    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    expect(fake.claims == 0, "adopted a selection while the setting was disabled");
  }

  {
    // Only the current selection is eligible: a superseded one must not come
    // back when a later owner exits.
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[4], "text/plain;charset=utf-8", bytesOf("first"));
    simulateCopy(harness.clipboard, &offerIds[5], "text/plain;charset=utf-8", bytesOf("second"));
    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    expect(fake.claims == 1, "did not adopt the most recent selection");

    const auto& history = harness.clipboard.history();
    expect(!history.empty(), "history did not record the copies");
    if (!history.empty()) {
      const auto& newest = history.front();
      expect(
          std::string(newest.data.begin(), newest.data.end()) == "second",
          "the adopted selection was not the most recent one"
      );
    }
  }

  {
    // Larger than the old single 10 MiB cap and than any sane text item, but
    // ordinary for a full-screen screenshot: read in full, adopted, and kept in
    // history under the image limit.
    constexpr std::size_t kTwelveMiB = 12U * 1024U * 1024U;
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[6], "image/png", pngPayloadOfSize(kTwelveMiB, /*complete=*/true));

    const auto& history = harness.clipboard.history();
    expect(!history.empty(), "a 12 MiB image was dropped instead of being read in full");
    if (!history.empty()) {
      expect(history.front().byteSize >= kTwelveMiB, "the stored image payload was truncated");
    }

    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    expect(fake.claims == 1, "did not adopt a 12 MiB image after its owner exited");
  }

  {
    // Text gets the much lower limit: too large for history, but still worth
    // keeping alive so it can be pasted.
    constexpr std::size_t kFiveMiB = 5U * 1024U * 1024U;
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[7], "text/plain;charset=utf-8", std::vector<std::uint8_t>(kFiveMiB, 'x'));
    expect(harness.clipboard.history().empty(), "a 5 MiB text item was stored despite the 4 MiB text limit");

    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    expect(fake.claims == 1, "did not adopt an oversized text selection");
  }

  {
    // A transient null selection during handoff must not re-offer stale backup
    // before the replacement offer arrives in the same dispatch batch.
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[0], "text/plain;charset=utf-8", bytesOf("old"));
    harness.clipboard.handleSelection(nullptr);
    simulateCopy(harness.clipboard, &offerIds[1], "text/plain;charset=utf-8", bytesOf("new"));
    expect(fake.claims == 0, "re-offered stale backup during selection handoff");

    const auto& history = harness.clipboard.history();
    expect(!history.empty(), "handoff did not record the new selection");
    if (!history.empty()) {
      expect(
          std::string(history.front().data.begin(), history.front().data.end()) == "new",
          "handoff history kept the previous selection"
      );
    }
  }

  {
    // The adoption queued by a NULL selection has to survive a main loop that
    // dispatches a source only on a ready fd or an advertised timeout: with no
    // transfer in flight the clipboard has no fd to be woken on.
    Harness harness;
    ClipboardPollSource source(harness.clipboard);
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[2], "text/plain;charset=utf-8", bytesOf("orphan"));
    expect(source.pollTimeoutMs() < 0, "poll source asked for a timed wake with nothing pending");

    harness.clipboard.handleSelection(nullptr);
    std::vector<pollfd> fds;
    const std::size_t start = source.addPollFds(fds);
    expect(fds.empty(), "clipboard held a poll fd with no transfer in flight");
    expect(source.pollTimeoutMs() == 0, "poll source did not request an immediate wake for a queued adoption");

    source.dispatch(fds, start);
    expect(fake.claims == 1, "the queued adoption was never flushed");
    expect(source.pollTimeoutMs() < 0, "poll source kept requesting immediate wakes after adopting");
  }

  {
    // Byte pressure must give up images, not the text behind them: the text was
    // copied first, so an oldest-first policy would drop it.
    constexpr std::size_t kImageBytes = 512U * 1024U;
    Harness harness;
    harness.clipboard.setMaxHistoryBytesForTesting(1024U * 1024U);

    simulateCopy(harness.clipboard, &offerIds[0], "text/plain;charset=utf-8", bytesOf("keep me"));
    for (int i = 0; i < 3; ++i) {
      simulateCopy(
          harness.clipboard, &offerIds[1 + i], "image/png",
          pngPayloadOfSize(kImageBytes, true, static_cast<std::uint8_t>(0x10 + i))
      );
    }

    const auto& history = harness.clipboard.history();
    const bool textSurvived = std::ranges::any_of(history, [](const ClipboardEntry& entry) {
      return !entry.isImage() && std::string(entry.data.begin(), entry.data.end()) == "keep me";
    });
    const auto images = std::ranges::count_if(history, [](const ClipboardEntry& entry) { return entry.isImage(); });

    expect(textSurvived, "an image evicted the text entry instead of an older image");
    expect(images == 1, "byte-budget eviction did not drop the oldest images");
  }

  {
    // With no images left to give up, the budget still has to be honoured, and
    // eviction has to terminate.
    Harness harness;
    harness.clipboard.setMaxHistoryBytesForTesting(16);

    for (int i = 0; i < 3; ++i) {
      simulateCopy(
          harness.clipboard, &offerIds[i], "text/plain;charset=utf-8",
          bytesOf(std::string("0123456789abcdefghi") + static_cast<char>('a' + i))
      );
    }
    expect(harness.clipboard.history().empty(), "text-only history ignored the byte budget");
  }

  {
    // clearUnpinnedHistory() is the whole of what a clear offers: pinned entries are
    // the only thing it spares, so with nothing pinned it empties the history and
    // callers need no "clear everything" special case.
    Harness harness;

    for (int i = 0; i < 3; ++i) {
      simulateCopy(
          harness.clipboard, &offerIds[i], "text/plain;charset=utf-8",
          bytesOf(std::string("entry ") + static_cast<char>('a' + i))
      );
    }
    expect(harness.clipboard.history().size() == 3, "three distinct copies did not produce three entries");

    harness.clipboard.clearUnpinnedHistory();
    expect(harness.clipboard.history().empty(), "clearing an unpinned history left entries behind");

    for (int i = 0; i < 3; ++i) {
      simulateCopy(
          harness.clipboard, &offerIds[3 + i], "text/plain;charset=utf-8",
          bytesOf(std::string("entry ") + static_cast<char>('x' + i))
      );
    }
    // Newest first, so "entry z" sits at the front.
    expect(harness.clipboard.setEntryPinned(2, true), "pinning the oldest entry failed");

    harness.clipboard.clearUnpinnedHistory();
    const auto& survivors = harness.clipboard.history();
    expect(survivors.size() == 1, "clearing dropped or kept the wrong number of entries");
    expect(
        !survivors.empty()
            && survivors.front().pinned
            && std::string(survivors.front().data.begin(), survivors.front().data.end()) == "entry x",
        "clearing did not spare the pinned entry"
    );
  }

  gFake = nullptr;
  if (gFailures != 0) {
    std::println(stderr, "clipboard_service_test: {} failure(s)", gFailures);
    return 1;
  }
  return 0;
}
