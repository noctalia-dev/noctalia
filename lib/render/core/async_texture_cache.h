#pragma once

#include "core/poll_source.h"
#include "render/core/color.h"
#include "render/core/texture_handle.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class GlSharedContext;
class TextureManager;

// Everything a decode produces, in buffer pixels. Two equal requests share one texture.
struct ImageDecodeRequest {
  std::string path;
  // Bounds the decoded long edge; 0 keeps the source size.
  int targetSize = 0;
  bool mipmap = false;
  // Crops the source to its centered square before bounding it to targetSize.
  bool centerSquareCrop = false;
  int blurRadius = 0;
  // App-icon colorization (image_effects::colorizeAppIcon), resolved from the palette on the main thread.
  std::optional<Color> appIconTint;
  bool appIconLightTheme = false;
  // Symbolic icon recolor (image_effects::symbolicMask).
  std::optional<Color> symbolicColor;
  // Bumped by AsyncTextureCache::invalidatePath() so a file changed on disk decodes again.
  std::uint64_t generation = 0;

  bool operator==(const ImageDecodeRequest&) const = default;
};

class AsyncTextureCache : public PollSource {
public:
  // Receives the texture once decoded, or an empty handle when the decode failed.
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

  // Settled requests (ready or failed) call back immediately and return an empty subscription.
  [[nodiscard]] ReadySubscription subscribeReady(const ImageDecodeRequest& request, TextureReadyCallback callback);

  // Takes a reference and queues the decode; returns the texture when it is already resident.
  TextureHandle acquire(const ImageDecodeRequest& request);
  TextureHandle peek(const ImageDecodeRequest& request) const;
  [[nodiscard]] bool failed(const ImageDecodeRequest& request) const;
  void release(const ImageDecodeRequest& request);

  // Generation to stamp on new requests for `path`; invalidatePath() advances it.
  [[nodiscard]] std::uint64_t generation(const std::string& path) const;
  void invalidatePath(const std::string& path);

  void trimUnused(std::size_t maxUnusedEntries = 0);
  void abandonGpuResources() noexcept;
  void reloadResidentTextures();

  int pollTimeoutMs() const override { return -1; }
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct RequestHash {
    std::size_t operator()(const ImageDecodeRequest& request) const noexcept;
  };

  struct Entry {
    TextureHandle handle;
    int refCount = 0;
    bool failed = false;
    std::uint64_t lastTouch = 0;
  };

  struct DecodedJob {
    ImageDecodeRequest request;
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    bool failed = false;
  };

  struct ReadyListener {
    ImageDecodeRequest request;
    TextureReadyCallback callback;
  };

  static DecodedJob decode(const ImageDecodeRequest& request);
  void workerLoop();
  void signalMain();
  void pushResult(DecodedJob job);
  void makeCurrent();
  void touchEntry(Entry& entry);
  void pruneUnusedEntries(std::size_t maxUnusedEntries);
  void removeReadyListener(std::uint64_t id);
  void notifyReady(const ImageDecodeRequest& request, TextureHandle handle);

  GlSharedContext* m_sharedGl = nullptr;
  std::function<void()> m_makeCurrentCallback;
  std::unique_ptr<TextureManager> m_textureManager;
  int m_eventFd = -1;
  std::vector<std::thread> m_workers;
  std::atomic<bool> m_shutdown{false};

  mutable std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  std::deque<ImageDecodeRequest> m_jobQueue;
  std::unordered_set<ImageDecodeRequest, RequestHash> m_inFlight;
  std::unordered_set<ImageDecodeRequest, RequestHash> m_canceled;

  mutable std::mutex m_resultMutex;
  std::deque<DecodedJob> m_results;

  // Main thread only state.
  std::unordered_map<ImageDecodeRequest, Entry, RequestHash> m_entries;
  std::unordered_map<std::string, std::uint64_t> m_pathGenerations;
  std::uint64_t m_touchSerial = 0;
  std::unordered_map<std::uint64_t, ReadyListener> m_readyListeners;
  std::uint64_t m_nextReadyListenerId = 0;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
};
