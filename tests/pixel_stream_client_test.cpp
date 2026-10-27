#include "ipc/pixel_stream_client.h"
#include "test_check.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {
  constexpr std::uint32_t kBuffer = 1;
  constexpr std::uint32_t kFrame = 2;

  void putLe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    }
  }

  class Companion {
  public:
    Companion() {
      std::array<char, 40> directory{};
      std::strcpy(directory.data(), "/tmp/noctalia-pixel-stream.XXXXXX");
      TEST_CHECK(::mkdtemp(directory.data()) != nullptr);
      m_directory = directory.data();
      path = m_directory + "/stream.sock";
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      m_listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
      TEST_CHECK(m_listener >= 0);
      TEST_CHECK(::bind(m_listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
      TEST_CHECK(::listen(m_listener, 4) == 0);
    }
    ~Companion() {
      closePeer();
      ::close(m_listener);
      ::unlink(path.c_str());
      ::rmdir(m_directory.c_str());
    }
    Companion(const Companion&) = delete;
    Companion& operator=(const Companion&) = delete;

    // Connects `client` and consumes its HELLO and first REQUEST.
    void accept(PixelStreamClient& client) {
      client.setPath("");
      client.setPath(path);
      client.tick(std::chrono::steady_clock::now());
      TEST_CHECK(client.connected());
      m_peer = ::accept4(m_listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      TEST_CHECK(m_peer >= 0);
      expect(1, PixelStreamClient::kProtocolVersion);
      expect(2, 0);
    }

    void expect(std::uint32_t type, std::uint32_t argument) const {
      std::array<std::uint8_t, 16> message{};
      TEST_CHECK(::recv(m_peer, message.data(), message.size(), MSG_DONTWAIT) == 8);
      std::uint32_t gotType = 0;
      std::uint32_t gotArgument = 0;
      for (std::size_t i = 0; i < 4; ++i) {
        gotType |= static_cast<std::uint32_t>(message[i]) << (8U * i);
        gotArgument |= static_cast<std::uint32_t>(message[4 + i]) << (8U * i);
      }
      TEST_CHECK(gotType == type);
      TEST_CHECK(gotArgument == argument);
    }

    void expectNothing() const {
      std::array<std::uint8_t, 16> message{};
      TEST_CHECK(::recv(m_peer, message.data(), message.size(), MSG_DONTWAIT) < 0);
    }

    void send(const std::vector<std::uint32_t>& fields, int fd = -1) const {
      std::vector<std::uint8_t> payload;
      for (const std::uint32_t field : fields) {
        putLe32(payload, field);
      }
      iovec vector{.iov_base = payload.data(), .iov_len = payload.size()};
      msghdr message{};
      message.msg_iov = &vector;
      message.msg_iovlen = 1;
      alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
      if (fd >= 0) {
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &fd, sizeof(int));
      }
      TEST_CHECK(::sendmsg(m_peer, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));
    }

    void closePeer() {
      if (m_peer >= 0) {
        ::close(m_peer);
        m_peer = -1;
      }
    }

    std::string path;

  private:
    std::string m_directory;
    int m_listener = -1;
    int m_peer = -1;
  };

  // A memfd holding width*height RGBA pixels filled with `value`.
  int makeBuffer(std::uint32_t width, std::uint32_t height, std::uint8_t value, bool seal = true) {
    const int fd = ::memfd_create("pixel-stream-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    TEST_CHECK(fd >= 0);
    const std::vector<std::uint8_t> pixels(std::size_t{width} * height * 4, value);
    TEST_CHECK(::write(fd, pixels.data(), pixels.size()) == static_cast<ssize_t>(pixels.size()));
    if (seal) {
      TEST_CHECK(::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0);
    }
    return fd;
  }

  // Attaches a sealed buffer and asserts the connection survived.
  void attach(Companion& companion, PixelStreamClient& client, std::uint32_t id, std::uint8_t value) {
    const int fd = makeBuffer(2, 1, value);
    companion.send({kBuffer, id, 2, 1}, fd);
    ::close(fd);
    client.dispatch(POLLIN);
    TEST_CHECK(client.connected());
  }

  // Runs one protocol violation on a fresh connection and checks it disconnects.
  template <typename Violation>
  void expectRejected(Companion& companion, PixelStreamClient& client, Violation violation) {
    companion.accept(client);
    violation();
    client.dispatch(POLLIN);
    TEST_CHECK(!client.connected());
    TEST_CHECK(!client.frame().has_value());
  }
} // namespace

int main() {
  Companion companion;
  PixelStreamClient client;
  int changes = 0;
  client.setChangeCallback([&changes] { ++changes; });

  // A frame arrives only in answer to a request and stays readable until released.
  companion.accept(client);
  attach(companion, client, 0, 7);
  attach(companion, client, 1, 9);
  TEST_CHECK(!client.frame().has_value());
  companion.send({kFrame, 1});
  client.dispatch(POLLIN);
  TEST_CHECK(changes == 1);
  const auto frame = client.frame();
  TEST_CHECK(frame.has_value());
  TEST_CHECK(frame->width == 2 && frame->height == 1);
  TEST_CHECK(frame->pixels[0] == 9 && frame->pixels[7] == 9);
  companion.expectNothing();
  client.releaseFrame();
  TEST_CHECK(!client.frame().has_value());
  companion.expect(2, 0);

  // A released buffer may be replaced; the replacement is what the next FRAME shows.
  attach(companion, client, 1, 3);
  companion.send({kFrame, 1});
  client.dispatch(POLLIN);
  TEST_CHECK(client.frame().has_value() && client.frame()->pixels[0] == 3);

  // The companion going away disconnects, drops the frame, and schedules a retry.
  companion.closePeer();
  client.dispatch(POLLIN | POLLHUP);
  TEST_CHECK(!client.connected());
  TEST_CHECK(!client.frame().has_value());
  TEST_CHECK(client.retryAt().has_value());
  const auto now = std::chrono::steady_clock::now();
  TEST_CHECK(*client.retryAt() > now);
  client.tick(now);
  TEST_CHECK(!client.connected());

  // Protocol violations close the connection.
  expectRejected(companion, client, [&] { companion.send({kFrame, 0}); }); // unattached buffer
  expectRejected(companion, client, [&] {
    attach(companion, client, 0, 1);
    companion.send({kFrame, 0});
    client.dispatch(POLLIN);
    companion.send({kFrame, 0}); // second FRAME without a REQUEST
  });
  expectRejected(companion, client, [&] {
    attach(companion, client, 0, 1);
    companion.send({kFrame, 0});
    client.dispatch(POLLIN);
    const int fd = makeBuffer(2, 1, 1);
    companion.send({kBuffer, 0, 2, 1}, fd); // replaces the buffer the shell holds
    ::close(fd);
  });
  expectRejected(companion, client, [&] {
    const int fd = makeBuffer(2, 1, 1, false);
    companion.send({kBuffer, 0, 2, 1}, fd); // shrinkable memfd
    ::close(fd);
  });
  expectRejected(companion, client, [&] {
    const int fd = makeBuffer(2, 1, 1);
    companion.send({kBuffer, 0, 2, 2}, fd); // file smaller than the pixels
    ::close(fd);
  });
  expectRejected(companion, client, [&] {
    const int fd = makeBuffer(2, 1, 1);
    companion.send({kBuffer, 0, 4097, 1}, fd); // dimension above the limit
    ::close(fd);
  });
  expectRejected(companion, client, [&] {
    const int fd = makeBuffer(2, 1, 1);
    companion.send({kBuffer, PixelStreamClient::kMaxBuffers, 2, 1}, fd); // id out of range
    ::close(fd);
  });
  expectRejected(companion, client, [&] { companion.send({kBuffer, 0, 2, 1}); }); // BUFFER without its fd
  expectRejected(companion, client, [&] { companion.send({9, 0}); });             // unknown message

  // An empty path stops streaming and never retries.
  companion.accept(client);
  client.setPath("");
  TEST_CHECK(!client.connected());
  TEST_CHECK(!client.retryAt().has_value());
  return EXIT_SUCCESS;
}
