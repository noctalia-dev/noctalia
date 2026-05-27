#pragma once

#include "app/poll_source.h"
#include "render/core/texture_manager.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Shared async thumbnail loader for image-backed UI. Worker threads decode and
// downsample images off the main thread; the main loop uploads finished
// pixmaps through TextureManager via uploadPending(). The service keeps
// explicit in-memory acquire/release ownership while persisting resized WebP
// files in the on-disk thumbnail cache.
class ThumbnailService : public PollSource {
public:
  using PendingUploadCallback = std::function<void()>;
  using ReadyCallback = std::function<void(const std::string& path, TextureHandle handle)>;

  class Subscription {
  public:
    Subscription() = default;
    ~Subscription();

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;

    void disconnect();

  private:
    friend class ThumbnailService;

    explicit Subscription(std::function<void()> disconnect);

    std::function<void()> m_disconnect;
  };

  ThumbnailService();
  ~ThumbnailService() override;

  ThumbnailService(const ThumbnailService&) = delete;
  ThumbnailService& operator=(const ThumbnailService&) = delete;

  // Decode completion is only a wakeup: consumers should schedule an update,
  // call uploadPending() with their current render context, then re-query
  // visible thumbnail handles.
  [[nodiscard]] Subscription subscribePendingUpload(PendingUploadCallback callback);
  [[nodiscard]] Subscription subscribeReady(const std::string& path, ReadyCallback callback);

  // Returns the uploaded handle if already decoded, or {0} if a decode is
  // pending. Each acquire must be paired with release() by the owning tile.
  [[nodiscard]] TextureHandle acquire(const std::string& path);

  // Non-owning lookup used while refreshing an already-acquired thumbnail.
  [[nodiscard]] TextureHandle peek(const std::string& path) const;

  // Releases one acquire() owner. The texture is unloaded only when the last
  // owner releases it.
  void release(const std::string& path);

  // Uploads decoded pixmaps to textures. Must run on the main thread with the
  // owning render context current.
  [[nodiscard]] bool uploadPending(TextureManager& textures);

  [[nodiscard]] int pollTimeoutMs() const override { return -1; }
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct DecodedJob {
    std::string path;
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    bool failed = false;
  };

  void workerLoop();
  void signalMain() const;
  void pushResult(DecodedJob job);
  void deleteAllTextures();
  void notifyPendingUpload();
  void notifyReady(const std::string& path, TextureHandle handle);
  void enqueueDecodeIfNeeded(const std::string& path);

  struct CacheEntry {
    TextureHandle handle;
    std::size_t refCount = 0;
    bool failed = false;
  };

  struct PendingListener {
    PendingUploadCallback callback;
  };

  struct ReadyListener {
    std::string path;
    ReadyCallback callback;
  };

  int m_eventFd = -1;
  std::vector<std::thread> m_workers;
  std::atomic<bool> m_shutdown{false};

  mutable std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  std::deque<std::string> m_jobQueue;
  std::unordered_set<std::string> m_inFlight;
  std::unordered_set<std::string> m_canceled;

  mutable std::mutex m_resultMutex;
  std::deque<DecodedJob> m_results;

  // Main thread only state.
  TextureManager* m_textureManager = nullptr;
  std::unordered_map<std::string, CacheEntry> m_entries;
  std::unordered_map<std::uint64_t, PendingListener> m_pendingListeners;
  std::unordered_map<std::uint64_t, ReadyListener> m_readyListeners;
  std::uint64_t m_nextListenerId = 1;
  std::shared_ptr<bool> m_lifetimeToken = std::make_shared<bool>(true);
};
