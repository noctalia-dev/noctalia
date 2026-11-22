#pragma once

#include "test_check.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nythera::test {

  template <typename Fn> class ScopeExit {
  public:
    explicit ScopeExit(Fn fn) noexcept : m_fn(std::move(fn)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&& other) noexcept
        : m_fn(std::move(other.m_fn)), m_active(std::exchange(other.m_active, false)) {}
    ScopeExit& operator=(ScopeExit&&) = delete;
    ~ScopeExit() noexcept {
      if (m_active)
        m_fn();
    }
    void release() noexcept { m_active = false; }

  private:
    Fn m_fn;
    bool m_active = true;
  };

  template <typename Fn> [[nodiscard]] inline ScopeExit<Fn> makeScopeExit(Fn fn) noexcept {
    return ScopeExit<Fn>(std::move(fn));
  }

  [[nodiscard]] inline bool
  isPathWithinRoot(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    if (candidate.empty() || root.empty())
      return false;
    const auto normCandidate = candidate.lexically_normal();
    const auto normRoot = root.lexically_normal();
    auto rootIt = normRoot.begin();
    auto candIt = normCandidate.begin();
    for (; rootIt != normRoot.end(); ++rootIt, ++candIt) {
      if (rootIt->empty() || *rootIt == ".")
        continue;
      while (candIt != normCandidate.end() && (candIt->empty() || *candIt == "."))
        ++candIt;
      if (candIt == normCandidate.end() || *rootIt != *candIt)
        return false;
    }
    return true;
  }

  [[nodiscard]] inline std::string uniqueToken(std::string_view prefix) {
    std::random_device rd;
    const std::uint64_t high = static_cast<std::uint64_t>(rd());
    const std::uint64_t low = static_cast<std::uint64_t>(rd());
    const std::uint64_t entropy = (high << 32U) ^ low;
    return std::format("{}-{}-{:016x}", prefix, static_cast<unsigned long>(::getpid()), entropy);
  }

  class ScopedEnvVar {
  public:
    ScopedEnvVar(const char* name, const char* value) : m_name(name != nullptr ? name : "") {
      TEST_CHECK(!m_name.empty());
      if (const char* existing = std::getenv(m_name.c_str()); existing != nullptr)
        m_previous = std::string(existing);
      if (value != nullptr)
        TEST_CHECK(::setenv(m_name.c_str(), value, 1) == 0);
      else
        TEST_CHECK(::unsetenv(m_name.c_str()) == 0);
    }

    explicit ScopedEnvVar(const char* name, const std::string& value) : ScopedEnvVar(name, value.c_str()) {}

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

    ScopedEnvVar(ScopedEnvVar&& other) noexcept
        : m_name(std::move(other.m_name)), m_previous(std::move(other.m_previous)),
          m_active(std::exchange(other.m_active, false)) {}

    ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

    ~ScopedEnvVar() noexcept {
      if (!m_active || m_name.empty())
        return;
      if (m_previous.has_value())
        ::setenv(m_name.c_str(), m_previous->c_str(), 1);
      else
        ::unsetenv(m_name.c_str());
    }

  private:
    std::string m_name;
    std::optional<std::string> m_previous;
    bool m_active = true;
  };

  class ScopedCurrentPath {
  public:
    ScopedCurrentPath() {
      std::error_code ec;
      m_previous = std::filesystem::current_path(ec);
      if (ec)
        m_previous.clear();
    }
    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;
    ~ScopedCurrentPath() noexcept {
      if (!m_previous.empty()) {
        std::error_code ec;
        std::filesystem::current_path(m_previous, ec);
      }
    }

  private:
    std::filesystem::path m_previous;
  };

  namespace detail {

    inline void restoreOwnedDirPermissions(const std::filesystem::path& dir, const std::filesystem::path& fixtureRoot) {
      if (!isPathWithinRoot(dir, fixtureRoot))
        return;
      struct stat st{};
      if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid())
        return;
      if ((st.st_mode & 0700U) != 0700U)
        ::chmod(dir.c_str(), 0700);
      const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0)
        return;
      struct stat openedStat{};
      if (::fstat(fd, &openedStat) != 0 || openedStat.st_dev != st.st_dev || openedStat.st_ino != st.st_ino) {
        ::close(fd);
        return;
      }
      DIR* stream = ::fdopendir(fd);
      if (stream == nullptr) {
        ::close(fd);
        return;
      }
      while (const dirent* entry = ::readdir(stream)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
          continue;
        const auto child = dir / entry->d_name;
        struct stat childStat{};
        if (::lstat(child.c_str(), &childStat) == 0 && S_ISDIR(childStat.st_mode) && childStat.st_uid == ::geteuid())
          restoreOwnedDirPermissions(child, fixtureRoot);
      }
      ::closedir(stream);
    }

    inline void removeOwnedTree(const std::filesystem::path& path) noexcept {
      if (path.empty() || path == "/")
        return;
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
      if (!ec)
        return;
      restoreOwnedDirPermissions(path, path);
      ec.clear();
      std::filesystem::remove_all(path, ec);
    }

    [[nodiscard]] inline bool isValidSingleComponent(std::string_view name) noexcept {
      return !name.empty()
          && name != "."
          && name != ".."
          && name.find('/') == std::string_view::npos
          && name.find('\0') == std::string_view::npos;
    }

  } // namespace detail

  class ScopedTempDir {
  public:
    explicit ScopedTempDir(
        std::string_view prefix, const std::filesystem::path& parent = std::filesystem::temp_directory_path()
    ) {
      TEST_CHECK(detail::isValidSingleComponent(prefix));
      std::error_code ec;
      TEST_CHECK(!parent.empty() && std::filesystem::is_directory(parent, ec) && !ec);
      std::string tmpl = (parent / (std::string(prefix) + "-XXXXXX")).string();
      char* created = ::mkdtemp(tmpl.data());
      TEST_CHECK(created != nullptr);
      m_path = created;
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ScopedTempDir(ScopedTempDir&& other) noexcept : m_path(std::move(other.m_path)) { other.m_path.clear(); }
    ScopedTempDir& operator=(ScopedTempDir&&) = delete;

    ~ScopedTempDir() noexcept { detail::removeOwnedTree(m_path); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    [[nodiscard]] operator const std::filesystem::path&() const noexcept { return m_path; }

  private:
    std::filesystem::path m_path;
  };

  class ScopedOwnedDir {
  public:
    ScopedOwnedDir(const std::filesystem::path& parent, std::string_view exactName, bool requireCreation = true) {
      TEST_CHECK(detail::isValidSingleComponent(exactName));
      TEST_CHECK(!parent.empty() && parent.is_absolute());
      std::error_code ec;
      const bool parentIsDir = std::filesystem::is_directory(parent, ec) && !ec;
      if (!parentIsDir) {
        TEST_CHECK(!requireCreation);
        return;
      }
      const auto candidate = parent / exactName;
      ec.clear();
      const bool created = std::filesystem::create_directory(candidate, ec);
      if (!created || ec) {
        TEST_CHECK(!requireCreation);
        return;
      }
      m_path = candidate;
    }

    ScopedOwnedDir(const ScopedOwnedDir&) = delete;
    ScopedOwnedDir& operator=(const ScopedOwnedDir&) = delete;
    ScopedOwnedDir(ScopedOwnedDir&& other) noexcept : m_path(std::move(other.m_path)) { other.m_path.clear(); }
    ScopedOwnedDir& operator=(ScopedOwnedDir&&) = delete;

    ~ScopedOwnedDir() noexcept { detail::removeOwnedTree(m_path); }

    [[nodiscard]] bool created() const noexcept { return !m_path.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

  private:
    std::filesystem::path m_path;
  };

  struct TestEnvOptions {
    std::string_view prefix = "nythera-test";
    bool isolateDbusSession = true;
    bool requirePrivateDbusSession = false;
    bool isolateVfs = true;
    bool forbidProductionWastebin = true;
    bool setXdgDirs = false;
  };

  class ScopedTestEnv {
  public:
    explicit ScopedTestEnv(TestEnvOptions options = {})
        : m_realUserHome([] {
            const char* rawHome = std::getenv("HOME");
            return rawHome != nullptr && *rawHome != '\0' ? std::filesystem::path(rawHome) : std::filesystem::path{};
          }()),
          m_root(options.prefix) {
      TEST_CHECK(!(options.isolateDbusSession && options.requirePrivateDbusSession));

      m_home = m_root.path() / "home";
      m_config = m_root.path() / "config";
      m_state = m_root.path() / "state";
      m_data = m_root.path() / "data";
      m_cache = m_root.path() / "cache";
      m_runtime = m_root.path() / "runtime";
      m_work = m_root.path() / "work";

      for (const auto* dir : {&m_home, &m_config, &m_state, &m_data, &m_cache, &m_runtime, &m_work})
        TEST_CHECK(std::filesystem::create_directory(*dir));
      TEST_CHECK(::chmod(m_runtime.c_str(), 0700) == 0);

      m_env.emplace_back("HOME", m_home.string());
      m_env.emplace_back("NYTHERA_CONFIG_HOME", m_config.string());
      m_env.emplace_back("XDG_CONFIG_HOME", m_config.string());
      m_env.emplace_back("NYTHERA_STATE_HOME", m_state.string());
      m_env.emplace_back("XDG_STATE_HOME", m_state.string());
      m_env.emplace_back("NYTHERA_DATA_HOME", m_data.string());
      m_env.emplace_back("XDG_DATA_HOME", m_data.string());
      m_env.emplace_back("XDG_CACHE_HOME", m_cache.string());
      m_env.emplace_back("XDG_RUNTIME_DIR", m_runtime.string());

      if (options.setXdgDirs) {
        m_env.emplace_back("XDG_DATA_DIRS", m_data.string());
        m_env.emplace_back("XDG_CONFIG_DIRS", m_config.string());
      }
      if (options.isolateVfs) {
        m_env.emplace_back("GIO_USE_VFS", "local");
        m_env.emplace_back("GVFS_DISABLE_FUSE", "1");
      }
      if (options.forbidProductionWastebin) {
        m_env.emplace_back("NYTHERA_TEST_FORBID_PRODUCTION_WASTEBIN", "1");
        m_env.emplace_back("NYTHERA_TEST_FIXTURE_ROOT", m_root.path().string());
        m_env.emplace_back("NYTHERA_TEST_ISOLATED_TRASH_ROOT", m_data.string());
      }
      if (options.isolateDbusSession) {
        const auto disabledBus = "unix:path=" + (m_runtime / "disabled-bus").string();
        m_env.emplace_back("DBUS_SESSION_BUS_ADDRESS", disabledBus);
        m_env.emplace_back("DBUS_STARTER_ADDRESS", nullptr);
        m_env.emplace_back("DBUS_STARTER_BUS_TYPE", nullptr);
      } else if (options.requirePrivateDbusSession) {
        const char* marker = std::getenv("NYTHERA_TEST_PRIVATE_DBUS");
        TEST_CHECK(marker != nullptr && std::string_view(marker) == "1");
        const char* busAddress = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        TEST_CHECK(busAddress != nullptr && *busAddress != '\0');
        const auto liveUserBus = "/run/user/" + std::to_string(static_cast<unsigned long>(::geteuid())) + "/bus";
        TEST_CHECK(std::string_view(busAddress).find(liveUserBus) == std::string_view::npos);
      }
    }

    ScopedTestEnv(const ScopedTestEnv&) = delete;
    ScopedTestEnv& operator=(const ScopedTestEnv&) = delete;
    ScopedTestEnv(ScopedTestEnv&&) = delete;
    ScopedTestEnv& operator=(ScopedTestEnv&&) = delete;
    ~ScopedTestEnv() noexcept = default;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root.path(); }
    [[nodiscard]] const std::filesystem::path& home() const noexcept { return m_home; }
    [[nodiscard]] const std::filesystem::path& config() const noexcept { return m_config; }
    [[nodiscard]] const std::filesystem::path& state() const noexcept { return m_state; }
    [[nodiscard]] const std::filesystem::path& data() const noexcept { return m_data; }
    [[nodiscard]] const std::filesystem::path& cache() const noexcept { return m_cache; }
    [[nodiscard]] const std::filesystem::path& runtime() const noexcept { return m_runtime; }
    [[nodiscard]] const std::filesystem::path& work() const noexcept { return m_work; }
    [[nodiscard]] const std::filesystem::path& realUserHome() const noexcept { return m_realUserHome; }

  private:
    std::filesystem::path m_realUserHome;
    ScopedTempDir m_root;
    std::filesystem::path m_home;
    std::filesystem::path m_config;
    std::filesystem::path m_state;
    std::filesystem::path m_data;
    std::filesystem::path m_cache;
    std::filesystem::path m_runtime;
    std::filesystem::path m_work;
    std::vector<ScopedEnvVar> m_env;
  };

  [[nodiscard]] inline bool validateRealNfsMountRoot(
      const char* rawPath, const std::filesystem::path& realUserHome, std::filesystem::path& canonicalOut,
      std::string& errorOut
  ) {
    canonicalOut.clear();
    errorOut.clear();
    if (rawPath == nullptr || *rawPath == '\0') {
      errorOut = "NFS mount path must be non-empty.";
      return false;
    }
    const std::filesystem::path candidate(rawPath);
    if (!candidate.is_absolute()) {
      errorOut = "NFS mount path must be absolute.";
      return false;
    }
    for (const auto& component : candidate) {
      if (component == "..") {
        errorOut = "NFS mount path must not contain '..' components.";
        return false;
      }
    }
    std::error_code ec;
    const auto symlinkStat = std::filesystem::symlink_status(candidate, ec);
    if (ec || !std::filesystem::exists(symlinkStat)) {
      errorOut = "NFS mount path does not exist.";
      return false;
    }
    const auto canonical = std::filesystem::canonical(candidate, ec);
    if (ec || canonical.empty() || !std::filesystem::is_directory(canonical, ec) || ec) {
      errorOut = "NFS mount path is not a valid existing directory.";
      return false;
    }
    if (::access(canonical.c_str(), W_OK | X_OK) != 0) {
      errorOut = "NFS mount path is not writable and searchable.";
      return false;
    }

    static constexpr std::array<const char*, 16> kForbiddenRoots{
        "/",     "/mnt", "/media", "/tmp", "/var/tmp", "/dev",  "/dev/shm", "/home",
        "/root", "/etc", "/usr",   "/var", "/boot",    "/proc", "/sys",     "/run",
    };
    for (const char* forbidden : kForbiddenRoots) {
      if (canonical == std::filesystem::path(forbidden)) {
        errorOut = std::format("NFS mount path {} is a forbidden system/mount root.", canonical.string());
        return false;
      }
    }

    if (const auto tempRoot = std::filesystem::temp_directory_path(ec); !ec && !tempRoot.empty()) {
      const auto tempCanon = std::filesystem::canonical(tempRoot, ec);
      if (!ec && !tempCanon.empty() && isPathWithinRoot(canonical, tempCanon)) {
        errorOut = "NFS mount path must not reside inside the local temp directory.";
        return false;
      }
    }

    if (!realUserHome.empty()) {
      ec.clear();
      const auto homeCanon = std::filesystem::canonical(realUserHome, ec);
      const auto& homeRef = (!ec && !homeCanon.empty()) ? homeCanon : realUserHome.lexically_normal();
      if (isPathWithinRoot(canonical, homeRef) || isPathWithinRoot(homeRef, canonical)) {
        errorOut = "NFS mount path must not be inside or an ancestor of the user's HOME.";
        return false;
      }
    }

    struct stat rootStat{};
    struct stat mountStat{};
    if (::stat("/", &rootStat) != 0 || ::stat(canonical.c_str(), &mountStat) != 0) {
      errorOut = "Could not stat '/' or the NFS mount path.";
      return false;
    }
    if (mountStat.st_dev == rootStat.st_dev) {
      errorOut = "NFS mount path must reside on a mounted filesystem distinct from '/'.";
      return false;
    }

    canonicalOut = canonical;
    return true;
  }

} // namespace nythera::test
