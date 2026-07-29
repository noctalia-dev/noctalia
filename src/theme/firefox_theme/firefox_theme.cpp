#include "theme/firefox_theme/firefox_theme.h"

#include "theme/firefox_theme/css.h"
#include "theme/firefox_theme/native_messaging.h"
#include "theme/firefox_theme/settings.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace noctalia::theme {
  namespace {

    namespace css = firefox_theme::css;
    namespace native_messaging = firefox_theme::native_messaging;
    namespace settings = firefox_theme::settings;

    // Wire names required by the Pywalfox Firefox/Thunderbird extension.
    constexpr std::string_view kExtensionId = "pywalfox@frewacom.org";
    constexpr std::string_view kManifestName = "pywalfox";
    constexpr std::string_view kHostVersion = "noctalia-2.9.0-compat";

    constexpr std::string_view kActionVersion = "debug:version";
    constexpr std::string_view kActionColors = "action:colors";
    constexpr std::string_view kActionInvalid = "action:invalid";
    constexpr std::string_view kActionCssEnable = "css:enable";
    constexpr std::string_view kActionCssDisable = "css:disable";
    constexpr std::string_view kActionCssFontSize = "css:font:size";
    constexpr std::string_view kActionThemeMode = "theme:mode";

    constexpr std::string_view kCmdUpdate = "action:update";
    constexpr std::string_view kCmdDark = "theme:mode:dark";
    constexpr std::string_view kCmdLight = "theme:mode:light";
    constexpr std::string_view kCmdAuto = "theme:mode:auto";

    [[nodiscard]] std::filesystem::path homeDir() {
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return home;
      }
      return {};
    }

    [[nodiscard]] std::filesystem::path xdgCacheHome() {
      if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache != nullptr && cache[0] != '\0') {
        return cache;
      }
      const auto home = homeDir();
      return home.empty() ? std::filesystem::path{} : home / ".cache";
    }

    [[nodiscard]] std::string selfExePath() {
      char buf[4096];
      const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
      if (n <= 0) {
        return {};
      }
      buf[n] = '\0';
      return std::string(buf, static_cast<std::size_t>(n));
    }

    bool setCloexec(int fd) {
      const int flags = ::fcntl(fd, F_GETFD);
      if (flags < 0) {
        return false;
      }
      return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
    }

    bool setNonBlocking(int fd) {
      const int flags = ::fcntl(fd, F_GETFL);
      if (flags < 0) {
        return false;
      }
      return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    [[nodiscard]] std::filesystem::path defaultColorsJsonPath() {
      const auto cache = xdgCacheHome();
      return cache.empty() ? std::filesystem::path{} : cache / "wal" / "colors.json";
    }

    [[nodiscard]] std::filesystem::path unixSocketPath() {
      // Must match the historical Pywalfox host socket name.
      return std::filesystem::temp_directory_path() / ("pywalfox_socket_" + std::to_string(::getuid()));
    }

    // Per-uid fan-out channel: every running host watches this file so theme pushes
    // reach all Firefox profiles, not only the process that owns the Unix socket.
    [[nodiscard]] std::filesystem::path commandNotifyDir() {
      if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && runtime[0] != '\0') {
        return std::filesystem::path(runtime) / "noctalia" / "firefox-theme";
      }
      return std::filesystem::temp_directory_path() / ("noctalia-firefox-theme-" + std::to_string(::getuid()));
    }

    [[nodiscard]] std::filesystem::path commandNotifyPath() { return commandNotifyDir() / "command"; }

    bool publishCommand(std::string_view command) {
      const auto dir = commandNotifyDir();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec) {
        return false;
      }

      const auto path = commandNotifyPath();
      const auto tmp = dir / ("command.tmp." + std::to_string(::getpid()));
      {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
          return false;
        }
        out << command;
        if (!out.flush()) {
          return false;
        }
      }
      std::filesystem::rename(tmp, path, ec);
      if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
      }
      return true;
    }

    [[nodiscard]] std::optional<std::string> readPublishedCommand() {
      std::ifstream in(commandNotifyPath());
      if (!in) {
        return std::nullopt;
      }
      std::string line;
      if (!std::getline(in, line)) {
        return std::nullopt;
      }
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
      }
      if (line.empty()) {
        return std::nullopt;
      }
      return line;
    }

    [[nodiscard]] bool isUnixDatagramInUse(const std::filesystem::path& path) {
      std::error_code ec;
      if (!std::filesystem::exists(path, ec) || ec) {
        return false;
      }
      const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        return false;
      }
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      const std::string pathStr = path.string();
      if (pathStr.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
      }
      std::memcpy(addr.sun_path, pathStr.c_str(), pathStr.size() + 1);
      const bool inUse = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
      ::close(fd);
      return inUse;
    }

    [[nodiscard]] std::filesystem::path userManifestPath() {
      const auto home = homeDir();
      if (home.empty()) {
        return {};
      }
      return home / ".mozilla" / "native-messaging-hosts" / "pywalfox.json";
    }

    [[nodiscard]] std::filesystem::path resolveNoctaliaExecutable() {
      const std::string self = selfExePath();
      if (!self.empty()) {
        return self;
      }
#ifdef NOCTALIA_INSTALL_PREFIX
      const auto installed = std::filesystem::path(NOCTALIA_INSTALL_PREFIX) / "bin" / "noctalia";
      std::error_code ec;
      if (std::filesystem::is_regular_file(installed, ec)) {
        return installed;
      }
#endif
      return {};
    }

    [[nodiscard]] bool pathLooksLikeNoctaliaHost(const std::filesystem::path& path) {
      const std::string name = path.filename().string();
      return name == "noctalia" || name == "noctalia-pywalfox";
    }

    [[nodiscard]] std::optional<std::filesystem::path> readExistingManifestHostPath() {
      const auto manifest = userManifestPath();
      if (manifest.empty()) {
        return std::nullopt;
      }
      std::ifstream in(manifest);
      if (!in) {
        return std::nullopt;
      }
      try {
        nlohmann::json root;
        in >> root;
        if (!root.contains("path") || !root["path"].is_string()) {
          return std::nullopt;
        }
        return std::filesystem::path(root["path"].get<std::string>());
      } catch (...) {
        return std::nullopt;
      }
    }

    bool installManifest(const std::filesystem::path& hostExecutable, std::string* error) {
      if (hostExecutable.empty() || !std::filesystem::is_regular_file(hostExecutable)) {
        if (error != nullptr) {
          *error = "noctalia executable not found";
        }
        return false;
      }

      std::error_code ec;
      const auto canonical = std::filesystem::weakly_canonical(hostExecutable, ec);
      const auto path = ec ? hostExecutable : canonical;

      const auto manifest = userManifestPath();
      if (manifest.empty()) {
        if (error != nullptr) {
          *error = "HOME is not set";
        }
        return false;
      }

      std::filesystem::create_directories(manifest.parent_path(), ec);
      if (ec) {
        if (error != nullptr) {
          *error = "failed to create native-messaging-hosts directory: " + ec.message();
        }
        return false;
      }

      const nlohmann::json body = {
          {"name", std::string(kManifestName)},
          {"description", "Noctalia Firefox theme native messaging host"},
          {"path", path.string()},
          {"type", "stdio"},
          {"allowed_extensions", nlohmann::json::array({std::string(kExtensionId)})},
      };

      std::ofstream out(manifest, std::ios::trunc);
      if (!out) {
        if (error != nullptr) {
          *error = "failed to write " + manifest.string();
        }
        return false;
      }
      out << body.dump(2) << '\n';
      return true;
    }

    bool uninstallManifest(std::string* error) {
      const auto manifest = userManifestPath();
      if (manifest.empty()) {
        if (error != nullptr) {
          *error = "HOME is not set";
        }
        return false;
      }
      std::error_code ec;
      if (!std::filesystem::exists(manifest, ec)) {
        return true;
      }
      if (!std::filesystem::remove(manifest, ec)) {
        if (error != nullptr) {
          *error = "failed to remove " + manifest.string() + ": " + ec.message();
        }
        return false;
      }
      return true;
    }

    // Install only when absent or already owned by noctalia — never clobber a foreign host.
    bool ensureManifestOwnedByNoctalia(std::string* error, std::string* warning) {
      const auto host = resolveNoctaliaExecutable();
      if (host.empty()) {
        if (error != nullptr) {
          *error = "noctalia executable not found";
        }
        return false;
      }

      if (const auto existing = readExistingManifestHostPath()) {
        if (!pathLooksLikeNoctaliaHost(*existing)) {
          if (warning != nullptr) {
            *warning = "leaving existing native messaging host at " + existing->string();
          }
          return true;
        }
      }

      return installManifest(host, error);
    }

    bool trySendSocketCommand(std::string_view command) {
      const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        return false;
      }

      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      const auto path = unixSocketPath().string();
      if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
      }
      std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

      if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
      }

      const ssize_t sent = ::send(fd, command.data(), command.size(), 0);
      ::close(fd);
      return sent >= 0 && static_cast<std::size_t>(sent) == command.size();
    }

    // Publish to every running host via the shared command file, and also nudge the
    // process that owns the legacy Pywalfox Unix socket (if any).
    bool notifyRunningHosts(std::string_view command, std::string* warning) {
      const bool published = publishCommand(command);
      const bool socketOk = trySendSocketCommand(command);
      if (published || socketOk) {
        return true;
      }
      if (warning != nullptr && warning->empty()) {
        *warning = "Firefox theme host is not running; colors will apply when the extension connects";
      }
      return false;
    }

    int sendHostCommand(std::string_view command) {
      if (notifyRunningHosts(command, nullptr)) {
        return 0;
      }
      std::println(stderr, "failed to notify Firefox theme hosts (no command file write and no socket)");
      return 1;
    }

    struct ColorsPayload {
      std::vector<std::string> colors;
      std::string wallpaper;
      std::string error;
      bool ok = false;
    };

    [[nodiscard]] ColorsPayload loadColors(const std::filesystem::path& path) {
      ColorsPayload out;
      std::ifstream in(path);
      if (!in) {
        out.error = "Could not read colors from: " + path.string();
        return out;
      }

      nlohmann::json root;
      try {
        in >> root;
      } catch (const std::exception& e) {
        out.error = std::string("Failed to read colors: ") + e.what();
        return out;
      }

      if (!root.contains("colors") || !root["colors"].is_object()) {
        out.error = path.string() + " does not contain any color values";
        return out;
      }
      if (!root.contains("wallpaper") || !root["wallpaper"].is_string()) {
        out.error = path.string() + " does not contain a wallpaper path";
        return out;
      }

      const auto& colorsObj = root["colors"];
      for (int i = 0; i < 16; ++i) {
        const std::string key = "color" + std::to_string(i);
        if (colorsObj.contains(key) && colorsObj[key].is_string()) {
          out.colors.push_back(colorsObj[key].get<std::string>());
        }
      }
      if (out.colors.size() < 16) {
        out.colors.clear();
        for (auto it = colorsObj.begin(); it != colorsObj.end(); ++it) {
          if (it.value().is_string()) {
            out.colors.push_back(it.value().get<std::string>());
          }
        }
      }

      if (out.colors.size() < 16) {
        out.error = path.string() + " containing the generated Pywal colors is invalid";
        return out;
      }

      out.wallpaper = root["wallpaper"].get<std::string>();
      out.ok = true;
      return out;
    }

    void sendMessage(const nlohmann::json& message) { native_messaging::writeMessage(message); }

    void sendVersion() {
      sendMessage({{"action", std::string(kActionVersion)}, {"success", true}, {"data", std::string(kHostVersion)}});
    }

    void sendColorsFromPath(const std::filesystem::path& path) {
      const auto payload = loadColors(path);
      nlohmann::json msg = {{"action", std::string(kActionColors)}, {"success", payload.ok}};
      if (payload.ok) {
        msg["data"] = {{"colors", payload.colors}, {"wallpaper", payload.wallpaper}};
      } else {
        msg["error"] = payload.error;
      }
      sendMessage(msg);
    }

    void sendColors() { sendColorsFromPath(defaultColorsJsonPath()); }

    void sendInvalid() { sendMessage({{"action", std::string(kActionInvalid)}, {"success", false}}); }

    void sendThemeMode(std::string_view mode) {
      sendMessage({{"action", std::string(kActionThemeMode)}, {"success", true}, {"data", std::string(mode)}});
    }

    void handleExtensionMessage(const nlohmann::json& message, bool& persistedStateSent) {
      if (!message.contains("action") || !message["action"].is_string()) {
        sendInvalid();
        return;
      }
      const std::string action = message["action"].get<std::string>();
      if (action == kActionVersion) {
        sendVersion();
        return;
      }
      if (action == kActionColors) {
        sendColors();
        if (!persistedStateSent) {
          persistedStateSent = true;
          if (const auto mode = settings::get("theme_mode"); mode.has_value() && !mode->empty()) {
            sendThemeMode(*mode);
          }
        }
        return;
      }
      if (action == kActionCssEnable || action == kActionCssDisable || action == kActionCssFontSize) {
        if (!message.contains("target")
            || !message["target"].is_string()
            || message["target"].get<std::string>().empty()) {
          sendInvalid();
          return;
        }
        const std::string target = message["target"].get<std::string>();
        std::string err;
        const auto chrome = css::firefoxChromePath(&err);
        if (!chrome) {
          sendMessage(
              {{"action", action},
               {"success", false},
               {"data", target},
               {"error", err.empty() ? "Could not find path to chrome folder" : err}}
          );
          return;
        }

        bool ok = false;
        std::string messageText;
        nlohmann::json data = target;
        if (action == kActionCssEnable) {
          ok = css::enable(*chrome, target, &messageText);
        } else if (action == kActionCssDisable) {
          ok = css::disable(*chrome, target, &messageText);
        } else {
          if (!message.contains("size")) {
            sendInvalid();
            return;
          }
          int size = 0;
          if (message["size"].is_number_integer()) {
            size = message["size"].get<int>();
          } else if (message["size"].is_string()) {
            try {
              size = std::stoi(message["size"].get<std::string>());
            } catch (...) {
              sendInvalid();
              return;
            }
          } else {
            sendInvalid();
            return;
          }
          ok = css::setFontSize(*chrome, target, size, &messageText);
          data = size;
        }
        nlohmann::json msg = {{"action", action}, {"success", ok}, {"data", data}};
        if (ok) {
          msg["message"] = messageText;
        } else {
          msg["error"] = messageText;
        }
        sendMessage(msg);
        return;
      }
      sendInvalid();
    }

    void handleSocketCommand(std::string_view command) {
      if (command == kCmdUpdate) {
        sendColors();
        return;
      }
      if (command == kCmdDark) {
        settings::set("theme_mode", "dark");
        sendThemeMode("dark");
        return;
      }
      if (command == kCmdLight) {
        settings::set("theme_mode", "light");
        sendThemeMode("light");
        return;
      }
      if (command == kCmdAuto) {
        settings::set("theme_mode", "auto");
        sendThemeMode("auto");
      }
    }

    // Legacy socket traffic (e.g. older tools that only know the Unix socket): apply locally
    // and republish so secondary profile hosts pick it up via the shared command file.
    void handleSocketCommandFromPrimary(std::string_view command) {
      handleSocketCommand(command);
      (void)publishCommand(command);
    }

    [[nodiscard]] int openUnixDatagramServer(const std::filesystem::path& path) {
      // Match legacy pywalfox: never steal a live socket. Extra Firefox profiles run as
      // secondaries and rely on the shared command file (+ colors.json inotify).
      if (isUnixDatagramInUse(path)) {
        return -1;
      }
      ::unlink(path.c_str());
      const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        return -1;
      }
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      const std::string pathStr = path.string();
      if (pathStr.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return -1;
      }
      std::memcpy(addr.sun_path, pathStr.c_str(), pathStr.size() + 1);
      if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
      }
      ::chmod(path.c_str(), 0600);
      (void)setNonBlocking(fd);
      return fd;
    }

    [[nodiscard]] int openPathInotify(const std::filesystem::path& watchedPath, int* watchFdOut) {
      *watchFdOut = -1;
      const int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
      if (fd < 0) {
        return -1;
      }
      const auto dir = watchedPath.parent_path();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const int wd = ::inotify_add_watch(fd, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
      if (wd < 0) {
        ::close(fd);
        return -1;
      }
      *watchFdOut = wd;
      return fd;
    }

    // Drain one inotify fd; returns whether `fileName` was among the events.
    [[nodiscard]] bool drainInotifyMatches(int inotifyFd, std::string_view fileName) {
      alignas(inotify_event) char buf[4096];
      bool matched = false;
      while (true) {
        const ssize_t n = ::read(inotifyFd, buf, sizeof(buf));
        if (n < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }
          break;
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(n)) {
          const auto* event = reinterpret_cast<const inotify_event*>(buf + offset);
          if (event->len > 0 && fileName == event->name) {
            matched = true;
          }
          offset += sizeof(inotify_event) + event->len;
        }
      }
      return matched;
    }

    void printCliHelp() {
      std::println(
          "noctalia firefox-theme — Firefox theme host helpers (Pywalfox-compatible)\n"
          "\n"
          "Usage: noctalia firefox-theme <ACTION>\n"
          "\n"
          "Actions:\n"
          "  host           Run as Firefox native messaging host\n"
          "  install        Install user-local native messaging manifest\n"
          "  uninstall      Remove user-local native messaging manifest\n"
          "  update         Ask all running hosts to push colors to their extensions\n"
          "  dark|light|auto  Persist and push theme mode to all running hosts\n"
          "  -h, --help     Show this help\n"
          "\n"
          "Templates use post_action = \"firefox-theme\" after writing colors.json.\n"
          "Firefox still requires the Pywalfox browser extension.\n"
          "Multiple Firefox profiles are supported: each profile runs a host process,\n"
          "and theme pushes fan out to all of them.\n"
      );
    }

  } // namespace

  FirefoxThemeApplyResult applyFirefoxTheme(const std::filesystem::path& colorsJsonPath, std::string_view mode) {
    FirefoxThemeApplyResult result;
    if (colorsJsonPath.empty()) {
      result.error = "colors.json path is empty";
      return result;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(colorsJsonPath, ec)) {
      result.error = "colors.json not found: " + colorsJsonPath.string();
      return result;
    }

    const auto payload = loadColors(colorsJsonPath);
    if (!payload.ok) {
      result.error = payload.error;
      return result;
    }

    std::string ensureError;
    std::string ensureWarning;
    if (!ensureManifestOwnedByNoctalia(&ensureError, &ensureWarning)) {
      result.error = ensureError;
      return result;
    }
    if (!ensureWarning.empty()) {
      result.warning = ensureWarning;
    }

    // Match community apply.sh: pywalfox dark|light (when valid), then pywalfox update.
    // publishCommand fans out to every profile host; the Unix socket nudges the primary.
    if (mode == "dark" || mode == "light") {
      firefox_theme::settings::set("theme_mode", mode);
      (void)notifyRunningHosts(mode == "dark" ? kCmdDark : kCmdLight, &result.warning);
    }
    (void)notifyRunningHosts(kCmdUpdate, &result.warning);
    result.success = true;
    return result;
  }

  bool isFirefoxNativeMessagingLaunch(int argc, char* argv[]) {
    if (argc < 2 || argv[1] == nullptr) {
      return false;
    }
    const std::string_view arg = argv[1];
    if (arg == kExtensionId) {
      return true;
    }
    // Firefox may pass an absolute chrome/extension path as argv[1] and the id as argv[2].
    if (argc >= 3 && argv[2] != nullptr && std::string_view(argv[2]) == kExtensionId) {
      return true;
    }
    return false;
  }

  int runFirefoxNativeMessagingHost() {
    (void)setNonBlocking(STDIN_FILENO);
    (void)setCloexec(STDIN_FILENO);

    const auto socketPath = unixSocketPath();
    // Only one host per uid owns the legacy socket; additional profiles skip bind.
    const int socketFd = openUnixDatagramServer(socketPath);

    int colorsWatch = -1;
    const auto colorsPath = defaultColorsJsonPath();
    const int colorsInotifyFd = openPathInotify(colorsPath, &colorsWatch);
    const std::string colorsFileName = colorsPath.filename().string();

    int commandWatch = -1;
    const auto commandPath = commandNotifyPath();
    const int commandInotifyFd = openPathInotify(commandPath, &commandWatch);
    const std::string commandFileName = commandPath.filename().string();

    native_messaging::StdinReader reader;
    bool persistedStateSent = false;

    while (true) {
      std::array<pollfd, 4> fds{};
      nfds_t nfds = 0;
      fds[nfds++] = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
      if (socketFd >= 0) {
        fds[nfds++] = {.fd = socketFd, .events = POLLIN, .revents = 0};
      }
      if (colorsInotifyFd >= 0) {
        fds[nfds++] = {.fd = colorsInotifyFd, .events = POLLIN, .revents = 0};
      }
      if (commandInotifyFd >= 0) {
        fds[nfds++] = {.fd = commandInotifyFd, .events = POLLIN, .revents = 0};
      }

      const int ready = ::poll(fds.data(), nfds, -1);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }

      for (nfds_t i = 0; i < nfds; ++i) {
        if (fds[i].revents == 0) {
          continue;
        }
        if (fds[i].fd == STDIN_FILENO) {
          if ((fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (fds[i].revents & POLLIN) == 0) {
            goto shutdown;
          }
          while (true) {
            const auto msg = reader.tryRead();
            if (!msg.has_value()) {
              if (reader.eof()) {
                goto shutdown;
              }
              break;
            }
            handleExtensionMessage(*msg, persistedStateSent);
          }
        } else if (socketFd >= 0 && fds[i].fd == socketFd) {
          char buf[1024];
          while (true) {
            const ssize_t n = ::recv(socketFd, buf, sizeof(buf), 0);
            if (n < 0) {
              if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
              }
              break;
            }
            if (n == 0) {
              break;
            }
            handleSocketCommandFromPrimary(std::string_view(buf, static_cast<std::size_t>(n)));
          }
        } else if (colorsInotifyFd >= 0 && fds[i].fd == colorsInotifyFd) {
          if (drainInotifyMatches(colorsInotifyFd, colorsFileName)) {
            sendColors();
          }
        } else if (commandInotifyFd >= 0 && fds[i].fd == commandInotifyFd) {
          if (drainInotifyMatches(commandInotifyFd, commandFileName)) {
            if (const auto command = readPublishedCommand()) {
              handleSocketCommand(*command);
            }
          }
        }
      }
    }

  shutdown:
    if (socketFd >= 0) {
      ::close(socketFd);
      ::unlink(socketPath.c_str());
    }
    if (colorsInotifyFd >= 0) {
      ::close(colorsInotifyFd);
    }
    if (commandInotifyFd >= 0) {
      ::close(commandInotifyFd);
    }
    return 0;
  }

  int runFirefoxThemeCli(int argc, char* argv[]) {
    std::string_view action;
    if (argc >= 3 && argv[2] != nullptr) {
      action = argv[2];
    }

    if (action.empty() || action == "-h" || action == "--help" || action == "help") {
      printCliHelp();
      return action.empty() ? 1 : 0;
    }
    if (action == "host" || action == "start") {
      return runFirefoxNativeMessagingHost();
    }
    if (action == "install") {
      const auto host = resolveNoctaliaExecutable();
      std::string err;
      if (!installManifest(host, &err)) {
        std::println(stderr, "install failed: {}", err);
        return 1;
      }
      std::println("Installed native messaging host manifest:");
      std::println("  {}", userManifestPath().string());
      std::println("  path = {}", host.string());
      std::println("Restart Firefox if it was already running.");
      return 0;
    }
    if (action == "uninstall") {
      std::string err;
      if (!uninstallManifest(&err)) {
        std::println(stderr, "uninstall failed: {}", err);
        return 1;
      }
      std::println("Removed {}", userManifestPath().string());
      return 0;
    }
    if (action == "update") {
      return sendHostCommand(kCmdUpdate);
    }
    if (action == "dark") {
      settings::set("theme_mode", "dark");
      return sendHostCommand(kCmdDark);
    }
    if (action == "light") {
      settings::set("theme_mode", "light");
      return sendHostCommand(kCmdLight);
    }
    if (action == "auto") {
      settings::set("theme_mode", "auto");
      return sendHostCommand(kCmdAuto);
    }

    std::println(stderr, "error: unknown firefox-theme action '{}'", action);
    printCliHelp();
    return 1;
  }

} // namespace noctalia::theme
