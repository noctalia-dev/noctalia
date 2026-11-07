#include "core/application_identity.h"
#include "render/backend/render_backend.h"
#include "render/core/image_decoder.h"
#include "render/core/shared_texture_cache.h"
#include "render/core/texture_manager.h"
#include "render/gl_shared_context.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <poll.h>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>
#include <wayland-client.h>

namespace {
  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  class Decoder final : public ImageDecoder {
  public:
    std::expected<DecodedRasterImage, std::string> decode(const std::uint8_t*, std::size_t) override {
      return std::unexpected("unused");
    }
    std::expected<LoadedImageFile, std::string> loadFile(const std::string&, int, bool) override {
      ++calls;
      return LoadedImageFile{.rgba = std::vector<std::uint8_t>(64, 255), .width = 4, .height = 4};
    }
    std::atomic<int> calls{0};
  };

  std::shared_ptr<const LoadedImageFile> image(int width, int height, std::array<std::uint8_t, 4> pixel) {
    auto result = std::make_shared<LoadedImageFile>();
    result->width = width;
    result->height = height;
    result->sourceWidth = width * 10;
    result->sourceHeight = height * 10;
    result->rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::size_t i = 0; i < result->rgba.size(); ++i) {
      result->rgba[i] = pixel[i % 4];
    }
    return result;
  }

  void dispatch(SharedTextureCache& cache) {
    std::vector<pollfd> fds;
    const auto start = cache.addPollFds(fds);
    require(::poll(fds.data(), fds.size(), 2000) > 0, "cache did not wake its poll source");
    cache.dispatch(fds, start);
  }

  void pixels(TextureHandle texture, std::array<std::uint8_t, 4> expected) {
    require(texture.valid(), "expected a resident texture");
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.id.value(), 0);
    require(
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
        "texture was not reachable in this share group"
    );
    std::array<std::uint8_t, 4> actual{};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    require(glGetError() == GL_NO_ERROR && actual == expected, "uploaded pixel readback differed");
  }

  void sharedChecks(GlSharedContext& shared, RenderBackend& first, RenderBackend& second, Decoder& decoder) {
    SharedTextureCache cache;
    cache.initialize(&shared);
    const auto red = image(4, 4, {241, 17, 33, 128});
    const auto blue = image(4, 4, {13, 27, 239, 255});
    const PreparedTextureRequest a{.source = "asset", .revision = "one"};
    const PreparedTextureRequest b{.source = "asset", .revision = "two"};
    int changed = 0;
    auto left = cache.acquirePrepared(a, red, &first, [&] { ++changed; });
    auto right = cache.acquirePrepared(a, red, &second, [&] { ++changed; });
    require(
        !left.empty() && !right.empty() && !left.texture().valid(), "acquire uploaded inline or rejected valid pixels"
    );
    require(cache.preparedBytes() == 64 && cache.residentBytes() == 0, "pending payload accounting duplicated pixels");
    dispatch(cache);
    require(
        changed == 2 && left.texture().id == right.texture().id && cache.residentBytes() == 64,
        "shared consumers duplicated residency or missed notifications"
    );
    require(
        left.texture().sourceWidth == 40 && left.texture().sourceHeight == 40, "original image dimensions were lost"
    );
    require(first.makeCurrentNoSurface(), "first consumer context was unavailable");
    pixels(left.texture(), {241, 17, 33, 128});
    require(second.makeCurrentNoSurface(), "second consumer context was unavailable");
    pixels(right.texture(), {241, 17, 33, 128});
    const auto old = left.texture().id;
    require(left.setPreparedImage(b, blue), "prepared revision update was rejected");
    require(left.texture().id == old && changed == 2, "revision change replaced the texture inline");
    dispatch(cache);
    require(
        changed == 3 && left.texture().id != old && right.texture().id == old, "revision update changed the wrong lease"
    );
    require(cache.residentBytes() == 128 && cache.preparedBytes() == 128, "revision residency accounting was wrong");
    require(shared.makeCurrentSurfaceless(), "shared context was unavailable");
    pixels(left.texture(), {13, 27, 239, 255});
    require(left.setPreparedImage(a, red), "retarget to a resident revision failed");
    require(left.texture().id != old, "resident retarget happened outside dispatch");
    dispatch(cache);
    require(
        left.texture().id == old && cache.residentBytes() == 64 && changed == 4,
        "resident retarget did not reuse the shared texture"
    );

    const PreparedTextureRequest mip{.source = "asset", .revision = "one", .mipmap = true};
    auto mipmap = cache.acquirePrepared(mip, red, &first, [] {});
    auto sized = cache.acquirePrepared(a, image(2, 2, {41, 51, 61, 255}), &first, [] {});
    dispatch(cache);
    require(cache.residentBytes() == 64 + 84 + 16, "size/mipmap variants shared the wrong residency");
    glBindTexture(GL_TEXTURE_2D, mipmap.texture().id.value());
    GLint filter = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
    require(filter == GL_LINEAR_MIPMAP_LINEAR, "prepared mipmap mode was lost");
    mipmap.reset();
    sized.reset();
    auto pending = cache.acquirePrepared({.source = "cancel", .revision = "one"}, red, &first, [&] {
      throw std::runtime_error("cancelled callback ran");
    });
    pending.reset();
    auto replacement = cache.acquirePrepared({.source = "cancel", .revision = "two"}, blue, &first, [] {});
    dispatch(cache);
    require(shared.makeCurrentSurfaceless(), "shared context was unavailable");
    pixels(replacement.texture(), {13, 27, 239, 255});
    replacement.reset();

    cache.invalidatePrepared("asset");
    require(left.texture().valid() && !left.failed(), "invalidation ran outside dispatch");
    require(cache.acquirePrepared(a, red, &first, [] {}).empty(), "invalidated identity was silently reacquired");
    dispatch(cache);
    require(
        left.failed()
            && right.failed()
            && !left.texture().valid()
            && cache.residentBytes() == 0
            && cache.preparedBytes() == 0,
        "source invalidation retained stale pixels or residency"
    );
    require(changed == 6, "invalidation missed active consumers");
    left.reset();
    right.reset();
    auto recovered = cache.acquirePrepared({.source = "asset", .revision = "three"}, red, &first, [&] { ++changed; });
    dispatch(cache);
    const auto beforeReset = changed;
    cache.abandonGpuResources();
    require(
        !recovered.texture().valid() && cache.residentBytes() == 0 && cache.preparedBytes() == 64,
        "GPU reset lost prepared recovery pixels"
    );
    cache.reloadResidentTextures();
    dispatch(cache);
    require(changed == beforeReset + 1 && recovered.texture().valid(), "prepared recovery did not notify its consumer");
    require(shared.makeCurrentSurfaceless(), "shared context was unavailable");
    pixels(recovered.texture(), {241, 17, 33, 128});
    recovered.reset();
    require(cache.residentBytes() == 0 && cache.preparedBytes() == 0, "last release retained prepared memory");
    require(decoder.calls.load() == 0, "prepared images entered the path decoder");

    auto path = cache.acquire({.path = "fixture", .coverage = {}}, &first, [] {});
    dispatch(cache);
    require(path.texture().valid() && decoder.calls.load() == 1, "existing path decoding regressed");
    const auto originalPath = path.texture().id;
    path.setCoverage({2, 2});
    require(path.texture().id == originalPath, "path coverage update switched inline");
    dispatch(cache);
    require(
        path.texture().valid() && path.texture().width == 2 && decoder.calls.load() == 2,
        "path coverage retarget regressed"
    );
    require(!path.setPreparedImage(a, red), "path lease accepted a prepared-image replacement");
    path.reset();
    SharedTextureCache::Lease self;
    self = cache.acquirePrepared({.source = "callback", .revision = "one"}, red, &first, [&] { self.reset(); });
    dispatch(cache);
    require(self.empty() && cache.residentBytes() == 0, "self-releasing callback retained a texture");

    auto malformed = std::make_shared<LoadedImageFile>();
    malformed->width = 2;
    malformed->height = 2;
    malformed->rgba.resize(3);
    require(cache.acquirePrepared(a, malformed, &first, [] {}).empty(), "truncated pixels were accepted");
    require(
        cache.acquirePrepared({.source = "asset", .revision = ""}, red, &first, [] {}).empty(),
        "unversioned identity was accepted"
    );
    require(
        cache.acquirePrepared({.source = "", .revision = "one"}, red, &first, [] {}).empty(),
        "empty source identity was accepted"
    );
    require(cache.acquirePrepared(a, {}, &first, [] {}).empty(), "missing prepared pixels were accepted");
    SharedTextureCache::Lease orphan;
    {
      auto owner = std::make_unique<SharedTextureCache>();
      owner->initialize(&shared);
      orphan = owner->acquirePrepared(a, red, &first, [] {});
      dispatch(*owner);
    }
    require(!orphan.texture().valid() && !orphan.failed(), "lease used a destroyed cache");
    orphan.reset();
  }

  void unsharedChecks(GlSharedContext& unshared) {
    auto first = createDefaultRenderBackend(), second = createDefaultRenderBackend();
    first->initialize(unshared);
    second->initialize(unshared);
    SharedTextureCache cache;
    cache.initialize(nullptr);
    const auto prepared = image(2, 2, {21, 31, 41, 255});
    const PreparedTextureRequest key{.source = "unshared", .revision = "one"};
    require(cache.acquirePrepared(key, prepared, nullptr, [] {}).empty(), "missing nonshared backend was accepted");
    auto a = cache.acquirePrepared(key, prepared, first.get(), [] {});
    auto b = cache.acquirePrepared(key, prepared, second.get(), [] {});
    dispatch(cache);
    require(
        cache.preparedBytes() == 16 && cache.residentBytes() == 32, "nonshared backends lost per-context accounting"
    );
    require(first->makeCurrentNoSurface(), "nonshared context one failed");
    pixels(a.texture(), {21, 31, 41, 255});
    require(second->makeCurrentNoSurface(), "nonshared context two failed");
    pixels(b.texture(), {21, 31, 41, 255});
    a.reset();
    b.reset();
    require(cache.preparedBytes() == 0 && cache.residentBytes() == 0, "nonshared release retained memory");
    SharedTextureCache::Lease orphan;
    TextureId retired;
    {
      auto owner = std::make_unique<SharedTextureCache>();
      orphan = owner->acquirePrepared(key, prepared, first.get(), [] {});
      dispatch(*owner);
      retired = orphan.texture().id;
    }
    require(
        first->makeCurrentNoSurface() && glIsTexture(retired.value()) == GL_FALSE,
        "cache destruction leaked a prepared backend texture"
    );
    require(!orphan.texture().valid(), "nonshared lease used a destroyed cache");
    orphan.reset();
  }
} // namespace

int main(int argc, char** argv) {
  // Normal headless suites skip. A deliberate absolute private socket is required; never connect to the active desktop.
  if (argc == 1) {
    return 77;
  }
  if (argc != 2 || argv[1][0] != '/') {
    return 1;
  }
  setApplicationIdentity({.name = "texture-cache-test", .envPrefix = "TEXTURE_CACHE_TEST"});
  const std::unique_ptr<wl_display, decltype(&wl_display_disconnect)> display(
      wl_display_connect(argv[1]), &wl_display_disconnect
  );
  if (!display) {
    return 1;
  }
  try {
    auto decoder = std::make_unique<Decoder>();
    auto* provider = decoder.get();
    setImageDecoder(std::move(decoder));
    {
      GlSharedContext shared;
      shared.initialize(display.get());
      auto first = createDefaultRenderBackend(), second = createDefaultRenderBackend();
      first->initialize(shared);
      second->initialize(shared);
      sharedChecks(shared, *first, *second, *provider);
    }
    {
      GlSharedContext unshared;
      unshared.initialize(display.get(), false);
      unsharedChecks(unshared);
    }
  } catch (const std::exception& error) {
    std::println(stderr, "shared_texture_cache_test: {}", error.what());
    return 1;
  }
  return 0;
}
