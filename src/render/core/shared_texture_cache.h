#pragma once

#include "app/poll_source.h"
#include "render/core/texture_handle.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
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
class RenderBackend;
class TextureManager;

enum class SharedTextureKind : std::uint8_t {
  Color,
  // Single-channel texture built from the image's first channel (desktop widget wallpaper masks).
  AlphaMask,
};

// Physical pixels a texture still has to cover where it is drawn. The cache halves the decoded image with a 2x2 box
// filter while the result stays at least this large on both axes: exactly the mip levels the GPU would generate and
// never sample for that coverage, so nothing on screen changes. An empty coverage keeps the full resolution, as do
// disabled mipmaps (the renderer then samples the base level directly, so dropping it would change pixels).
struct TextureCoverage {
  int width = 0;
  int height = 0;

  [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
  bool operator==(const TextureCoverage&) const = default;
};

struct SharedTextureRequest {
  std::string path;
  SharedTextureKind kind = SharedTextureKind::Color;
  TextureCoverage coverage;

  bool operator==(const SharedTextureRequest&) const = default;
};

// Path-keyed, refcounted cache for large image textures (wallpapers and their masks). Images are decoded on a
// background thread and uploaded from dispatch() on the main thread, so a huge file never blocks the event loop.
//
// With a shared EGL context every consumer (wallpaper, backdrop, lockscreen, editors) shares one upload. Without
// one, each entry is uploaded into the RenderBackend its consumer draws with; the decode is still shared.
class SharedTextureCache : public PollSource {
public:
  // A consumer's hold on one texture. Releases its reference when destroyed or reset. texture() stays invalid while
  // the image decodes, after a decode failure, and between a GPU reset and the re-upload that follows it.
  class Lease {
  public:
    Lease() = default;
    ~Lease();

    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;

    [[nodiscard]] bool empty() const noexcept { return m_cache == nullptr; }
    [[nodiscard]] TextureHandle texture() const;
    [[nodiscard]] bool failed() const;
    // Re-targets the lease at another coverage. The current texture stays valid until the new one is uploaded, then
    // onChange fires; like every texture() change this happens from dispatch(), never inside this call.
    void setCoverage(TextureCoverage coverage);
    void reset();

  private:
    friend class SharedTextureCache;

    Lease(SharedTextureCache* cache, std::weak_ptr<void> lifetimeToken, std::uint64_t id);
    [[nodiscard]] SharedTextureCache* liveCache() const;

    SharedTextureCache* m_cache = nullptr;
    std::weak_ptr<void> m_lifetimeToken;
    std::uint64_t m_id = 0;
  };

  SharedTextureCache();
  ~SharedTextureCache() override;

  SharedTextureCache(const SharedTextureCache&) = delete;
  SharedTextureCache& operator=(const SharedTextureCache&) = delete;

  void initialize(GlSharedContext* sharedGl);

  // Takes a reference on `request`. `backend` receives the upload when there is no shared context and is ignored
  // otherwise; it must outlive the lease. `onChange` runs on the main thread from dispatch() whenever the lease's
  // texture() or failed() changes. It never runs from inside acquire(), so callers can apply a texture that is
  // already resident right away. Returns an empty lease for an empty path or a missing non-shared backend.
  [[nodiscard]] Lease acquire(SharedTextureRequest request, RenderBackend* backend, std::function<void()> onChange);

  void abandonGpuResources() noexcept;
  // Queues a fresh decode for every entry that lost its texture in a GPU reset.
  void reloadResidentTextures();

  [[nodiscard]] int pollTimeoutMs() const override { return -1; }
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  enum class EntryState : std::uint8_t {
    Loading,
    Ready,
    Failed,
  };

  struct EntryKey {
    SharedTextureRequest request;
    // Upload target without a shared context; null when the shared context owns the texture.
    RenderBackend* backend = nullptr;

    bool operator==(const EntryKey&) const = default;
  };

  struct RequestHash {
    [[nodiscard]] std::size_t operator()(const SharedTextureRequest& request) const noexcept;
  };

  struct EntryKeyHash {
    [[nodiscard]] std::size_t operator()(const EntryKey& key) const noexcept;
  };

  struct Entry {
    TextureHandle handle;
    int refCount = 0;
    EntryState state = EntryState::Loading;
  };

  struct LeaseRecord {
    EntryKey key;
    // Coverage the lease is moving to; key keeps serving until this entry is uploaded.
    std::optional<EntryKey> pending;
    std::function<void()> onChange;
  };

  struct DecodedImage {
    SharedTextureRequest request;
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    bool opaque = false;
    bool failed = false;
  };

  void retainEntry(const EntryKey& key);
  void releaseEntry(const EntryKey& key);
  void releaseLease(std::uint64_t id);
  [[nodiscard]] const Entry* findLeaseEntry(std::uint64_t id) const;
  void setLeaseCoverage(std::uint64_t id, TextureCoverage coverage);
  // Promotes a lease's pending entry once it is uploaded, or drops it if it failed. True if texture() changed.
  bool resolvePending(LeaseRecord& record);
  [[nodiscard]] static TextureCoverage effectiveCoverage(TextureCoverage coverage);

  void requestDecode(const SharedTextureRequest& request);
  void cancelDecodeIfUnused(const SharedTextureRequest& request);
  [[nodiscard]] bool hasLoadingEntry(const SharedTextureRequest& request) const;
  void upload(const EntryKey& key, Entry& entry, const DecodedImage& image);
  void unloadEntry(const EntryKey& key, Entry& entry);
  [[nodiscard]] TextureManager* textureManagerFor(const EntryKey& key) const;
  // Binds a context that can reach `key`'s textures. False while the context is lost; the next
  // reloadResidentTextures() requeues whatever could not be uploaded.
  [[nodiscard]] bool makeCurrentFor(const EntryKey& key);

  void workerLoop();
  void pushResult(DecodedImage image);
  void signalMain();

  GlSharedContext* m_sharedGl = nullptr;
  std::unique_ptr<TextureManager> m_textureManager;
  int m_eventFd = -1;
  std::thread m_worker;
  std::atomic<bool> m_shutdown{false};

  mutable std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  std::deque<SharedTextureRequest> m_jobQueue;

  std::mutex m_resultMutex;
  std::deque<DecodedImage> m_results;

  // Main thread only state.
  // Requests queued or decoding; a result clears its request even when no entry wants it anymore.
  std::unordered_set<SharedTextureRequest, RequestHash> m_pendingDecodes;
  std::unordered_map<EntryKey, Entry, EntryKeyHash> m_entries;
  std::unordered_map<std::uint64_t, LeaseRecord> m_leases;
  // Leases whose pending coverage was already resident when set; dispatch() switches them.
  std::vector<std::uint64_t> m_deferredSwitches;
  std::uint64_t m_nextLeaseId = 0;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
};
