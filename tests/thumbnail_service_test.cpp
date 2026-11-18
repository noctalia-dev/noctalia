#include "render/core/image_encoder.h"
#include "render/core/texture_manager.h"
#include "render/core/thumbnail_service.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <vector>

namespace {

  // Capture the exact RGBA buffer handed to the established image renderer, without a GL context.
  class RecordingTextures : public TextureManager {
  public:
    std::vector<std::uint8_t> pixels;
    TextureHandle loadFromRgba(const std::uint8_t* data, int width, int height, bool) override {
      pixels.assign(data, data + width * height * 4);
      return {.id = TextureId{1}, .width = width, .height = height};
    }
    TextureHandle loadFromFile(const std::string&, int, bool) override { return {}; }
    TextureHandle loadFromEncodedBytes(const std::uint8_t*, std::size_t, bool) override { return {}; }
    TextureHandle loadFromRaw(const std::uint8_t*, std::size_t, int, int, int, PixmapFormat, bool) override {
      return {};
    }
    TextureHandle loadFromPixels(const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return {};
    }
    TextureHandle createEmpty(int, int, TextureDataFormat, TextureFilter) override { return {}; }
    bool replace(TextureHandle&, const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return false;
    }
    bool updateSubImage(TextureHandle&, const std::uint8_t*, int, int, int, int, TextureDataFormat) override {
      return false;
    }
    void unload(TextureHandle& handle) override { handle = {}; }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override {}
  };

  void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    assert(!bytes.empty());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(file.good());
  }

  std::vector<std::uint8_t> load(const std::filesystem::path& path, int target) {
    RecordingTextures textures;
    ThumbnailService service;
    (void)service.acquire(path.string(), target);
    std::vector<pollfd> fds;
    const auto index = service.addPollFds(fds);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (textures.pixels.empty() && std::chrono::steady_clock::now() < deadline) {
      (void)poll(fds.data(), fds.size(), 100);
      service.dispatch(fds, index);
      (void)service.uploadPending(textures);
    }
    assert(!textures.pixels.empty());
    return textures.pixels;
  }

  void checkAlpha(const std::vector<std::uint8_t>& pixels, std::uint8_t alpha) {
    for (std::size_t i = 3; i < pixels.size(); i += 4)
      assert(pixels[i] == alpha);
  }

  // Seed a valid opaque entry under the old version's actual cache key.
  void seedOpaqueV4Cache(const std::filesystem::path& root, const std::filesystem::path& source) {
    const std::string key = source.string() + '\n' + std::to_string(std::filesystem::file_size(source)) + '\n'
        + std::to_string(std::filesystem::last_write_time(source).time_since_epoch().count())
        + "\n8\nthumbnail-service-v4";
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : key) {
      hash ^= ch;
      hash *= 1099511628211ULL;
    }
    const auto directory = root / "nythera" / "thumbnails";
    std::filesystem::create_directories(directory);
    const std::array<std::uint8_t, 4> opaque{0, 0, 0, 255};
    write(directory / std::format("{:016x}.webp", hash), encodeWebp(opaque.data(), 1, 1, 100));
  }

  void checkFixture(
      const std::filesystem::path& path, const std::vector<std::uint8_t>& encoded, int target, std::uint8_t alpha
  ) {
    write(path, encoded);
    checkAlpha(load(path, target), alpha);
    // Retain the cache key, but make source decoding fail. A second service must read the on-disk WebP.
    const auto mtime = std::filesystem::last_write_time(path);
    write(path, std::vector<std::uint8_t>(encoded.size(), 0));
    std::filesystem::last_write_time(path, mtime);
    checkAlpha(load(path, target), alpha);
  }
} // namespace

int main() {
  std::array<char, 64> temp{};
  std::string pattern = "/tmp/nythera-thumbnail-test-XXXXXX";
  std::copy(pattern.begin(), pattern.end(), temp.begin());
  const char* directory = mkdtemp(temp.data());
  assert(directory);
  const std::filesystem::path root(directory);
  const char* cacheEnv = std::getenv("XDG_CACHE_HOME");
  const std::string previousCache = cacheEnv ? cacheEnv : "";
  const bool hadCache = cacheEnv != nullptr;
  assert(setenv("XDG_CACHE_HOME", directory, 1) == 0);

  for (const std::uint8_t alpha : {0, 128, 255}) {
    std::vector<std::uint8_t> rgba(8 * 8 * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
      rgba[i] = 255;
      rgba[i + 3] = alpha;
    }
    for (int target : {4, 8}) {
      const auto suffix = std::to_string(alpha) + "-" + std::to_string(target);
      checkFixture(root / (suffix + ".png"), encodePng(rgba.data(), 8, 8), target, alpha);
      checkFixture(root / (suffix + ".webp"), encodeWebp(rgba.data(), 8, 8, 100), target, alpha);
      if (alpha == 255)
        checkFixture(root / (suffix + ".jpg"), encodeJpeg(rgba.data(), 8, 8, 90), target, 255);
    }
  }

  const auto oldCacheSource = root / "old-cache.png";
  const std::array<std::uint8_t, 4> transparent{255, 0, 0, 0};
  write(oldCacheSource, encodePng(transparent.data(), 1, 1));
  seedOpaqueV4Cache(root, oldCacheSource);
  checkAlpha(load(oldCacheSource, 8), 0);

  // A valid transparent 1x1 GIF, including a transparency control extension.
  const std::vector<std::uint8_t> gif{
      'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 255, 255, 255,
      0x21, 0xF9, 4, 1, 0, 0, 0, 0, 0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0, 0x3B};
  checkFixture(root / "transparent.gif", gif, 8, 0);

  // Hidden blue must not bleed into red at a downsampled transparent boundary.
  const std::vector<std::uint8_t> edge{0, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 128, 0, 0, 255, 0};
  const auto edgePath = root / "edge.png";
  write(edgePath, encodePng(edge.data(), 4, 1));
  const auto resized = load(edgePath, 2);
  assert(resized.size() == 8);
  for (std::size_t i = 0; i < resized.size(); i += 4) {
    assert(resized[i] >= 250 && resized[i + 1] <= 5 && resized[i + 2] <= 5);
    assert(resized[i + 3] > 0 && resized[i + 3] < 255);
  }
  const auto cached = load(edgePath, 2);
  for (std::size_t i = 0; i < cached.size(); i += 4) {
    assert(cached[i] >= 240 && cached[i + 1] <= 10 && cached[i + 2] <= 10);
    assert(cached[i + 3] == resized[i + 3]);
  }

  if (hadCache)
    assert(setenv("XDG_CACHE_HOME", previousCache.c_str(), 1) == 0);
  else
    assert(unsetenv("XDG_CACHE_HOME") == 0);
  std::filesystem::remove_all(root);
  std::println("thumbnail alpha: fresh/cache PNG, WebP, GIF, opaque JPEG and filtered edges passed");
}
