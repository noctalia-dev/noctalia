#include "filemanager/checksum.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace filemanager::checksum {
  namespace {
    GChecksumType checksumType(Algorithm algorithm) {
      switch (algorithm) {
      case Algorithm::Sha256:
        return G_CHECKSUM_SHA256;
      case Algorithm::Sha512:
        return G_CHECKSUM_SHA512;
      case Algorithm::Md5:
        return G_CHECKSUM_MD5;
      }
      return G_CHECKSUM_SHA256;
    }
    bool sameSnapshot(const struct stat& a, const struct stat& b) {
      return a.st_dev == b.st_dev
          && a.st_ino == b.st_ino
          && a.st_size == b.st_size
          && a.st_mtim.tv_sec == b.st_mtim.tv_sec
          && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
          && a.st_ctim.tv_sec == b.st_ctim.tv_sec
          && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
    }

    bool ioError(GError** error) {
      const int code = errno;
      g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(code), g_strerror(code));
      return false;
    }

    bool changed(GError** error) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "File changed while it was being read.");
      return false;
    }

    // Check the opened descriptor, then compare it with the path at EOF. O_NOFOLLOW
    // closes the gap between menu eligibility and opening the selected file.
    template <typename Consume>
    bool readRegular(
        const std::filesystem::path& path, GCancellable* cancellable, GError** error, const Progress& progress,
        Consume consume
    ) {
      if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return false;
      struct stat entry{};
      if (lstat(path.c_str(), &entry) != 0)
        return ioError(error);
      if (!S_ISREG(entry.st_mode)) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
            "Choose a regular local file. Symbolic links are not supported."
        );
        return false;
      }
      const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0)
        return ioError(error);
      struct CloseFd {
        int fd;
        ~CloseFd() { close(fd); }
      } guard{fd};
      struct stat before{};
      if (fstat(fd, &before) != 0)
        return ioError(error);
      if (!S_ISREG(before.st_mode) || !sameSnapshot(entry, before))
        return changed(error);

      const auto total = static_cast<std::uint64_t>(before.st_size);
      std::array<char, 128 * 1024> buffer;
      std::uint64_t bytes = 0;
      if (progress)
        progress(0, total);
      for (;;) {
        if (g_cancellable_set_error_if_cancelled(cancellable, error))
          return false;
        const auto count = read(fd, buffer.data(), buffer.size());
        if (count < 0) {
          if (errno == EINTR)
            continue;
          return ioError(error);
        }
        if (count == 0)
          break;
        bytes += static_cast<std::uint64_t>(count);
        if (bytes > total)
          return changed(error);
        if (!consume(std::string_view(buffer.data(), static_cast<std::size_t>(count))))
          return false;
        if (progress)
          progress(bytes, total);
      }
      struct stat after{}, current{};
      if (fstat(fd, &after) != 0)
        return ioError(error);
      if (lstat(path.c_str(), &current) != 0
          || bytes != total
          || !S_ISREG(current.st_mode)
          || !sameSnapshot(before, after)
          || !sameSnapshot(before, current))
        return changed(error);
      return !g_cancellable_set_error_if_cancelled(cancellable, error);
    }

    std::string_view trim(std::string_view text) {
      while (!text.empty() && g_ascii_isspace(text.front()))
        text.remove_prefix(1);
      while (!text.empty() && g_ascii_isspace(text.back()))
        text.remove_suffix(1);
      return text;
    }
  } // namespace

  bool eligibleSelection(const std::filesystem::path& path, std::size_t count, bool localSelection) {
    if (count != 1 || !localSelection || !path.is_absolute())
      return false;
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error));
  }

  std::string normalizeHash(std::string_view hash) {
    std::string normalized(trim(hash));
    std::ranges::transform(normalized, normalized.begin(), [](char c) { return g_ascii_tolower(c); });
    return normalized;
  }

  Comparison compare(std::string_view calculated, std::string_view expected) {
    return calculated == normalizeHash(expected) ? Comparison::Match : Comparison::Mismatch;
  }

  const char* name(Algorithm algorithm) noexcept {
    switch (algorithm) {
    case Algorithm::Sha256:
      return "SHA-256";
    case Algorithm::Sha512:
      return "SHA-512";
    case Algorithm::Md5:
      return "MD5";
    }
    return "SHA-256";
  }
  std::optional<Algorithm> fromHash(std::string_view hash) noexcept {
    if (hash.size() == 32)
      return Algorithm::Md5;
    if (hash.size() == 64)
      return Algorithm::Sha256;
    if (hash.size() == 128)
      return Algorithm::Sha512;
    return std::nullopt;
  }
  std::optional<Algorithm> fromManifestName(std::string_view filename) noexcept {
    std::string lower(filename);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
    if (lower.ends_with(".sha256"))
      return Algorithm::Sha256;
    if (lower.ends_with(".sha512"))
      return Algorithm::Sha512;
    if (lower.ends_with(".md5"))
      return Algorithm::Md5;
    return std::nullopt;
  }
  bool validHash(std::string_view hash, Algorithm algorithm) noexcept {
    const auto expected = algorithm == Algorithm::Md5 ? 32U : algorithm == Algorithm::Sha256 ? 64U : 128U;
    return hash.size() == expected && std::ranges::all_of(hash, [](unsigned char c) { return g_ascii_isxdigit(c); });
  }
  std::optional<ManifestEntry> parseManifestLine(std::string_view line, Algorithm algorithm) {
    if (line.ends_with('\r'))
      line.remove_suffix(1);
    const bool escaped = line.starts_with('\\');
    if (escaped)
      line.remove_prefix(1);
    const auto split = line.find_first_of(" \t");
    if (split == std::string_view::npos || !validHash(line.substr(0, split), algorithm))
      return std::nullopt;
    const auto hash = line.substr(0, split);
    auto rest = line.substr(split);
    if (rest.starts_with("  ") || rest.starts_with(" *"))
      rest.remove_prefix(2); // Keep filename whitespace and literal leading asterisks.
    else
      rest.remove_prefix(1);
    if (rest.empty() || rest.find('\0') != std::string_view::npos)
      return std::nullopt;
    std::string filename;
    for (std::size_t i = 0; i < rest.size(); ++i) {
      if (!escaped || rest[i] != '\\') {
        filename += rest[i];
        continue;
      }
      if (++i == rest.size())
        return std::nullopt;
      switch (rest[i]) {
      case '\\':
        filename += '\\';
        break;
      case 'n':
        filename += '\n';
        break;
      case 'r':
        filename += '\r';
        break;
      default:
        return std::nullopt;
      }
    }
    return ManifestEntry{algorithm, normalizeHash(hash), std::move(filename)};
  }

  bool loadExpected(
      const std::filesystem::path& manifest, const std::filesystem::path& target, Algorithm algorithm,
      GCancellable* cancellable, std::string& expected, GError** error
  ) {
    constexpr std::size_t maxManifestBytes = 4 * 1024 * 1024;
    std::string contents;
    const auto invalid = [error](const char* message) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
      return false;
    };
    if (!readRegular(manifest, cancellable, error, {}, [&](std::string_view chunk) {
          if (contents.size() + chunk.size() > maxManifestBytes)
            return invalid("Checksum manifests must be 4 MiB or smaller.");
          contents.append(chunk);
          return true;
        }))
      return false;

    std::optional<std::string> named, bare;
    std::size_t entries = 0;
    std::string_view remaining(contents);
    while (!remaining.empty()) {
      if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return false;
      const auto end = remaining.find('\n');
      const auto line = remaining.substr(0, end);
      remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end + 1);
      const auto stripped = trim(line);
      if (stripped.empty() || stripped.starts_with('#'))
        continue;
      ++entries;
      if (validHash(stripped, algorithm)) {
        bare = normalizeHash(stripped);
        continue;
      }
      const auto parsed = parseManifestLine(line, algorithm);
      if (!parsed)
        return invalid("Malformed checksum manifest or incorrect algorithm.");
      // Manifest names are labels only. They never become paths passed to I/O.
      const std::filesystem::path relative(parsed->filename);
      if (relative.is_absolute() || std::ranges::find(relative, "..") != relative.end())
        continue;
      const auto normalized = relative.lexically_normal();
      if (normalized != target.filename()
          && (manifest.parent_path() / normalized).lexically_normal() != target.lexically_normal())
        continue;
      if (named && *named != parsed->hash)
        return invalid("The manifest contains conflicting checksums for the selected file.");
      named = parsed->hash;
    }
    if (bare && entries != 1)
      return invalid("A checksum without a filename must be the only entry in the manifest.");
    if (!bare && !named)
      return invalid("The manifest contains no checksum for the selected file.");
    expected = bare ? std::move(*bare) : std::move(*named);
    return true;
  }

  bool calculate(
      const std::filesystem::path& path, Algorithm algorithm, GCancellable* cancellable, Result& result, GError** error,
      const Progress& progress
  ) {
    const std::unique_ptr<GChecksum, decltype(&g_checksum_free)> checksum(
        g_checksum_new(checksumType(algorithm)), g_checksum_free
    );
    std::uint64_t bytes = 0;
    if (!readRegular(path, cancellable, error, progress, [&](std::string_view chunk) {
          g_checksum_update(checksum.get(), reinterpret_cast<const guchar*>(chunk.data()), chunk.size());
          bytes += chunk.size();
          return true;
        }))
      return false;
    result = {g_checksum_get_string(checksum.get()), bytes};
    return true;
  }
} // namespace filemanager::checksum
