#include "wayland/clipboard_service.h"

#include "config/atomic_file.h"
#include "config/config_limits.h"
#include "core/log.h"
#include "ext-data-control-v1-client-protocol.h"
#include "util/file_utils.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <unordered_set>

namespace {

  constexpr std::size_t kMinHistoryMaxEntries = static_cast<std::size_t>(noctalia::config::kClipboardHistoryMinEntries);
  constexpr std::size_t kMaxHistoryMaxEntries = static_cast<std::size_t>(noctalia::config::kClipboardHistoryMaxEntries);
  constexpr std::size_t kMaxHistoryBytes = 64U * 1024U * 1024U;
  constexpr std::size_t kMaxEntryBytes = 10U * 1024U * 1024U;
  constexpr std::size_t kPreviewBytes = 200;

  constexpr std::array kTextMimeTypes = {
      std::string_view{"text/plain;charset=utf-8"},
      std::string_view{"text/plain"},
      std::string_view{"UTF8_STRING"},
  };

  constexpr std::array kImageMimeTypes = {
      std::string_view{"image/png"},
      std::string_view{"image/jpeg"},
  };

  constexpr std::array kPasswordHintMimeTypes = {
      std::string_view{"x-kde-passwordManagerHint"},
  };

  [[nodiscard]] bool selectionAdvertisesPasswordHint(const std::vector<std::string>& mimeTypes) {
    for (const std::string& advertised : mimeTypes) {
      for (const std::string_view hint : kPasswordHintMimeTypes) {
        if (advertised == hint) {
          return true;
        }
      }
    }
    return false;
  }

  constexpr Logger kLog("clipboard");
  std::uint64_t gStorageCounter = 0;

  void secureFile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!FileUtils::setPrivateFilePermissions(path, ec)) {
      kLog.warn("failed to secure clipboard storage path {}: {}", path.string(), ec.message());
    }
  }

  void secureDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    if (!FileUtils::setPrivateDirectoryPermissions(path, ec)) {
      kLog.warn("failed to secure clipboard storage path {}: {}", path.string(), ec.message());
    }
  }

  void secureClipboardStorage(const std::filesystem::path& root) {
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(root, ec)) {
      if (ec) {
        kLog.warn("failed to inspect clipboard storage {}: {}", root.string(), ec.message());
      }
      return;
    }

    secureDirectory(root);
    fs::recursive_directory_iterator current(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && current != end) {
      const fs::file_status status = current->status(ec);
      if (ec) {
        break;
      }
      if (fs::is_directory(status)) {
        secureDirectory(current->path());
      } else if (fs::is_regular_file(status)) {
        secureFile(current->path());
      }
      current.increment(ec);
    }
    if (ec) {
      kLog.warn("failed to inspect clipboard storage {}: {}", root.string(), ec.message());
    }
  }

  void ensurePrivateDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    if (!FileUtils::createPrivateDirectories(path, ec)) {
      throw std::filesystem::filesystem_error("failed to create private clipboard directory", path, ec);
    }
  }

  std::string_view binaryView(const std::vector<std::uint8_t>& data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
  }

  std::string extensionForImageMimeType(std::string_view mimeType) {
    if (mimeType == "image/png")
      return ".png";
    if (mimeType == "image/jpeg" || mimeType == "image/jpg")
      return ".jpg";
    if (mimeType == "image/webp")
      return ".webp";
    if (mimeType == "image/gif")
      return ".gif";
    if (mimeType == "image/bmp")
      return ".bmp";
    if (mimeType == "image/tiff")
      return ".tiff";
    if (mimeType == "image/svg+xml")
      return ".svg";
    if (mimeType == "image/avif")
      return ".avif";
    if (mimeType == "image/heic")
      return ".heic";
    return ".img";
  }

  std::string exportExtensionForEntry(const ClipboardEntry& entry) {
    std::string extension = extensionForImageMimeType(entry.dataMimeType);
    if (extension != ".img") {
      return extension;
    }

    for (const auto& mimeType : entry.mimeTypes) {
      extension = extensionForImageMimeType(mimeType);
      if (extension != ".img") {
        return extension;
      }
    }
    return extension;
  }

  void closeFd(int& fd) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }

  void* bindExtManager(wl_registry* registry, std::uint32_t name, std::uint32_t version) {
    const auto bindVersion = std::min(version, 1U);
    return wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, bindVersion);
  }

  void destroyExtManager(void* manager) {
    ext_data_control_manager_v1_destroy(static_cast<ext_data_control_manager_v1*>(manager));
  }

  void* getExtDataDevice(void* manager, wl_seat* seat) {
    return ext_data_control_manager_v1_get_data_device(static_cast<ext_data_control_manager_v1*>(manager), seat);
  }

  void destroyExtDevice(void* device) {
    ext_data_control_device_v1_destroy(static_cast<ext_data_control_device_v1*>(device));
  }

  int addExtDeviceListener(void* device, const void* listener, void* data) {
    return ext_data_control_device_v1_add_listener(
        static_cast<ext_data_control_device_v1*>(device),
        static_cast<const ext_data_control_device_v1_listener*>(listener), data
    );
  }

  void* createExtDataSource(void* manager) {
    return ext_data_control_manager_v1_create_data_source(static_cast<ext_data_control_manager_v1*>(manager));
  }

  void destroyExtSource(void* source) {
    ext_data_control_source_v1_destroy(static_cast<ext_data_control_source_v1*>(source));
  }

  int addExtSourceListener(void* source, const void* listener, void* data) {
    return ext_data_control_source_v1_add_listener(
        static_cast<ext_data_control_source_v1*>(source),
        static_cast<const ext_data_control_source_v1_listener*>(listener), data
    );
  }

  void extSourceOffer(void* source, const char* mimeType) {
    ext_data_control_source_v1_offer(static_cast<ext_data_control_source_v1*>(source), mimeType);
  }

  void extDeviceSetSelection(void* device, void* source) {
    ext_data_control_device_v1_set_selection(
        static_cast<ext_data_control_device_v1*>(device), static_cast<ext_data_control_source_v1*>(source)
    );
  }

  void destroyExtOffer(void* offer) {
    ext_data_control_offer_v1_destroy(static_cast<ext_data_control_offer_v1*>(offer));
  }

  int addExtOfferListener(void* offer, const void* listener, void* data) {
    return ext_data_control_offer_v1_add_listener(
        static_cast<ext_data_control_offer_v1*>(offer),
        static_cast<const ext_data_control_offer_v1_listener*>(listener), data
    );
  }

  void extOfferReceive(void* offer, const char* mimeType, int fd) {
    ext_data_control_offer_v1_receive(static_cast<ext_data_control_offer_v1*>(offer), mimeType, fd);
  }

  void* bindWlrManager(wl_registry* registry, std::uint32_t name, std::uint32_t version) {
    const auto bindVersion = std::min(version, 2U);
    return wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, bindVersion);
  }

  void destroyWlrManager(void* manager) {
    zwlr_data_control_manager_v1_destroy(static_cast<zwlr_data_control_manager_v1*>(manager));
  }

  void* getWlrDataDevice(void* manager, wl_seat* seat) {
    return zwlr_data_control_manager_v1_get_data_device(static_cast<zwlr_data_control_manager_v1*>(manager), seat);
  }

  void destroyWlrDevice(void* device) {
    zwlr_data_control_device_v1_destroy(static_cast<zwlr_data_control_device_v1*>(device));
  }

  int addWlrDeviceListener(void* device, const void* listener, void* data) {
    return zwlr_data_control_device_v1_add_listener(
        static_cast<zwlr_data_control_device_v1*>(device),
        static_cast<const zwlr_data_control_device_v1_listener*>(listener), data
    );
  }

  void* createWlrDataSource(void* manager) {
    return zwlr_data_control_manager_v1_create_data_source(static_cast<zwlr_data_control_manager_v1*>(manager));
  }

  void destroyWlrSource(void* source) {
    zwlr_data_control_source_v1_destroy(static_cast<zwlr_data_control_source_v1*>(source));
  }

  int addWlrSourceListener(void* source, const void* listener, void* data) {
    return zwlr_data_control_source_v1_add_listener(
        static_cast<zwlr_data_control_source_v1*>(source),
        static_cast<const zwlr_data_control_source_v1_listener*>(listener), data
    );
  }

  void wlrSourceOffer(void* source, const char* mimeType) {
    zwlr_data_control_source_v1_offer(static_cast<zwlr_data_control_source_v1*>(source), mimeType);
  }

  void wlrDeviceSetSelection(void* device, void* source) {
    zwlr_data_control_device_v1_set_selection(
        static_cast<zwlr_data_control_device_v1*>(device), static_cast<zwlr_data_control_source_v1*>(source)
    );
  }

  void destroyWlrOffer(void* offer) {
    zwlr_data_control_offer_v1_destroy(static_cast<zwlr_data_control_offer_v1*>(offer));
  }

  int addWlrOfferListener(void* offer, const void* listener, void* data) {
    return zwlr_data_control_offer_v1_add_listener(
        static_cast<zwlr_data_control_offer_v1*>(offer),
        static_cast<const zwlr_data_control_offer_v1_listener*>(listener), data
    );
  }

  void wlrOfferReceive(void* offer, const char* mimeType, int fd) {
    zwlr_data_control_offer_v1_receive(static_cast<zwlr_data_control_offer_v1*>(offer), mimeType, fd);
  }

  const DataControlOps kExtDataControlOps = {
      .managerInterfaceName = ext_data_control_manager_v1_interface.name,
      .bindManager = &bindExtManager,
      .destroyManager = &destroyExtManager,
      .getDataDevice = &getExtDataDevice,
      .destroyDevice = &destroyExtDevice,
      .addDeviceListener = &addExtDeviceListener,
      .createDataSource = &createExtDataSource,
      .destroySource = &destroyExtSource,
      .addSourceListener = &addExtSourceListener,
      .sourceOffer = &extSourceOffer,
      .deviceSetSelection = &extDeviceSetSelection,
      .destroyOffer = &destroyExtOffer,
      .addOfferListener = &addExtOfferListener,
      .offerReceive = &extOfferReceive,
  };

  const DataControlOps kWlrDataControlOps = {
      .managerInterfaceName = zwlr_data_control_manager_v1_interface.name,
      .bindManager = &bindWlrManager,
      .destroyManager = &destroyWlrManager,
      .getDataDevice = &getWlrDataDevice,
      .destroyDevice = &destroyWlrDevice,
      .addDeviceListener = &addWlrDeviceListener,
      .createDataSource = &createWlrDataSource,
      .destroySource = &destroyWlrSource,
      .addSourceListener = &addWlrSourceListener,
      .sourceOffer = &wlrSourceOffer,
      .deviceSetSelection = &wlrDeviceSetSelection,
      .destroyOffer = &destroyWlrOffer,
      .addOfferListener = &addWlrOfferListener,
      .offerReceive = &wlrOfferReceive,
  };

  void handleExtDataOffer(void* data, ext_data_control_device_v1* /*device*/, ext_data_control_offer_v1* offer) {
    static_cast<ClipboardService*>(data)->handleDataOffer(offer);
  }

  void handleExtSelection(void* data, ext_data_control_device_v1* /*device*/, ext_data_control_offer_v1* offer) {
    static_cast<ClipboardService*>(data)->handleSelection(offer);
  }

  void handleExtFinished(void* data, ext_data_control_device_v1* /*device*/) {
    static_cast<ClipboardService*>(data)->handleDeviceFinished();
  }

  void handleExtPrimarySelection(void* data, ext_data_control_device_v1* /*device*/, ext_data_control_offer_v1* offer) {
    static_cast<ClipboardService*>(data)->handlePrimarySelection(offer);
  }

  const ext_data_control_device_v1_listener kExtDeviceListener = {
      .data_offer = &handleExtDataOffer,
      .selection = &handleExtSelection,
      .finished = &handleExtFinished,
      .primary_selection = &handleExtPrimarySelection,
  };

  void handleExtOfferMimeType(void* data, ext_data_control_offer_v1* offer, const char* mimeType) {
    static_cast<ClipboardService*>(data)->handleOfferMimeType(offer, mimeType);
  }

  const ext_data_control_offer_v1_listener kExtOfferListener = {
      .offer = &handleExtOfferMimeType,
  };

  void handleExtSourceSend(void* data, ext_data_control_source_v1* source, const char* mimeType, int fd) {
    static_cast<ClipboardService*>(data)->handleSourceSend(source, mimeType, fd);
  }

  void handleExtSourceCancelled(void* data, ext_data_control_source_v1* source) {
    static_cast<ClipboardService*>(data)->handleSourceCancelled(source);
  }

  const ext_data_control_source_v1_listener kExtSourceListener = {
      .send = &handleExtSourceSend,
      .cancelled = &handleExtSourceCancelled,
  };

  void handleWlrDataOffer(void* data, zwlr_data_control_device_v1* /*device*/, zwlr_data_control_offer_v1* offer) {
    static_cast<ClipboardService*>(data)->handleDataOffer(offer);
  }

  void handleWlrSelection(void* data, zwlr_data_control_device_v1* /*device*/, zwlr_data_control_offer_v1* offer) {
    static_cast<ClipboardService*>(data)->handleSelection(offer);
  }

  void handleWlrFinished(void* data, zwlr_data_control_device_v1* /*device*/) {
    static_cast<ClipboardService*>(data)->handleDeviceFinished();
  }

  void
  handleWlrPrimarySelection(void* data, zwlr_data_control_device_v1* /*device*/, zwlr_data_control_offer_v1* offer) {
    static_cast<ClipboardService*>(data)->handlePrimarySelection(offer);
  }

  const zwlr_data_control_device_v1_listener kWlrDeviceListener = {
      .data_offer = &handleWlrDataOffer,
      .selection = &handleWlrSelection,
      .finished = &handleWlrFinished,
      .primary_selection = &handleWlrPrimarySelection,
  };

  void handleWlrOfferMimeType(void* data, zwlr_data_control_offer_v1* offer, const char* mimeType) {
    static_cast<ClipboardService*>(data)->handleOfferMimeType(offer, mimeType);
  }

  const zwlr_data_control_offer_v1_listener kWlrOfferListener = {
      .offer = &handleWlrOfferMimeType,
  };

  void handleWlrSourceSend(void* data, zwlr_data_control_source_v1* source, const char* mimeType, int fd) {
    static_cast<ClipboardService*>(data)->handleSourceSend(source, mimeType, fd);
  }

  void handleWlrSourceCancelled(void* data, zwlr_data_control_source_v1* source) {
    static_cast<ClipboardService*>(data)->handleSourceCancelled(source);
  }

  const zwlr_data_control_source_v1_listener kWlrSourceListener = {
      .send = &handleWlrSourceSend,
      .cancelled = &handleWlrSourceCancelled,
  };

  const void* deviceListenerFor(const DataControlOps& ops) {
    return &ops == &kExtDataControlOps ? static_cast<const void*>(&kExtDeviceListener)
                                       : static_cast<const void*>(&kWlrDeviceListener);
  }

  const void* offerListenerFor(const DataControlOps& ops) {
    return &ops == &kExtDataControlOps ? static_cast<const void*>(&kExtOfferListener)
                                       : static_cast<const void*>(&kWlrOfferListener);
  }

  const void* sourceListenerFor(const DataControlOps& ops) {
    return &ops == &kExtDataControlOps ? static_cast<const void*>(&kExtSourceListener)
                                       : static_cast<const void*>(&kWlrSourceListener);
  }

} // namespace

ClipboardService::ClipboardService() { loadPersistedHistory(); }

void ClipboardService::setHistoryRetentionEnabled(bool enabled) {
  if (m_historyRetention == enabled) {
    return;
  }
  m_historyRetention = enabled;

  if (enabled) {
    // Restore the persisted history that was set aside while retention was off.
    loadPersistedHistory();
  } else {
    // Keep only the live selection (most recent unpinned entry) in memory so
    // paste keeps working; drop pins and older/persisted history.
    std::deque<ClipboardEntry> live;
    for (auto& entry : m_history) {
      if (!entry.pinned) {
        live.push_back(std::move(entry));
        break;
      }
    }
    m_history = std::move(live);
    m_historyBytes = m_history.empty() ? 0 : m_history.front().byteSize;
  }

  ++m_changeSerial;
  notifyChanged();
}

void ClipboardService::setMaxHistoryEntries(std::size_t maxEntries) {
  maxEntries = std::clamp(maxEntries, kMinHistoryMaxEntries, kMaxHistoryMaxEntries);
  if (m_maxHistoryEntries == maxEntries) {
    return;
  }
  m_maxHistoryEntries = maxEntries;
  if (!m_historyRetention) {
    return;
  }
  trimHistoryToBudget();
  ++m_changeSerial;
  notifyChanged();
}

ClipboardService::~ClipboardService() { cleanup(); }

const DataControlOps* extDataControlOps() { return &kExtDataControlOps; }

const DataControlOps* wlrDataControlOps() { return &kWlrDataControlOps; }

bool ClipboardEntry::isImage() const {
  return std::ranges::any_of(mimeTypes, [](const std::string& mimeType) { return mimeType.starts_with("image/"); });
}

bool ClipboardService::bind(void* manager, const DataControlOps* ops, wl_seat* seat) {
  if (manager == nullptr || ops == nullptr || seat == nullptr) {
    cleanup();
    return false;
  }

  if (m_manager == manager && m_ops == ops && m_seat == seat && m_device != nullptr) {
    return true;
  }

  cleanup();

  m_manager = manager;
  m_ops = ops;
  m_seat = seat;
  m_device = m_ops->getDataDevice(m_manager, m_seat);
  if (m_device == nullptr) {
    kLog.warn("failed to create data control device");
    return false;
  }

  if (m_ops->addDeviceListener(m_device, deviceListenerFor(*m_ops), this) != 0) {
    kLog.warn("failed to attach clipboard device listener");
    cleanup();
    return false;
  }

  kLog.info("clipboard service bound via {}", m_ops->managerInterfaceName);
  return true;
}

void ClipboardService::cleanup() {
  cancelActiveRead();
  cancelActiveWrites();
  clearOffers();

  if (m_ops != nullptr) {
    for (auto& outgoing : m_outgoingSources) {
      if (outgoing.source != nullptr) {
        m_ops->destroySource(outgoing.source);
      }
    }
  }
  m_outgoingSources.clear();

  if (m_device != nullptr && m_ops != nullptr) {
    m_ops->destroyDevice(m_device);
  }

  m_device = nullptr;
  m_selectionOffer = nullptr;
  m_seat = nullptr;
  m_ops = nullptr;
  m_manager = nullptr;
}

bool ClipboardService::isAvailable() const noexcept { return m_device != nullptr; }

const std::deque<ClipboardEntry>& ClipboardService::history() const noexcept { return m_history; }

std::uint64_t ClipboardService::changeSerial() const noexcept { return m_changeSerial; }

std::size_t ClipboardService::addPollFds(std::vector<pollfd>& fds) const {
  const std::size_t start = fds.size();
  if (m_activeRead.fd >= 0) {
    fds.push_back({.fd = m_activeRead.fd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0});
  }
  for (const auto& write : m_activeWrites) {
    if (write.fd >= 0) {
      fds.push_back({.fd = write.fd, .events = POLLOUT | POLLHUP | POLLERR, .revents = 0});
    }
  }
  return fds.size() - start;
}

bool ClipboardService::ensureEntryLoaded(std::size_t index) {
  if (index >= m_history.size()) {
    return false;
  }
  return loadEntryPayload(m_history[index]);
}

std::optional<std::string> ClipboardService::exportEntryForExternalTool(std::size_t index) {
  namespace fs = std::filesystem;

  if (index >= m_history.size()) {
    return std::nullopt;
  }

  ClipboardEntry& entry = m_history[index];
  if (!entry.isImage()) {
    return std::nullopt;
  }

  const bool wasLoaded = entry.payloadLoaded;
  if (!loadEntryPayload(entry)) {
    return std::nullopt;
  }

  try {
    if (entry.storageId.empty()) {
      entry.storageId = generateStorageId();
    }

    const fs::path exportDir = fs::path(stateDirectory()) / "exports";
    ensurePrivateDirectory(stateDirectory());
    ensurePrivateDirectory(exportDir);
    const fs::path exportPath = exportDir / (entry.storageId + exportExtensionForEntry(entry));

    if (!writeTextFileAtomic(exportPath, binaryView(entry.data), FileUtils::privateFileMode())) {
      throw std::runtime_error("failed to write exported clipboard image");
    }

    if (!wasLoaded) {
      evictPayloadData(entry);
    }
    return exportPath.string();
  } catch (const std::exception& e) {
    if (!wasLoaded) {
      evictPayloadData(entry);
    }
    kLog.warn("failed to export clipboard image: {}", e.what());
    return std::nullopt;
  }
}

void ClipboardService::evictEntryPayload(std::size_t index) {
  if (index >= m_history.size()) {
    return;
  }
  evictPayloadData(m_history[index]);
}

void ClipboardService::evictAllPayloads() {
  for (std::size_t i = 0; i < m_history.size(); ++i) {
    evictEntryPayload(i);
  }
}

std::optional<std::string> ClipboardService::clipboardText() {
  for (auto& entry : m_history) {
    // Pinned entries sit at the front but are not the newest capture; paste
    // must reflect the most recent real clipboard content.
    if (entry.pinned || entry.isImage()) {
      continue;
    }
    if (!loadEntryPayload(entry) || entry.data.empty()) {
      continue;
    }
    return std::string(entry.data.begin(), entry.data.end());
  }
  return std::nullopt;
}

void ClipboardService::setClipboardText(std::string text) { (void)copyText(std::move(text)); }

bool ClipboardService::copyText(std::string text) {
  std::vector<std::uint8_t> data(text.begin(), text.end());
  return copyData({"text/plain;charset=utf-8", "text/plain"}, std::move(data));
}

bool ClipboardService::copyText(std::string text, std::string mimeType) {
  std::vector<std::uint8_t> data(text.begin(), text.end());
  return copyData({std::move(mimeType)}, std::move(data));
}

bool ClipboardService::copyImagePng(std::vector<std::uint8_t> png) {
  if (png.empty()) {
    return false;
  }
  return copyData({"image/png"}, std::move(png));
}

bool ClipboardService::copyEntry(const ClipboardEntry& entry) {
  if (entry.data.empty() || entry.dataMimeType.empty()) {
    return false;
  }

  std::vector<std::string> mimeTypes;
  mimeTypes.push_back(entry.dataMimeType);
  if (isTextMimeType(entry.dataMimeType)) {
    if (!std::ranges::contains(mimeTypes, "text/plain;charset=utf-8")) {
      mimeTypes.emplace_back("text/plain;charset=utf-8");
    }
    if (!std::ranges::contains(mimeTypes, "text/plain")) {
      mimeTypes.emplace_back("text/plain");
    }
  }
  return copyData(std::move(mimeTypes), entry.data);
}

std::size_t ClipboardService::pinnedCount() const noexcept {
  std::size_t count = 0;
  while (count < m_history.size() && m_history[count].pinned) {
    ++count;
  }
  return count;
}

bool ClipboardService::promoteEntry(std::size_t index) {
  if (index >= m_history.size()) {
    return false;
  }
  // Pinned entries keep their position in the pinned block; copying one must
  // not reorder the user's pins.
  if (m_history[index].pinned) {
    return true;
  }

  // Unpinned entries move to the top of the unpinned region (just below the
  // contiguous pinned block at the front).
  const std::size_t target = pinnedCount();
  if (index == target) {
    return true;
  }

  ClipboardEntry entry = std::move(m_history[index]);
  m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(index));
  entry.capturedAt = std::chrono::system_clock::now();
  entry.timestamp = std::chrono::steady_clock::now();
  m_history.insert(m_history.begin() + static_cast<std::ptrdiff_t>(target), std::move(entry));
  ++m_changeSerial;
  persistHistory();
  notifyChanged();
  return true;
}

bool ClipboardService::setEntryPinned(std::size_t index, bool pinned) {
  if (index >= m_history.size()) {
    return false;
  }
  if (m_history[index].pinned == pinned) {
    return true;
  }

  ClipboardEntry entry = std::move(m_history[index]);
  m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(index));
  entry.pinned = pinned;

  if (pinned) {
    // Most-recently-pinned first: new pins go to the very front.
    m_history.push_front(std::move(entry));
  } else {
    // Unpinned entries return to the top of the unpinned region, just below
    // the remaining pinned block.
    const std::size_t target = pinnedCount();
    m_history.insert(m_history.begin() + static_cast<std::ptrdiff_t>(target), std::move(entry));
  }

  ++m_changeSerial;
  persistHistory();
  notifyChanged();
  return true;
}

bool ClipboardService::removeHistoryEntry(std::size_t index) {
  if (index >= m_history.size()) {
    return false;
  }
  const std::size_t removedBytes = m_history[index].byteSize;
  m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(index));
  if (m_historyBytes >= removedBytes) {
    m_historyBytes -= removedBytes;
  } else {
    m_historyBytes = 0;
  }
  ++m_changeSerial;
  persistHistory();
  notifyChanged();
  return true;
}

void ClipboardService::clearUnpinnedHistory() {
  const std::size_t firstUnpinned = pinnedCount();
  if (firstUnpinned >= m_history.size()) {
    return;
  }

  std::size_t removedBytes = 0;
  for (std::size_t i = firstUnpinned; i < m_history.size(); ++i) {
    removedBytes += m_history[i].byteSize;
  }
  m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(firstUnpinned), m_history.end());
  if (m_historyBytes >= removedBytes) {
    m_historyBytes -= removedBytes;
  } else {
    m_historyBytes = 0;
  }
  ++m_changeSerial;
  persistHistory();
  notifyChanged();
}

void ClipboardService::clearHistory() {
  if (m_history.empty()) {
    return;
  }
  m_history.clear();
  m_historyBytes = 0;
  ++m_changeSerial;
  persistHistory();
  notifyChanged();
}

bool ClipboardService::copyData(std::vector<std::string> mimeTypes, std::vector<std::uint8_t> data) {
  if (m_device == nullptr || m_ops == nullptr) {
    return false;
  }
  if (mimeTypes.empty() || data.empty()) {
    return false;
  }

  void* source = m_ops->createDataSource(m_manager);
  if (source == nullptr) {
    return false;
  }

  if (m_ops->addSourceListener(source, sourceListenerFor(*m_ops), this) != 0) {
    m_ops->destroySource(source);
    return false;
  }

  for (const auto& mimeType : mimeTypes) {
    m_ops->sourceOffer(source, mimeType.c_str());
  }
  auto payload = std::make_shared<std::vector<std::uint8_t>>(std::move(data));
  m_outgoingSources.push_back(
      OutgoingSource{
          .source = source,
          .mimeTypes = std::move(mimeTypes),
          .data = std::move(payload),
      }
  );
  m_ops->deviceSetSelection(m_device, source);
  return true;
}

void ClipboardService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void ClipboardService::dispatchReadEvents(short revents) {
  if (m_activeRead.fd < 0) {
    return;
  }

  if ((revents & POLLERR) != 0) {
    finishRead(true);
    return;
  }

  std::array<std::uint8_t, 16384> buffer{};
  for (;;) {
    const ssize_t bytesRead = read(m_activeRead.fd, buffer.data(), buffer.size());
    if (bytesRead > 0) {
      const auto nextSize = m_activeRead.buffer.size() + static_cast<std::size_t>(bytesRead);
      if (nextSize > kMaxEntryBytes) {
        kLog.warn("discarding oversized clipboard entry");
        finishRead(true);
        return;
      }
      m_activeRead.buffer.insert(m_activeRead.buffer.end(), buffer.begin(), buffer.begin() + bytesRead);
      continue;
    }

    if (bytesRead == 0) {
      finishRead(false);
      return;
    }

    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }

    finishRead(true);
    return;
  }

  if ((revents & POLLHUP) != 0) {
    finishRead(false);
  }
}

void ClipboardService::dispatchPollEvents(const std::vector<pollfd>& fds, std::size_t startIdx, std::size_t count) {
  const std::size_t end = std::min(fds.size(), startIdx + count);
  for (std::size_t i = startIdx; i < end; ++i) {
    if (fds[i].revents == 0) {
      continue;
    }
    if (fds[i].fd == m_activeRead.fd) {
      dispatchReadEvents(fds[i].revents);
    }
  }

  for (std::size_t i = startIdx; i < end; ++i) {
    if (fds[i].revents == 0) {
      continue;
    }
    dispatchWriteEvents(fds[i].fd, fds[i].revents);
  }
}

void ClipboardService::handleDataOffer(void* offer) {
  if (offer == nullptr || m_ops == nullptr) {
    return;
  }

  m_offers.push_back(
      OfferState{
          .offer = offer,
          .mimeTypes = {},
      }
  );
  if (m_ops->addOfferListener(offer, offerListenerFor(*m_ops), this) != 0) {
    kLog.warn("failed to attach clipboard offer listener");
  }
}

void ClipboardService::handleOfferMimeType(void* offer, const char* mimeType) {
  auto* state = findOffer(offer);
  if (state == nullptr || mimeType == nullptr) {
    return;
  }
  state->mimeTypes.emplace_back(mimeType);
}

void ClipboardService::handleSelection(void* offer) {
  cancelActiveRead();
  destroyOffer(m_selectionOffer);
  m_selectionOffer = offer;

  if (offer == nullptr) {
    return;
  }

  if (!startReceive(offer)) {
    kLog.debug("selection offer has no supported MIME types");
  }
}

void ClipboardService::handlePrimarySelection(void* offer) { destroyOffer(offer); }

void ClipboardService::handleDeviceFinished() { cleanup(); }

void ClipboardService::handleSourceSend(void* source, const char* mimeType, int fd) {
  const auto it = std::ranges::find(m_outgoingSources, source, &OutgoingSource::source);
  if (it != m_outgoingSources.end() && mimeType != nullptr) {
    const auto mimeIt = std::ranges::find(it->mimeTypes, std::string_view(mimeType));
    if (mimeIt != it->mimeTypes.end()) {
      if (queueOutgoingWrite(source, fd, it->data)) {
        return;
      }
    }
  }
  if (fd >= 0) {
    close(fd);
  }
}

void ClipboardService::handleSourceCancelled(void* source) {
  const auto it = std::ranges::find(m_outgoingSources, source, &OutgoingSource::source);
  if (it == m_outgoingSources.end()) {
    return;
  }

  if (m_ops != nullptr && it->source != nullptr) {
    m_ops->destroySource(it->source);
  }
  for (auto& write : m_activeWrites) {
    if (write.source == source) {
      write.source = nullptr;
    }
  }
  m_outgoingSources.erase(it);
}

const ClipboardService::OfferState* ClipboardService::findOffer(void* offer) const {
  const auto it = std::ranges::find(m_offers, offer, &OfferState::offer);
  return it != m_offers.end() ? &*it : nullptr;
}

ClipboardService::OfferState* ClipboardService::findOffer(void* offer) {
  const auto it = std::ranges::find(m_offers, offer, &OfferState::offer);
  return it != m_offers.end() ? &*it : nullptr;
}

void ClipboardService::destroyOffer(void* offer) {
  if (offer == nullptr || m_ops == nullptr) {
    return;
  }

  const auto it = std::ranges::find(m_offers, offer, &OfferState::offer);
  if (it == m_offers.end()) {
    return;
  }

  m_ops->destroyOffer(offer);
  m_offers.erase(it);
}

void ClipboardService::clearOffers() {
  if (m_ops != nullptr) {
    for (auto& offer : m_offers) {
      if (offer.offer != nullptr) {
        m_ops->destroyOffer(offer.offer);
      }
    }
  }
  m_offers.clear();
}

void ClipboardService::cancelActiveRead() {
  closeFd(m_activeRead.fd);
  m_activeRead.buffer.clear();
  m_activeRead.mimeType.clear();
  m_activeRead.offeredMimeTypes.clear();
  m_activeRead.offer = nullptr;
}

void ClipboardService::cancelActiveWrites() {
  for (auto& write : m_activeWrites) {
    closeFd(write.fd);
  }
  m_activeWrites.clear();
}

bool ClipboardService::startReceive(void* offer) {
  if (m_ops == nullptr) {
    return false;
  }

  const OfferState* state = findOffer(offer);
  if (state == nullptr) {
    return false;
  }

  if (selectionAdvertisesPasswordHint(state->mimeTypes)) {
    kLog.debug("ignoring clipboard selection: password-hint MIME advertised");
    return false;
  }

  const std::string mimeType = chooseMimeType(*state);
  if (mimeType.empty()) {
    return false;
  }

  int pipeFds[2] = {-1, -1};
  if (pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK) != 0) {
    return false;
  }

  m_ops->offerReceive(offer, mimeType.c_str(), pipeFds[1]);
  close(pipeFds[1]);

  m_activeRead.fd = pipeFds[0];
  m_activeRead.offer = offer;
  m_activeRead.mimeType = mimeType;
  m_activeRead.buffer.clear();
  m_activeRead.offeredMimeTypes = state->mimeTypes;
  return true;
}

void ClipboardService::finishRead(bool discard) {
  const bool shouldStore = !discard && !m_activeRead.buffer.empty();
  const std::string mimeType = m_activeRead.mimeType;
  auto mimeTypes = std::move(m_activeRead.offeredMimeTypes);
  auto data = std::move(m_activeRead.buffer);
  const void* offer = m_activeRead.offer;

  cancelActiveRead();

  if (offer == m_selectionOffer) {
    destroyOffer(m_selectionOffer);
    m_selectionOffer = nullptr;
  }

  if (!shouldStore) {
    return;
  }

  ClipboardEntry entry;
  entry.storageId = generateStorageId();
  entry.mimeTypes = std::move(mimeTypes);
  if (!std::ranges::contains(entry.mimeTypes, mimeType)) {
    entry.mimeTypes.push_back(mimeType);
  }
  entry.dataMimeType = mimeType;
  entry.data = std::move(data);
  entry.byteSize = entry.data.size();
  entry.payloadLoaded = true;
  entry.payloadPath = payloadPathForId(entry.storageId);
  entry.capturedAt = std::chrono::system_clock::now();
  entry.timestamp = std::chrono::steady_clock::now();
  if (isTextMimeType(mimeType)) {
    entry.textPreview = buildTextPreview(entry.data);
  }

  addToHistory(std::move(entry));
}

void ClipboardService::addToHistory(ClipboardEntry entry) {
  if (entry.byteSize == 0) {
    entry.byteSize = entry.data.size();
  }

  if (entry.byteSize == 0) {
    return;
  }

  if (!entry.data.empty() && isTextMimeType(entry.dataMimeType) && isEmptyTextPayload(entry.data)) {
    return;
  }

  if (entry.storageId.empty()) {
    entry.storageId = generateStorageId();
  }
  if (entry.payloadPath.empty()) {
    entry.payloadPath = payloadPathForId(entry.storageId);
  }

  if (entry.textPreview.empty() && !entry.data.empty() && isTextMimeType(entry.dataMimeType)) {
    entry.textPreview = buildTextPreview(entry.data);
  }

  if (!m_historyRetention) {
    // History is disabled: retain only the live selection in memory (so paste
    // still works) and never persist. Ignore the self-copy echo of unchanged
    // content to avoid needless churn.
    if (!m_history.empty()
        && !entry.data.empty()
        && m_history.front().byteSize == entry.byteSize
        && m_history.front().data == entry.data
        && m_history.front().dataMimeType == entry.dataMimeType) {
      return;
    }
    m_history.clear();
    m_historyBytes = entry.byteSize;
    m_history.push_back(std::move(entry));
    ++m_changeSerial;
    notifyChanged();
    return;
  }

  // Pinned entries form a contiguous block at the front; new captures join the
  // top of the unpinned region just below it.
  const std::size_t insertAt = pinnedCount();

  // Dedup against the most recent capture (the first unpinned entry) and
  // against every pinned entry. The latter matters because copying/actioning a
  // pinned entry echoes its content back as a fresh selection, which would
  // otherwise reappear as a duplicate unpinned entry at the top.
  if (!entry.data.empty()) {
    auto matchesExisting = [&](ClipboardEntry& current) {
      const bool currentWasLoaded = current.payloadLoaded;
      bool samePayload = false;
      if (current.byteSize == entry.byteSize) {
        const bool currentLoaded = current.payloadLoaded || loadEntryPayload(current);
        samePayload = currentLoaded && current.data == entry.data;
        if (!currentWasLoaded) {
          evictPayloadData(current);
        }
      }
      const bool sameMime = current.dataMimeType == entry.dataMimeType;
      const bool equivalentText = isTextMimeType(current.dataMimeType) && isTextMimeType(entry.dataMimeType);
      return samePayload && (sameMime || equivalentText);
    };

    for (std::size_t i = 0; i < insertAt && i < m_history.size(); ++i) {
      if (matchesExisting(m_history[i])) {
        return;
      }
    }
    if (insertAt < m_history.size() && matchesExisting(m_history[insertAt])) {
      return;
    }
  }

  const std::size_t entryBytes = entry.byteSize;
  if (entryBytes > kMaxEntryBytes) {
    return;
  }

  m_history.insert(m_history.begin() + static_cast<std::ptrdiff_t>(insertAt), std::move(entry));
  m_historyBytes += entryBytes;
  trimHistoryToBudget();

  ++m_changeSerial;
  if (persistHistory()) {
    evictPayloadData(m_history[insertAt]);
  }
  const std::string latestMime = m_history[insertAt].mimeTypes.empty() ? "" : m_history[insertAt].mimeTypes.front();
  kLog.debug("clipboard history size={} entries={} latest_mime={}", m_historyBytes, m_history.size(), latestMime);
  notifyChanged();
}

void ClipboardService::loadPersistedHistory() {
  namespace fs = std::filesystem;

  m_history.clear();
  m_historyBytes = 0;

  secureClipboardStorage(stateDirectory());

  const fs::path path(manifestPath());
  if (!fs::exists(path)) {
    return;
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }

  try {
    nlohmann::json json;
    file >> json;
    if (!json.is_object() || !json["entries"].is_array()) {
      return;
    }

    for (const auto& item : json["entries"]) {
      ClipboardEntry entry;
      entry.storageId = item.value("id", "");
      entry.payloadPath = item.value("payload_path", payloadPathForId(entry.storageId));
      entry.mimeTypes = item.value("mime_types", std::vector<std::string>{});
      entry.dataMimeType = item.value("data_mime_type", "");
      entry.textPreview = item.value("text_preview", "");
      entry.byteSize = item.value("byte_size", static_cast<std::size_t>(0));
      entry.pinned = item.value("pinned", false);
      entry.payloadLoaded = false;

      const auto capturedAtMs = item.value("captured_at_ms", std::int64_t{0});
      entry.capturedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(capturedAtMs));

      if (entry.storageId.empty() || entry.dataMimeType.empty() || entry.byteSize == 0) {
        continue;
      }

      m_history.push_back(std::move(entry));
      m_historyBytes += m_history.back().byteSize;
    }

    // Enforce the storage invariant: pinned block first (relative order
    // preserved = most-recently-pinned first), then the unpinned region.
    std::ranges::stable_partition(m_history, [](const ClipboardEntry& entry) { return entry.pinned; });

    trimHistoryToBudget();
    kLog.info("loaded {} persisted clipboard entries", m_history.size());
  } catch (const std::exception& e) {
    kLog.warn("failed to load clipboard history: {}", e.what());
    m_history.clear();
    m_historyBytes = 0;
  }
}

bool ClipboardService::persistHistory() {
  if (!m_historyRetention) {
    return false;
  }

  namespace fs = std::filesystem;

  try {
    ensurePrivateDirectory(stateDirectory());
    ensurePrivateDirectory(entriesDirectory());

    nlohmann::json entries = nlohmann::json::array();
    std::unordered_set<std::string> activePayloadPaths;
    activePayloadPaths.reserve(m_history.size());

    for (auto& entry : m_history) {
      if (entry.storageId.empty()) {
        entry.storageId = generateStorageId();
      }
      if (entry.payloadPath.empty()) {
        entry.payloadPath = payloadPathForId(entry.storageId);
      }

      activePayloadPaths.insert(entry.payloadPath);
      if (entry.payloadLoaded && !entry.data.empty()) {
        if (!writeTextFileAtomic(entry.payloadPath, binaryView(entry.data), FileUtils::privateFileMode())) {
          throw std::runtime_error("failed to write clipboard payload");
        }
      }

      const auto capturedAtMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(entry.capturedAt.time_since_epoch()).count();
      entries.push_back({
          {"id", entry.storageId},
          {"payload_path", entry.payloadPath},
          {"mime_types", entry.mimeTypes},
          {"data_mime_type", entry.dataMimeType},
          {"text_preview", entry.textPreview},
          {"byte_size", entry.byteSize},
          {"captured_at_ms", capturedAtMs},
          {"pinned", entry.pinned},
      });
    }

    const fs::path manifest(manifestPath());
    const std::string manifestContent = nlohmann::json{{"entries", entries}}.dump(2);
    if (!writeTextFileAtomic(manifest, manifestContent, FileUtils::privateFileMode())) {
      throw std::runtime_error("failed to write clipboard manifest");
    }

    const fs::path entriesDir(entriesDirectory());
    if (fs::exists(entriesDir)) {
      for (const auto& dirEntry : fs::directory_iterator(entriesDir)) {
        const auto filePath = dirEntry.path().string();
        if (!activePayloadPaths.contains(filePath)) {
          fs::remove(dirEntry.path());
        }
      }
    }
    return true;
  } catch (const std::exception& e) {
    kLog.warn("failed to persist clipboard history: {}", e.what());
    return false;
  }
}

void ClipboardService::trimHistoryToBudget() {
  // Pinned entries are exempt from the budget and do not count toward it.
  // Only the unpinned tail is trimmed; since the pinned block is contiguous at
  // the front, the back is always unpinned while any unpinned entry remains.
  std::size_t unpinnedCount = 0;
  std::size_t unpinnedBytes = 0;
  for (const auto& entry : m_history) {
    if (!entry.pinned) {
      ++unpinnedCount;
      unpinnedBytes += entry.byteSize;
    }
  }

  while ((unpinnedCount > m_maxHistoryEntries || unpinnedBytes > kMaxHistoryBytes)
         && !m_history.empty()
         && !m_history.back().pinned) {
    const std::size_t removedBytes = m_history.back().byteSize;
    unpinnedBytes -= removedBytes;
    --unpinnedCount;
    if (m_historyBytes >= removedBytes) {
      m_historyBytes -= removedBytes;
    } else {
      m_historyBytes = 0;
    }
    m_history.pop_back();
  }
}

bool ClipboardService::loadEntryPayload(ClipboardEntry& entry) {
  if (entry.payloadLoaded) {
    return !entry.data.empty();
  }
  if (entry.payloadPath.empty()) {
    entry.payloadPath = payloadPathForId(entry.storageId);
  }

  std::ifstream file(entry.payloadPath, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return false;
  }

  const auto size = file.tellg();
  if (size <= 0) {
    return false;
  }

  entry.data.resize(static_cast<std::size_t>(size));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(entry.data.data()), size);
  if (!file.good()) {
    entry.data.clear();
    entry.data.shrink_to_fit();
    return false;
  }
  entry.payloadLoaded = true;
  if (entry.byteSize == 0) {
    entry.byteSize = entry.data.size();
  }
  if (entry.textPreview.empty() && isTextMimeType(entry.dataMimeType)) {
    entry.textPreview = buildTextPreview(entry.data);
  }
  return true;
}

void ClipboardService::evictPayloadData(ClipboardEntry& entry) {
  if (entry.data.empty() || entry.payloadPath.empty()) {
    return;
  }
  entry.data.clear();
  entry.data.shrink_to_fit();
  entry.payloadLoaded = false;
}

std::string ClipboardService::stateDirectory() {
  const std::string dir = FileUtils::stateDir();
  if (!dir.empty()) {
    return dir + "/clipboard";
  }
  return "/tmp/noctalia-clipboard";
}

std::string ClipboardService::manifestPath() { return stateDirectory() + "/index.json"; }

std::string ClipboardService::entriesDirectory() { return stateDirectory() + "/entries"; }

std::string ClipboardService::payloadPathForId(std::string_view storageId) {
  return entriesDirectory() + "/" + std::string(storageId) + ".bin";
}

std::string ClipboardService::generateStorageId() {
  const auto now =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  return std::to_string(now) + "-" + std::to_string(++gStorageCounter);
}

std::string ClipboardService::chooseMimeType(const OfferState& offer) const {
  const auto choose = [&offer](const auto& preferred) -> std::string {
    for (std::string_view mimeType : preferred) {
      const auto it = std::ranges::find(offer.mimeTypes, mimeType);
      if (it != offer.mimeTypes.end()) {
        return *it;
      }
    }
    return {};
  };

  if (std::string mimeType = choose(kTextMimeTypes); !mimeType.empty()) {
    return mimeType;
  }
  return choose(kImageMimeTypes);
}

bool ClipboardService::isTextMimeType(std::string_view mimeType) {
  return std::ranges::contains(kTextMimeTypes, mimeType);
}

bool ClipboardService::isEmptyTextPayload(const std::vector<std::uint8_t>& data) {
  bool sawContent = false;
  for (std::uint8_t byte : data) {
    if (byte == 0) {
      continue;
    }
    if (byte == ' ' || byte == '\n' || byte == '\r' || byte == '\t' || byte == '\f' || byte == '\v') {
      continue;
    }
    sawContent = true;
    break;
  }
  return !sawContent;
}

bool ClipboardService::queueOutgoingWrite(void* source, int fd, std::shared_ptr<const std::vector<std::uint8_t>> data) {
  if (fd < 0 || data == nullptr || data->empty()) {
    return false;
  }

  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    kLog.warn("failed to make clipboard send fd nonblocking");
    return false;
  }

  m_activeWrites.push_back(
      ActiveWrite{
          .fd = fd,
          .source = source,
          .data = std::move(data),
          .offset = 0,
      }
  );
  drainOutgoingWrite(m_activeWrites.size() - 1);
  return true;
}

void ClipboardService::dispatchWriteEvents(int fd, short revents) {
  if (fd < 0 || revents == 0) {
    return;
  }

  const auto findWrite = [this, fd]() { return std::ranges::find(m_activeWrites, fd, &ActiveWrite::fd); };

  auto it = findWrite();
  if (it == m_activeWrites.end()) {
    return;
  }

  if ((revents & POLLOUT) != 0) {
    drainOutgoingWrite(static_cast<std::size_t>(it - m_activeWrites.begin()));
    it = findWrite();
  }

  if (it != m_activeWrites.end() && (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    closeActiveWrite(static_cast<std::size_t>(it - m_activeWrites.begin()));
  }
}

void ClipboardService::drainOutgoingWrite(std::size_t index) {
  if (index >= m_activeWrites.size()) {
    return;
  }

  auto& writeState = m_activeWrites[index];
  if (writeState.fd < 0 || writeState.data == nullptr) {
    closeActiveWrite(index);
    return;
  }

  const auto& data = *writeState.data;
  while (writeState.offset < data.size()) {
    const auto* bytes = reinterpret_cast<const char*>(data.data() + writeState.offset);
    const std::size_t remaining = data.size() - writeState.offset;
    const ssize_t written = write(writeState.fd, bytes, remaining);
    if (written > 0) {
      writeState.offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    closeActiveWrite(index);
    return;
  }

  closeActiveWrite(index);
}

void ClipboardService::closeActiveWrite(std::size_t index) {
  if (index >= m_activeWrites.size()) {
    return;
  }
  closeFd(m_activeWrites[index].fd);
  if (index + 1 < m_activeWrites.size()) {
    m_activeWrites[index] = std::move(m_activeWrites.back());
  }
  m_activeWrites.pop_back();
}

std::string ClipboardService::buildTextPreview(const std::vector<std::uint8_t>& data) {
  const std::size_t limit = std::min<std::size_t>(data.size(), kPreviewBytes);
  // Forward-scan to find the last complete UTF-8 sequence within the byte limit.
  std::size_t safeSize = 0;
  std::size_t i = 0;
  while (i < limit) {
    const auto b = static_cast<uint8_t>(data[i]);
    int seqLen = 1;
    if ((b & 0x80) == 0)
      seqLen = 1;
    else if ((b & 0xE0) == 0xC0)
      seqLen = 2;
    else if ((b & 0xF0) == 0xE0)
      seqLen = 3;
    else if ((b & 0xF8) == 0xF0)
      seqLen = 4;
    if (i + static_cast<std::size_t>(seqLen) > limit)
      break;
    safeSize = i + seqLen;
    i += seqLen;
  }
  std::string preview(reinterpret_cast<const char*>(data.data()), safeSize);
  std::ranges::replace(preview, '\n', ' ');
  std::ranges::replace(preview, '\r', ' ');
  std::ranges::replace(preview, '\t', ' ');
  return preview;
}

void ClipboardService::notifyChanged() const {
  if (m_changeCallback) {
    m_changeCallback();
  }
}
