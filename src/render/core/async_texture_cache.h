#pragma once

#include "app/poll_source.h"
#include "render/core/texture_handle.h"

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

class GlSharedContext;
class TextureManager;

class AsyncTextureCache : public PollSource {
public:
  using TextureReadyCallback = std::function<void(TextureHandle)>;

  class ReadySubscription {
  public:
    ReadySubscription() = default;
    ~ReadySubscription();

    ReadySubscription(const ReadySubscription&) = delete;
    ReadySubscription& operator=(const ReadySubscription&) = delete;

    ReadySubscription(ReadySubscription&& other) noexcept;
    ReadySubscription& operator=(ReadySubscription&& other) noexcept;

    void disconnect();

  private:
    friend class AsyncTextureCache;

    ReadySubscription(AsyncTextureCache* cache, std::weak_ptr<void> lifetimeToken, std::uint64_t id);

    AsyncTextureCache* m_cache = nullptr;
    std::weak_ptr<void> m_lifetimeToken;
    std::uint64_t m_id = 0;
  };

  AsyncTextureCache();
  ~AsyncTextureCache() override;

  AsyncTextureCache(const AsyncTextureCache&) = delete;
  AsyncTextureCache& operator=(const AsyncTextureCache&) = delete;

  void initialize(GlSharedContext* sharedGl);
  void setMakeCurrentCallback(std::function<void()> callback) { m_makeCurrentCallback = std::move(callback); }
  [[nodiscard]] ReadySubscription
  subscribeReady(const std::string& path, int targetSize, bool mipmap, TextureReadyCallback callback);

  [[nodiscard]] TextureHandle acquire(const std::string& path, int targetSize = 0, bool mipmap = false);
  [[nodiscard]] TextureHandle peek(const std::string& path, int targetSize = 0, bool mipmap = false) const;
  void release(const std::string& path, int targetSize = 0, bool mipmap = false);
  void trimUnused(std::size_t maxUnusedEntries = 0);
  void reloadResidentTextures();

  [[nodiscard]] int pollTimeoutMs() const override { return -1; }
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct RequestKey {
    std::string path;
    int targetSize = 0;
    bool mipmap = false;

    [[nodiscard]] bool operator==(const RequestKey& other) const noexcept {
      return path == other.path && targetSize == other.targetSize && mipmap == other.mipmap;
    }
  };

  struct RequestKeyHash {
    [[nodiscard]] std::size_t operator()(const RequestKey& key) const noexcept;
  };

  struct Entry {
    TextureHandle handle;
    int refCount = 0;
    bool failed = false;
    std::uint64_t lastTouch = 0;
  };

  struct DecodedJob {
    RequestKey key;
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    bool failed = false;
  };

  struct ReadyListener {
    RequestKey key;
    TextureReadyCallback callback;
  };

  void workerLoop();
  void signalMain();
  void pushResult(DecodedJob job);
  void makeCurrent();
  void touchEntry(Entry& entry);
  void pruneUnusedEntries(std::size_t maxUnusedEntries);
  void removeReadyListener(std::uint64_t id);
  void notifyReady(const RequestKey& key, TextureHandle handle);

  [[nodiscard]] static RequestKey makeKey(const std::string& path, int targetSize, bool mipmap);

  GlSharedContext* m_sharedGl = nullptr;
  std::function<void()> m_makeCurrentCallback;
  std::unique_ptr<TextureManager> m_textureManager;
  int m_eventFd = -1;
  std::vector<std::thread> m_workers;
  std::atomic<bool> m_shutdown{false};

  mutable std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  std::deque<RequestKey> m_jobQueue;
  std::unordered_set<RequestKey, RequestKeyHash> m_inFlight;
  std::unordered_set<RequestKey, RequestKeyHash> m_canceled;

  mutable std::mutex m_resultMutex;
  std::deque<DecodedJob> m_results;

  // Main thread only state.
  std::unordered_map<RequestKey, Entry, RequestKeyHash> m_entries;
  std::uint64_t m_touchSerial = 0;
  std::unordered_map<std::uint64_t, ReadyListener> m_readyListeners;
  std::uint64_t m_nextReadyListenerId = 0;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
};
