#include "theme/firefox_theme/native_messaging.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace noctalia::theme::firefox_theme::native_messaging {
  namespace {

    bool writeAll(const void* data, std::size_t size) {
      const auto* bytes = static_cast<const std::uint8_t*>(data);
      while (size > 0) {
        const ssize_t written = ::write(STDOUT_FILENO, bytes, size);
        if (written < 0) {
          if (errno == EINTR) {
            continue;
          }
          return false;
        }
        if (written == 0) {
          return false;
        }
        bytes += written;
        size -= static_cast<std::size_t>(written);
      }
      return true;
    }

  } // namespace

  bool writeMessage(const nlohmann::json& message) {
    const std::string body = message.dump();
    const auto len = static_cast<std::uint32_t>(body.size());
    std::uint8_t header[4];
    header[0] = static_cast<std::uint8_t>(len & 0xffU);
    header[1] = static_cast<std::uint8_t>((len >> 8) & 0xffU);
    header[2] = static_cast<std::uint8_t>((len >> 16) & 0xffU);
    header[3] = static_cast<std::uint8_t>((len >> 24) & 0xffU);
    if (!writeAll(header, sizeof(header))) {
      return false;
    }
    if (!writeAll(body.data(), body.size())) {
      return false;
    }
    return true;
  }

  std::optional<nlohmann::json> StdinReader::tryRead() {
    while (true) {
      if (m_state == State::Length) {
        if (m_buf.size() < 4) {
          std::uint8_t tmp[64];
          const ssize_t n = ::read(STDIN_FILENO, tmp, sizeof(tmp));
          if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
              return std::nullopt;
            }
            m_eof = true;
            return std::nullopt;
          }
          if (n == 0) {
            m_eof = true;
            return std::nullopt;
          }
          m_buf.insert(m_buf.end(), tmp, tmp + n);
          continue;
        }
        m_expected = static_cast<std::uint32_t>(m_buf[0])
            | (static_cast<std::uint32_t>(m_buf[1]) << 8)
            | (static_cast<std::uint32_t>(m_buf[2]) << 16)
            | (static_cast<std::uint32_t>(m_buf[3]) << 24);
        m_buf.erase(m_buf.begin(), m_buf.begin() + 4);
        if (m_expected > 1024U * 1024U) {
          m_eof = true;
          return std::nullopt;
        }
        m_state = State::Body;
      }

      if (m_buf.size() < m_expected) {
        std::vector<std::uint8_t> tmp(std::max<std::size_t>(64, m_expected - m_buf.size()));
        const ssize_t n = ::read(STDIN_FILENO, tmp.data(), tmp.size());
        if (n < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return std::nullopt;
          }
          m_eof = true;
          return std::nullopt;
        }
        if (n == 0) {
          m_eof = true;
          return std::nullopt;
        }
        m_buf.insert(m_buf.end(), tmp.begin(), tmp.begin() + n);
        continue;
      }

      const std::string body(reinterpret_cast<const char*>(m_buf.data()), m_expected);
      m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(m_expected));
      m_state = State::Length;
      m_expected = 0;
      try {
        return nlohmann::json::parse(body);
      } catch (...) {
        return nlohmann::json{{"action", "action:invalid"}};
      }
    }
  }

} // namespace noctalia::theme::firefox_theme::native_messaging
