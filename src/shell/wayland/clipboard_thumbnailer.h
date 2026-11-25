#pragma once

#include "security/encrypted_file_store.h"
#include "security/secure_buffer.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Small RGBA previews of stored clipboard images for the history list. One worker decrypts, decodes and
// downscales a payload at a time, so a list of large screenshots costs one full-size decode at a time and
// keeps only the small results. Thumbnails stay in memory only; nothing decrypted is written to disk.
class ClipboardThumbnailer {
public:
  struct Thumbnail {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
  };

  struct Request {
    std::string storageId;
    std::filesystem::path payloadPath;
    std::size_t maxBytes = 0;
    int targetPx = 0;
    std::shared_ptr<const security::SecureKey> key;
    security::EncryptionContext context;
  };

  ClipboardThumbnailer();
  ~ClipboardThumbnailer();
  ClipboardThumbnailer(const ClipboardThumbnailer&) = delete;
  ClipboardThumbnailer& operator=(const ClipboardThumbnailer&) = delete;

  // Main thread only. Returns the finished thumbnail, or nullptr while it is queued or after it failed.
  [[nodiscard]] const Thumbnail* find(const std::string& storageId, int targetPx) const;
  [[nodiscard]] bool failed(const std::string& storageId, int targetPx) const;
  [[nodiscard]] bool queued(const std::string& storageId, int targetPx) const;
  // Main thread only. Queues a decode unless the thumbnail is ready, failed, or already queued.
  void request(Request request);
  // Called on the main thread after a queued thumbnail finished or failed.
  void setReadyCallback(std::function<void()> callback);
  void clear();

private:
  struct Result {
    std::string key;
    std::optional<Thumbnail> thumbnail;
  };

  static std::string cacheKey(const std::string& storageId, int targetPx);
  static std::optional<Thumbnail> makeThumbnail(const Request& request);
  void workerLoop();
  void onResult(Result result);

  std::unordered_map<std::string, Thumbnail> m_ready;
  std::unordered_set<std::string> m_failed;
  std::unordered_set<std::string> m_queued;
  std::function<void()> m_readyCallback;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<Request> m_jobs;
  bool m_shutdown = false;
  std::thread m_worker;
};
