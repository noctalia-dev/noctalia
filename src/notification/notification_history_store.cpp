#include "notification/notification_history_store.h"

#include "core/log.h"
#include "notification/notification_manager.h"
#include "render/core/image_decoder.h"
#include "util/base64.h"
#include "util/file_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <webp/encode.h>

namespace {

  constexpr Logger kLog("notification-history");

  constexpr std::string_view kOriginExternal = "external";
  constexpr std::string_view kOriginInternal = "internal";

  constexpr std::string_view kUrgencyLow = "low";
  constexpr std::string_view kUrgencyNormal = "normal";
  constexpr std::string_view kUrgencyCritical = "critical";

  constexpr std::string_view kCloseExpired = "expired";
  constexpr std::string_view kCloseDismissed = "dismissed";
  constexpr std::string_view kCloseByCall = "closed_by_call";

  std::optional<std::chrono::system_clock::time_point> millisToWall(int64_t ms) {
    if (ms <= 0) {
      return std::nullopt;
    }
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
  }

  int64_t wallToMillis(const std::optional<std::chrono::system_clock::time_point>& tp) {
    if (!tp.has_value()) {
      return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp->time_since_epoch()).count();
  }

  std::string_view urgencyStr(Urgency u) noexcept {
    switch (u) {
    case Urgency::Low:
      return kUrgencyLow;
    case Urgency::Normal:
      return kUrgencyNormal;
    case Urgency::Critical:
      return kUrgencyCritical;
    }
    return kUrgencyNormal;
  }

  Urgency urgencyFrom(std::string_view s) noexcept {
    if (s == kUrgencyLow) {
      return Urgency::Low;
    }
    if (s == kUrgencyCritical) {
      return Urgency::Critical;
    }
    return Urgency::Normal;
  }

  std::string_view originStr(NotificationOrigin o) noexcept {
    return o == NotificationOrigin::Internal ? kOriginInternal : kOriginExternal;
  }

  NotificationOrigin originFrom(std::string_view s) noexcept {
    return s == kOriginInternal ? NotificationOrigin::Internal : NotificationOrigin::External;
  }

  std::optional<CloseReason> closeReasonFrom(std::string_view s) noexcept {
    if (s == kCloseExpired) {
      return CloseReason::Expired;
    }
    if (s == kCloseDismissed) {
      return CloseReason::Dismissed;
    }
    if (s == kCloseByCall) {
      return CloseReason::ClosedByCall;
    }
    return std::nullopt;
  }

  std::string_view closeReasonStr(CloseReason r) noexcept {
    switch (r) {
    case CloseReason::Expired:
      return kCloseExpired;
    case CloseReason::Dismissed:
      return kCloseDismissed;
    case CloseReason::ClosedByCall:
      return kCloseByCall;
    }
    return kCloseByCall;
  }

  constexpr std::string_view kAssetsDirName = "notification_history_assets";

  /// History list only needs small previews; keeps WebP sidecars tiny.
  constexpr int kMaxPersistImageSide = 96;
  constexpr float kPersistWebPQuality = 65.0f;

  std::filesystem::path assetsDirectoryForJson(const std::filesystem::path& jsonFilePath) {
    return jsonFilePath.parent_path() / kAssetsDirName;
  }

  /// 32 lowercase hex chars — content-addressed asset names (`i_<hex>.webp`).
  std::string hashBytesToHex32(const std::uint8_t* p, std::size_t n) {
    std::uint64_t h0 = 14695981039346656037ULL;
    std::uint64_t h1 = 13166748625691186689ULL;
    for (std::size_t i = 0; i < n; ++i) {
      h0 ^= p[i];
      h0 *= 1099511628211ULL;
      h1 ^= static_cast<std::uint64_t>(p[i]) << ((i % 8) * 8);
      h1 *= 11400714819323198485ULL;
    }
    h0 ^= n;
    h1 ^= n * 0x9e3779b97f4a7c15ULL;
    char buf[33];
    std::snprintf(
        buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(h0), static_cast<unsigned long long>(h1)
    );
    return std::string(buf);
  }

  std::string contentAddressedWebpName(const std::uint8_t* rgba, std::size_t rgbaBytes) {
    return std::string("i_") + hashBytesToHex32(rgba, rgbaBytes) + ".webp";
  }

  std::string contentAddressedRgbaName(const std::uint8_t* bytes, std::size_t byteCount) {
    return std::string("i_") + hashBytesToHex32(bytes, byteCount) + ".rgba";
  }

  bool writeRawRgbaBlob(
      const std::filesystem::path& assetsDir, const std::string& baseFileName, const std::vector<std::uint8_t>& bytes
  ) {
    std::error_code ec;
    std::filesystem::create_directories(assetsDir, ec);
    const auto path = assetsDir / baseFileName;
    if (!bytes.empty() && std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) == bytes.size()) {
      return true;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
      return false;
    }
    if (!bytes.empty()) {
      f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(f);
  }

  bool writeWebpBlobIfAbsent(const std::filesystem::path& path, const std::uint8_t* encoded, std::size_t encodedSize) {
    std::error_code ec;
    if (encoded == nullptr || encodedSize == 0) {
      return false;
    }
    if (std::filesystem::exists(path, ec)) {
      const auto sz = std::filesystem::file_size(path, ec);
      if (!ec && sz == static_cast<std::uint64_t>(encodedSize)) {
        return true;
      }
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) {
      return false;
    }
    wf.write(reinterpret_cast<const char*>(encoded), static_cast<std::streamsize>(encodedSize));
    return static_cast<bool>(wf);
  }

  /// Icon preset + slow method trades CPU once-per-image for smaller files than WebPEncodeRGBA alone.
  std::optional<std::vector<std::uint8_t>> encodeWebpForHistory(const std::uint8_t* rgba, int w, int h, int stride) {
    WebPConfig config;
    if (!WebPConfigPreset(&config, WEBP_PRESET_ICON, kPersistWebPQuality)) {
      return std::nullopt;
    }
    config.method = 6;
    config.alpha_quality = 70;
    if (!WebPValidateConfig(&config)) {
      return std::nullopt;
    }

    WebPPicture picture;
    if (!WebPPictureInit(&picture)) {
      return std::nullopt;
    }
    picture.width = w;
    picture.height = h;
    if (!WebPPictureImportRGBA(&picture, rgba, stride)) {
      WebPPictureFree(&picture);
      return std::nullopt;
    }

    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;

    if (!WebPEncode(&config, &picture)) {
      WebPPictureFree(&picture);
      WebPMemoryWriterClear(&writer);
      return std::nullopt;
    }
    WebPPictureFree(&picture);

    std::vector<std::uint8_t> out;
    if (writer.size > 0 && writer.mem != nullptr) {
      out.assign(writer.mem, writer.mem + writer.size);
    }
    WebPMemoryWriterClear(&writer);
    if (out.empty()) {
      return std::nullopt;
    }
    return out;
  }

  bool packContiguousRgba(const NotificationImageData& img, std::vector<std::uint8_t>& outRgba, int& outW, int& outH) {
    if (img.width <= 0 || img.height <= 0 || img.data.empty()) {
      return false;
    }
    if (img.bitsPerSample != 8 || (img.channels != 3 && img.channels != 4)) {
      return false;
    }
    outW = img.width;
    outH = img.height;
    const int c = img.channels;
    const int rs = (img.rowStride > 0) ? img.rowStride : (outW * c);
    outRgba.resize(static_cast<std::size_t>(outW) * static_cast<std::size_t>(outH) * 4);
    for (int y = 0; y < outH; ++y) {
      const std::uint8_t* row = img.data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(rs);
      std::uint8_t* dst = outRgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(outW) * 4;
      if (c == 4) {
        std::memcpy(dst, row, static_cast<std::size_t>(outW) * 4);
      } else {
        for (int x = 0; x < outW; ++x) {
          dst[x * 4 + 0] = row[x * 3 + 0];
          dst[x * 4 + 1] = row[x * 3 + 1];
          dst[x * 4 + 2] = row[x * 3 + 2];
          dst[x * 4 + 3] = 255;
        }
      }
    }
    return true;
  }

  void downscaleRgbaIfNeeded(std::vector<std::uint8_t>& rgba, int& w, int& h, int maxSide) {
    if (w <= maxSide && h <= maxSide) {
      return;
    }
    const float scale = std::min(
        static_cast<float>(maxSide) / static_cast<float>(w), static_cast<float>(maxSide) / static_cast<float>(h)
    );
    const int nw = std::max(1, static_cast<int>(std::lround(static_cast<float>(w) * scale)));
    const int nh = std::max(1, static_cast<int>(std::lround(static_cast<float>(h) * scale)));
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(nw) * static_cast<std::size_t>(nh) * 4);
    for (int y = 0; y < nh; ++y) {
      const int sy = y * h / nh;
      for (int x = 0; x < nw; ++x) {
        const int sx = x * w / nw;
        const std::uint8_t* srcPx = rgba.data()
            + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(sx)) * 4;
        std::uint8_t* dstPx =
            dst.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(nw) + static_cast<std::size_t>(x)) * 4;
        std::memcpy(dstPx, srcPx, 4);
      }
    }
    rgba = std::move(dst);
    w = nw;
    h = nh;
  }

  std::optional<NotificationImageData>
  imageFromJson(const nlohmann::json& j, const std::filesystem::path& jsonFilePath) {
    if (!j.is_object()) {
      return std::nullopt;
    }
    NotificationImageData img;
    img.width = j.value("width", 0);
    img.height = j.value("height", 0);
    img.rowStride = j.value("row_stride", 0);
    img.hasAlpha = j.value("has_alpha", true);
    img.bitsPerSample = j.value("bits_per_sample", 8);
    img.channels = j.value("channels", 4);

    const auto fileOnly = j.value("image_file", std::string());
    if (!fileOnly.empty()) {
      const auto blobPath = assetsDirectoryForJson(jsonFilePath) / fileOnly;
      img.data = FileUtils::readBinaryFile(blobPath.string());
      if (!img.data.empty()) {
        auto decoded = decodeRasterImage(img.data.data(), img.data.size());
        if (decoded) {
          img.width = decoded->width;
          img.height = decoded->height;
          img.rowStride = img.width * 4;
          img.channels = 4;
          img.hasAlpha = true;
          img.bitsPerSample = 8;
          img.data = std::move(decoded->pixels);
          return img;
        }
        // Legacy sidecar: raw RGBA bytes (not a supported container format).
        if (img.width > 0 && img.height > 0 && img.channels >= 3) {
          const std::size_t expected = static_cast<std::size_t>(img.width)
              * static_cast<std::size_t>(img.height)
              * static_cast<std::size_t>(img.channels);
          if (img.data.size() >= expected) {
            return img;
          }
        }
        kLog.warn("could not decode notification image blob {} ({})", blobPath.string(), decoded.error());
      } else if (img.width > 0 && img.height > 0) {
        kLog.warn("missing or empty image blob {}", blobPath.string());
      }
      return img;
    }

    const auto b64 = j.value("data_b64", std::string());
    if (!b64.empty()) {
      img.data = Base64::decode(b64);
      return img;
    }

    return img;
  }

  nlohmann::json
  imageToJson(const NotificationImageData& img, const std::filesystem::path& jsonFilePath, uint32_t notificationId) {
    nlohmann::json j;
    j["has_alpha"] = img.hasAlpha;
    j["bits_per_sample"] = img.bitsPerSample;
    j["channels"] = img.channels;

    if (img.data.empty() || img.width <= 0 || img.height <= 0) {
      j["width"] = img.width;
      j["height"] = img.height;
      j["row_stride"] = img.rowStride;
      return j;
    }

    std::vector<std::uint8_t> rgba;
    int w = 0;
    int h = 0;
    if (!packContiguousRgba(img, rgba, w, h)) {
      kLog.warn("could not pack notification image pixels for id {}", notificationId);
      j["width"] = img.width;
      j["height"] = img.height;
      j["row_stride"] = img.rowStride;
      j["data_b64"] = Base64::encode(img.data);
      return j;
    }

    downscaleRgbaIfNeeded(rgba, w, h, kMaxPersistImageSide);

    const auto assetsDir = assetsDirectoryForJson(jsonFilePath);
    std::error_code ecMk;
    std::filesystem::create_directories(assetsDir, ecMk);

    const std::string webpBase = contentAddressedWebpName(rgba.data(), rgba.size());
    const auto webpPath = assetsDir / webpBase;

    // Same normalized pixels → same filename: skip WebP encode and skip rewriting the file.
    if (std::filesystem::exists(webpPath, ecMk)) {
      j["width"] = w;
      j["height"] = h;
      j["row_stride"] = w * 4;
      j["has_alpha"] = true;
      j["bits_per_sample"] = 8;
      j["channels"] = 4;
      j["image_file"] = webpBase;
      return j;
    }

    std::vector<std::uint8_t> encodedBuf;
    if (auto advanced = encodeWebpForHistory(rgba.data(), w, h, w * 4); advanced.has_value()) {
      encodedBuf = std::move(*advanced);
    } else {
      std::uint8_t* encoded = nullptr;
      const std::size_t encodedSize = WebPEncodeRGBA(rgba.data(), w, h, w * 4, kPersistWebPQuality, &encoded);
      if (encoded != nullptr && encodedSize > 0) {
        encodedBuf.assign(encoded, encoded + encodedSize);
        WebPFree(encoded);
      }
    }

    if (!encodedBuf.empty() && writeWebpBlobIfAbsent(webpPath, encodedBuf.data(), encodedBuf.size())) {
      j["width"] = w;
      j["height"] = h;
      j["row_stride"] = w * 4;
      j["has_alpha"] = true;
      j["bits_per_sample"] = 8;
      j["channels"] = 4;
      j["image_file"] = webpBase;
      return j;
    }
    if (encodedBuf.empty()) {
      kLog.warn("WebP encode failed for notification image id {}", notificationId);
    } else {
      kLog.warn("failed to write WebP notification image for id {}", notificationId);
    }

    const std::string rawBase = contentAddressedRgbaName(img.data.data(), img.data.size());
    if (writeRawRgbaBlob(assetsDir, rawBase, img.data)) {
      j["width"] = img.width;
      j["height"] = img.height;
      j["row_stride"] = img.rowStride;
      j["image_file"] = rawBase;
      return j;
    }

    kLog.warn("failed to write notification image blob for id {}", notificationId);
    j["width"] = img.width;
    j["height"] = img.height;
    j["row_stride"] = img.rowStride;
    j["data_b64"] = Base64::encode(img.data);
    return j;
  }

  nlohmann::json notificationToJson(const Notification& n, const std::filesystem::path& jsonFilePath) {
    nlohmann::json j;
    j["id"] = n.id;
    j["origin"] = std::string(originStr(n.origin));
    j["app_name"] = n.appName;
    j["summary"] = n.summary;
    j["body"] = n.body;
    j["timeout"] = n.timeout;
    j["urgency"] = std::string(urgencyStr(n.urgency));
    j["actions"] = n.actions;
    if (n.icon.has_value()) {
      j["icon"] = *n.icon;
    } else {
      j["icon"] = nullptr;
    }
    if (n.imageData.has_value()) {
      j["image_data"] = imageToJson(*n.imageData, jsonFilePath, n.id);
    } else {
      j["image_data"] = nullptr;
    }
    if (n.category.has_value()) {
      j["category"] = *n.category;
    } else {
      j["category"] = nullptr;
    }
    if (n.desktopEntry.has_value()) {
      j["desktop_entry"] = *n.desktopEntry;
    } else {
      j["desktop_entry"] = nullptr;
    }
    j["received_wall_ms"] = wallToMillis(n.receivedWallClock);
    j["expiry_wall_ms"] = wallToMillis(n.expiryWallClock);
    return j;
  }

  Notification notificationFromJson(const nlohmann::json& j, const std::filesystem::path& jsonFilePath) {
    Notification n{};
    n.id = j.value("id", 0U);
    n.origin = originFrom(j.value("origin", std::string(kOriginExternal)));
    n.appName = j.value("app_name", std::string());
    n.summary = j.value("summary", std::string());
    n.body = j.value("body", std::string());
    n.timeout = j.value("timeout", 0);
    n.urgency = urgencyFrom(j.value("urgency", std::string(kUrgencyNormal)));
    if (j.contains("actions") && j["actions"].is_array()) {
      for (const auto& a : j["actions"]) {
        if (a.is_string()) {
          n.actions.push_back(a.get<std::string>());
        }
      }
    }
    if (j.contains("icon") && !j["icon"].is_null()) {
      n.icon = j["icon"].get<std::string>();
    }
    if (j.contains("image_data") && j["image_data"].is_object()) {
      n.imageData = imageFromJson(j["image_data"], jsonFilePath);
    }
    if (j.contains("category") && !j["category"].is_null()) {
      n.category = j["category"].get<std::string>();
    }
    if (j.contains("desktop_entry") && !j["desktop_entry"].is_null()) {
      n.desktopEntry = j["desktop_entry"].get<std::string>();
    }
    const int64_t rw = j.value("received_wall_ms", int64_t{0});
    if (rw > 0) {
      n.receivedWallClock = millisToWall(rw);
    }
    const int64_t ew = j.value("expiry_wall_ms", int64_t{0});
    if (ew > 0) {
      n.expiryWallClock = millisToWall(ew);
    }
    const auto steadyNow = Clock::now();
    n.receivedTime = steadyNow;
    n.expiryTime.reset();
    return n;
  }

  void collectReferencedImageFiles(const nlohmann::json& root, std::unordered_set<std::string>& out) {
    const auto entries = root.find("entries");
    if (entries == root.end() || !entries->is_array()) {
      return;
    }
    for (const auto& item : *entries) {
      if (!item.is_object() || !item.contains("notification")) {
        continue;
      }
      const auto& n = item["notification"];
      if (!n.contains("image_data") || !n["image_data"].is_object()) {
        continue;
      }
      const auto f = n["image_data"].value("image_file", std::string());
      if (!f.empty()) {
        out.insert(f);
      }
    }
  }

  void
  pruneOrphanImageBlobs(const std::filesystem::path& jsonFilePath, const std::unordered_set<std::string>& keepFiles) {
    const auto assetsDir = assetsDirectoryForJson(jsonFilePath);
    std::error_code ec;
    if (!std::filesystem::is_directory(assetsDir, ec)) {
      return;
    }
    for (const auto& ent : std::filesystem::directory_iterator(assetsDir, ec)) {
      if (ec || !ent.is_regular_file()) {
        continue;
      }
      const std::string name = ent.path().filename().string();
      const auto dot = name.rfind('.');
      if (dot == std::string::npos) {
        continue;
      }
      const std::string ext = name.substr(dot);
      if (ext != ".rgba" && ext != ".webp") {
        continue;
      }
      const bool legacyPerId = name.size() >= 7 && name[0] == 'n' && name[1] == '_';
      const bool contentAddressed = name.size() >= 7 && name[0] == 'i' && name[1] == '_';
      if (!legacyPerId && !contentAddressed) {
        continue;
      }
      if (!keepFiles.contains(name)) {
        std::filesystem::remove(ent.path(), ec);
      }
    }
  }

} // namespace

bool loadNotificationHistoryFromFile(
    const std::filesystem::path& path, std::deque<NotificationHistoryEntry>& out, std::uint32_t& outNextId,
    std::uint64_t& outChangeSerial
) {
  out.clear();
  outNextId = 1;
  outChangeSerial = 0;

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return true;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    kLog.warn("could not open notification history {}", path.string());
    return false;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(buffer.str());
  } catch (const std::exception& e) {
    kLog.warn("notification history parse failed: {}", e.what());
    return false;
  }

  if (!root.is_object()) {
    return false;
  }

  outNextId = root.value("next_id", 1U);
  outChangeSerial = root.value("change_serial", std::uint64_t{0});

  const auto entries = root.find("entries");
  if (entries == root.end() || !entries->is_array()) {
    return true;
  }

  std::uint32_t maxId = 0;
  std::uint64_t maxSerial = 0;

  for (const auto& item : *entries) {
    if (!item.is_object()) {
      continue;
    }
    NotificationHistoryEntry he;
    he.notification = notificationFromJson(item.at("notification"), path);
    he.active = item.value("active", false);
    he.seen = item.value("seen", true);
    if (item.contains("close_reason") && !item["close_reason"].is_null()) {
      const auto crs = item["close_reason"].get<std::string>();
      he.closeReason = closeReasonFrom(crs);
    }
    he.eventSerial = item.value("event_serial", std::uint64_t{0});

    maxId = std::max(maxId, he.notification.id);
    maxSerial = std::max(maxSerial, he.eventSerial);

    out.push_back(std::move(he));
  }

  outNextId = std::max(outNextId, maxId + 1);
  outChangeSerial = std::max(outChangeSerial, maxSerial);

  constexpr std::size_t kMaxHistoryEntries = 100;
  while (out.size() > kMaxHistoryEntries) {
    out.pop_front();
  }

  return true;
}

bool saveNotificationHistoryToFile(
    const std::filesystem::path& path, const std::deque<NotificationHistoryEntry>& entries, std::uint32_t nextId,
    std::uint64_t changeSerial
) {
  nlohmann::json root;
  root["version"] = 2;
  root["next_id"] = nextId;
  root["change_serial"] = changeSerial;
  auto& arr = root["entries"] = nlohmann::json::array();

  for (const auto& he : entries) {
    nlohmann::json je;
    je["notification"] = notificationToJson(he.notification, path);
    je["active"] = he.active;
    je["seen"] = he.seen;
    if (he.closeReason.has_value()) {
      je["close_reason"] = std::string(closeReasonStr(*he.closeReason));
    } else {
      je["close_reason"] = nullptr;
    }
    je["event_serial"] = he.eventSerial;
    arr.push_back(std::move(je));
  }

  const std::string tmpPath = path.string() + ".tmp";
  std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    kLog.warn("could not write notification history tmp {}", tmpPath);
    return false;
  }
  out << root.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
  out.close();

  std::error_code ec;
  std::filesystem::rename(tmpPath, path, ec);
  if (ec) {
    kLog.warn("could not rename notification history file: {}", ec.message());
    return false;
  }
  std::unordered_set<std::string> keepImageFiles;
  collectReferencedImageFiles(root, keepImageFiles);
  pruneOrphanImageBlobs(path, keepImageFiles);
  return true;
}
