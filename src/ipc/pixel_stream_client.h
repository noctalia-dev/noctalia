#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Shell side of the pixel stream companion protocol (documented for plugin authors in
// docs/user/plugins/development/declarative-ui.mdx, "Pixel streams").
//
// The companion listens on a filesystem SOCK_SEQPACKET socket. All fields are little-endian
// uint32. The shell sends HELLO {1, version} once and REQUEST {2, 0} whenever it is ready for a
// frame. The companion attaches shared buffers with BUFFER {1, id, width, height} carrying one
// sealed memfd (RGBA8, tightly packed rows), and answers each REQUEST with FRAME {2, id}. The
// buffer named by a FRAME stays owned by the shell until its next REQUEST.
class PixelStreamClient {
public:
  struct Frame {
    const std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
  };
  using ChangeCallback = std::function<void()>;

  static constexpr std::uint32_t kProtocolVersion = 1;
  static constexpr std::size_t kMaxBuffers = 4;
  static constexpr std::uint32_t kMaxDimension = 4096;
  static constexpr std::size_t kMaxPixels = std::size_t{4096} * 1024;
  static constexpr std::chrono::milliseconds kRetryInterval{1000};

  PixelStreamClient() = default;
  ~PixelStreamClient();
  PixelStreamClient(const PixelStreamClient&) = delete;
  PixelStreamClient& operator=(const PixelStreamClient&) = delete;

  // Called after a new frame arrived or the connection dropped.
  void setChangeCallback(ChangeCallback callback) { m_onChange = std::move(callback); }

  // Returns false when the path is unchanged. A new path drops the connection; the first
  // connection attempt is due immediately. An empty path stops streaming.
  bool setPath(std::string path);
  [[nodiscard]] const std::string& path() const noexcept { return m_path; }

  [[nodiscard]] int fd() const noexcept { return m_fd; }
  [[nodiscard]] bool connected() const noexcept { return m_fd >= 0; }
  // When the next connection attempt is due; empty while connected or without a path.
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> retryAt() const;
  // Attempts a connection once retryAt() has passed.
  void tick(std::chrono::steady_clock::time_point now);
  // Handles poll() results for fd().
  void dispatch(short revents);

  // The most recent frame, valid until releaseFrame(), a disconnect, or a path change.
  [[nodiscard]] std::optional<Frame> frame() const;
  // Returns the frame's buffer to the companion and requests the next frame.
  void releaseFrame();
  // Drops the connection and reconnects immediately; a new connection receives the current
  // frame right away. Used when the consumer lost its copy (graphics context reset).
  void reconnect();

private:
  struct Buffer {
    void* data = nullptr;
    std::size_t size = 0;
    int width = 0;
    int height = 0;
  };

  void connect();
  void disconnect();
  void fail(std::string_view reason);
  bool send(std::uint32_t type, std::uint32_t argument);
  // Reads one message. Returns false when the socket has nothing more to read or failed.
  bool readMessage();
  void attachBuffer(std::uint32_t id, std::uint32_t width, std::uint32_t height, int fd);
  void unmap(Buffer& buffer);
  void notify();

  std::string m_path;
  int m_fd = -1;
  std::chrono::steady_clock::time_point m_retryAt;
  std::array<Buffer, kMaxBuffers> m_buffers{};
  std::optional<std::uint32_t> m_frameBuffer;
  bool m_requested = false;
  ChangeCallback m_onChange;
};
