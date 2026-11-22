#include "shell/greeter/greeter_customization_staging.h"

#include "config/schema/greeter_schema.h"
#include "render/codecs/codecs.h"
#include "render/codecs/image_encoder.h"
#include "render/codecs/svg_rasterizer.h"
#include "util/file_utils.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <format>
#include <glib.h>
#include <span>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace greeter::detail {
  namespace {
    constexpr std::size_t kEncodedLimit = 16U * 1024U * 1024U;
    constexpr std::size_t kEncodedTotalLimit = 64U * 1024U * 1024U;
    constexpr std::size_t kDecodedTotalLimit = 128U * 1024U * 1024U;

    std::expected<std::vector<std::uint8_t>, std::string> readSource(const std::string& path) {
      const auto expanded = FileUtils::expandUserPath(path);
      if (!expanded.is_absolute()) {
        return std::unexpected("Sticker source must be an absolute local path or start with ~/.");
      }
      const int fd = ::open(expanded.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
      if (fd < 0) {
        return std::unexpected("Cannot open Sticker source: " + path);
      }
      struct stat before{};
      if (::fstat(fd, &before) != 0
          || !S_ISREG(before.st_mode)
          || before.st_size <= 0
          || static_cast<std::uint64_t>(before.st_size) > kEncodedLimit) {
        ::close(fd);
        return std::unexpected("Sticker source must be a regular file of at most 16 MiB: " + path);
      }
      std::vector<std::uint8_t> bytes;
      bytes.reserve(static_cast<std::size_t>(before.st_size));
      std::array<std::uint8_t, 64U * 1024U> chunk{};
      for (;;) {
        const auto count = ::read(fd, chunk.data(), chunk.size());
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count < 0 || (count > 0 && static_cast<std::size_t>(count) > kEncodedLimit - bytes.size())) {
          ::close(fd);
          return std::unexpected("Cannot read bounded Sticker source: " + path);
        }
        if (count == 0) {
          break;
        }
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
      }
      struct stat after{};
      const bool unchanged = ::fstat(fd, &after) == 0
          && before.st_size == after.st_size
          && before.st_mtim.tv_sec == after.st_mtim.tv_sec
          && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec
          && before.st_ctim.tv_sec == after.st_ctim.tv_sec
          && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
      ::close(fd);
      if (!unchanged) {
        return std::unexpected("Sticker source changed while being read: " + path);
      }
      return bytes;
    }

    std::expected<LoadedImageFile, std::string> decodeSource(const std::string& path) {
      auto bytes = readSource(path);
      if (!bytes) {
        return std::unexpected(bytes.error());
      }
      const auto extension = std::filesystem::path(path).extension().string();
      if (extension == ".svg" || extension == ".SVG") {
        return codecs::rasterizeSvg(*bytes, 4096, {});
      }
      auto image = codecs::decodeRasterImage(bytes->data(), bytes->size());
      if (!image) {
        return std::unexpected("Cannot decode Sticker source: " + image.error());
      }
      return LoadedImageFile{
          .rgba = std::move(image->pixels),
          .width = image->width,
          .height = image->height,
          .sourceWidth = image->width,
          .sourceHeight = image->height
      };
    }

    bool writePng(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
      const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd < 0) {
        return false;
      }
      std::size_t written = 0;
      while (written < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          break;
        }
        written += static_cast<std::size_t>(count);
      }
      const bool ready = written == bytes.size() && ::fchmod(fd, 0600) == 0;
      const bool closed = ::close(fd) == 0;
      if (!ready || !closed) {
        ::unlink(path.c_str());
      }
      return ready && closed;
    }
  } // namespace

  std::expected<toml::table, std::string>
  stageCustomization(const std::filesystem::path& staging, GreeterProfile profile) {
    namespace schema = noctalia::config::schema;
    schema::Diagnostics diagnostics;
    if (!schema::readGreeterConfig(schema::writeGreeterConfig(profile), {}, diagnostics)) {
      return std::unexpected(diagnostics.entries.front().describe());
    }
    std::unordered_map<std::string, std::string> paths;
    std::unordered_set<std::string> hashes;
    toml::array manifest;
    std::size_t encodedTotal = 0, decodedTotal = 0;
    if (profile.widgets && profile.widgets->instances) {
      for (auto& widget : *profile.widgets->instances) {
        if (widget.type != "sticker") {
          continue;
        }
        const auto setting = widget.settings.find("image_path");
        if (setting == widget.settings.end()) {
          continue;
        }
        auto& path = std::get<std::string>(setting->second);
        if (path.empty()) {
          continue;
        }
        if (const auto it = paths.find(path); it != paths.end()) {
          path = it->second;
          continue;
        }
        auto image = decodeSource(path);
        if (!image) {
          return std::unexpected(std::format("Sticker '{}': {}", widget.id, image.error()));
        }
        if (image->width < 1
            || image->height < 1
            || image->width > 4096
            || image->height > 4096
            || image->rgba.size()
                != static_cast<std::size_t>(image->width) * static_cast<std::size_t>(image->height) * 4U) {
          return std::unexpected("Sticker dimensions must be within 1..4096 pixels: " + widget.id);
        }
        std::string error;
        const auto png = encodePng(image->rgba.data(), image->width, image->height, &error);
        if (png.empty() || png.size() > kEncodedLimit) {
          return std::unexpected("Sticker PNG exceeds 16 MiB or cannot be encoded: " + widget.id + ". " + error);
        }
        gchar* digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, png.data(), png.size());
        if (!digest) {
          return std::unexpected("Cannot hash Sticker PNG.");
        }
        const std::string hash(digest);
        g_free(digest);
        if (hashes.insert(hash).second) {
          if (hashes.size() > 32
              || png.size() > kEncodedTotalLimit - encodedTotal
              || image->rgba.size() > kDecodedTotalLimit - decodedTotal) {
            return std::unexpected("Sticker assets exceed 32 images, 64 MiB PNG or 128 MiB decoded pixels.");
          }
          encodedTotal += png.size();
          decodedTotal += image->rgba.size();
          const auto name = "widget-" + hash + ".png";
          if (!writePng(staging / name, png)) {
            return std::unexpected("Cannot write staged Sticker PNG: " + widget.id);
          }
          manifest.push_back(name);
        }
        const auto installedReference = "widget-assets/" + hash + ".png";
        paths.emplace(path, installedReference);
        path = installedReference;
      }
    }
    auto tables = schema::writeGreeterConfig(profile);
    if (!manifest.empty()) {
      tables.insert("assets", toml::table{{"images", std::move(manifest)}});
    }
    return tables;
  }
} // namespace greeter::detail
