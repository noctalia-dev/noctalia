// Drives the data-control callbacks with a fake compositor, covering two things
// that are otherwise only observable against a live compositor: what happens to
// the selection when the client owning it exits, and which entries history gives
// up when it runs out of budget.

#include "security/secret_store.h"
#include "security/storage_key_provider.h"
#include "shell/wayland/clipboard_poll_source.h"
#include "shell/wayland/clipboard_service.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

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
    TEST_CHECK(fake.claims == 0);

    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    TEST_CHECK(fake.claims == 1);
    TEST_CHECK(fake.offeredMimeTypes == std::vector<std::string>{"image/png"});
  }

  {
    // Text keeps its aliases, because those are the same bytes.
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[1], "text/plain;charset=utf-8", bytesOf("hello"));
    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    TEST_CHECK(fake.claims == 1);
    TEST_CHECK(std::ranges::find(fake.offeredMimeTypes, "text/plain") != fake.offeredMimeTypes.end());
  }

  {
    // A payload cut short by the owner dying mid-transfer must not be re-offered.
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[2], "image/png", pngPayload(/*complete=*/false));
    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    TEST_CHECK(fake.claims == 0);
  }

  {
    // Off means off.
    Harness harness;
    harness.clipboard.setKeepFromClosedApps(false);
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[3], "image/png", pngPayload(/*complete=*/true));
    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    TEST_CHECK(fake.claims == 0);
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
    TEST_CHECK(fake.claims == 1);

    const auto& history = harness.clipboard.history();
    TEST_CHECK(!history.empty());
    const auto& newest = history.front();
    TEST_CHECK(std::string(newest.data.begin(), newest.data.end()) == "second");
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
    TEST_CHECK(!history.empty());
    TEST_CHECK(history.front().byteSize >= kTwelveMiB);

    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    TEST_CHECK(fake.claims == 1);
  }

  {
    // Text gets the much lower limit: too large for history, but still worth
    // keeping alive so it can be pasted.
    constexpr std::size_t kFiveMiB = 5U * 1024U * 1024U;
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[7], "text/plain;charset=utf-8", std::vector<std::uint8_t>(kFiveMiB, 'x'));
    TEST_CHECK(harness.clipboard.history().empty());

    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    TEST_CHECK(fake.claims == 1);
  }

  {
    // A transient null selection during handoff must not re-offer stale backup
    // before the replacement offer arrives in the same dispatch batch.
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[0], "text/plain;charset=utf-8", bytesOf("old"));
    harness.clipboard.handleSelection(nullptr);
    simulateCopy(harness.clipboard, &offerIds[1], "text/plain;charset=utf-8", bytesOf("new"));
    TEST_CHECK(fake.claims == 0);

    const auto& history = harness.clipboard.history();
    TEST_CHECK(!history.empty());
    TEST_CHECK(std::string(history.front().data.begin(), history.front().data.end()) == "new");
  }

  {
    // The adoption queued by a NULL selection has to survive a main loop that
    // dispatches a source only on a ready fd or an advertised timeout: with no
    // transfer in flight the clipboard has no fd to be woken on.
    Harness harness;
    ClipboardPollSource source(harness.clipboard);
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[2], "text/plain;charset=utf-8", bytesOf("orphan"));
    TEST_CHECK(source.pollTimeoutMs() < 0);

    harness.clipboard.handleSelection(nullptr);
    std::vector<pollfd> fds;
    const std::size_t start = source.addPollFds(fds);
    TEST_CHECK(fds.empty());
    TEST_CHECK(source.pollTimeoutMs() == 0);

    source.dispatch(fds, start);
    TEST_CHECK(fake.claims == 1);
    TEST_CHECK(source.pollTimeoutMs() < 0);
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

    TEST_CHECK(textSurvived);
    TEST_CHECK(images == 1);
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
    TEST_CHECK(harness.clipboard.history().empty());
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
    TEST_CHECK(harness.clipboard.history().size() == 3);

    harness.clipboard.clearUnpinnedHistory();
    TEST_CHECK(harness.clipboard.history().empty());

    for (int i = 0; i < 3; ++i) {
      simulateCopy(
          harness.clipboard, &offerIds[3 + i], "text/plain;charset=utf-8",
          bytesOf(std::string("entry ") + static_cast<char>('x' + i))
      );
    }
    // Newest first, so "entry z" sits at the front.
    TEST_CHECK(harness.clipboard.setEntryPinned(2, true));

    harness.clipboard.clearUnpinnedHistory();
    const auto& survivors = harness.clipboard.history();
    TEST_CHECK(survivors.size() == 1);
    TEST_CHECK(!survivors.empty());
    TEST_CHECK(survivors.front().pinned);
    TEST_CHECK(std::string(survivors.front().data.begin(), survivors.front().data.end()) == "entry x");
  }

  {
    // A password manager marks its copies with a hint (#4457): they must paste
    // into shell fields, yet never reach history or outlive their owner.
    Harness harness;
    fake.claims = 0;
    simulateCopy(harness.clipboard, &offerIds[0], "text/plain;charset=utf-8", bytesOf("old"));

    fake.payload = bytesOf("secret");
    fake.written = 0;
    harness.clipboard.handleDataOffer(&offerIds[1]);
    harness.clipboard.handleOfferMimeType(&offerIds[1], "text/plain;charset=utf-8");
    harness.clipboard.handleOfferMimeType(&offerIds[1], "x-kde-passwordManagerHint");
    harness.clipboard.handleSelection(&offerIds[1]);
    drainReads(harness.clipboard);

    TEST_CHECK(harness.clipboard.clipboardText() == "secret");
    const auto& history = harness.clipboard.history();
    TEST_CHECK(history.size() == 1);
    TEST_CHECK(std::string(history.front().data.begin(), history.front().data.end()) == "old");

    harness.clipboard.handleSelection(nullptr);
    flushDeferredAdopt(harness.clipboard);
    TEST_CHECK(fake.claims == 0);
    TEST_CHECK(!harness.clipboard.clipboardText().has_value());
  }

  {
    // Copying text equal to a pin is deduplicated out of history, but it is
    // still the live selection and has to be what pastes.
    Harness harness;
    simulateCopy(harness.clipboard, &offerIds[0], "text/plain;charset=utf-8", bytesOf("pinned"));
    TEST_CHECK(harness.clipboard.setEntryPinned(0, true));
    simulateCopy(harness.clipboard, &offerIds[1], "text/plain;charset=utf-8", bytesOf("other"));
    simulateCopy(harness.clipboard, &offerIds[2], "text/plain;charset=utf-8", bytesOf("pinned"));
    TEST_CHECK(harness.clipboard.clipboardText() == "pinned");
  }

  {
    // Scripts copy with an explicit type; text still gets the aliases GTK apps paste from.
    Harness harness;
    TEST_CHECK(harness.clipboard.copyText("100.64.0.1", "text/plain"));
    TEST_CHECK(fake.offeredMimeTypes == std::vector<std::string>{"text/plain", "text/plain;charset=utf-8"});

    TEST_CHECK(harness.clipboard.copyText("<b>hi</b>", "text/html"));
    TEST_CHECK(fake.offeredMimeTypes == std::vector<std::string>{"text/html"});
  }

  gFake = nullptr;
  return 0;
}
