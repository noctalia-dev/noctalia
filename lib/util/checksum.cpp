#include "util/checksum.h"

#include <array>
#include <fstream>
#include <glib.h>

namespace noctalia::util {

  std::string fileMd5Hex(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return {};
    }

    GChecksum* checksum = g_checksum_new(G_CHECKSUM_MD5);
    if (checksum == nullptr) {
      return {};
    }

    std::array<char, 8192> buffer{};
    while (in) {
      in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const std::streamsize bytesRead = in.gcount();
      if (bytesRead > 0) {
        g_checksum_update(checksum, reinterpret_cast<const guchar*>(buffer.data()), static_cast<gssize>(bytesRead));
      }
    }

    std::string digest;
    if (!in.bad()) {
      if (const gchar* value = g_checksum_get_string(checksum); value != nullptr) {
        digest = value;
      }
    }
    g_checksum_free(checksum);
    return digest;
  }

} // namespace noctalia::util
