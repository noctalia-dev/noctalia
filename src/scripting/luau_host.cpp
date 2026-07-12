#include "scripting/luau_host.h"

#include "compositors/compositor_platform.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "net/http_client.h"
#include "notification/notifications.h"
#include "render/text/font_registry.h"
#include "scripting/plugin_bindings.h"
#include "scripting/plugin_state_store.h"
#include "scripting/script_api_context.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/icon_resolver.h"
#include "system/terminal_launch.h"
#include "time/time_format.h"
#include "util/file_utils.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
  Logger kLog{"luau"};
  constexpr const char* kHostKey = "__noctalia_host";
  constexpr auto kDefaultCommandTimeout = std::chrono::milliseconds(5000);
  constexpr auto kMinCommandTimeout = std::chrono::milliseconds(50);
  constexpr auto kMaxCommandTimeout = std::chrono::milliseconds(60000);
  constexpr std::size_t kMaxAsyncCommandOutputBytes = 1024 * 1024;
  constexpr std::size_t kMaxAsyncCommandsPerHost = 8;
  constexpr int kMaxGlobalAsyncCommands = 32;
  constexpr std::size_t kMaxAsyncProcessMatchesPerHost = 16;
  constexpr int kMaxGlobalAsyncProcessMatches = 64;
  constexpr int kMaxGlobalDetachedCommands = 32;
  constexpr std::size_t kMaxAsyncHttpPerHost = 8;
  constexpr std::size_t kMaxStreamsPerHost = 4;
  // A single stream line can't exceed this; protects against a process spewing one
  // unbounded line with no newline.
  constexpr std::size_t kMaxStreamLineBytes = 64 * 1024;
  // Per-plugin VM heap ceiling. Far above any legitimate plugin's working set, so
  // it only ever trips on a runaway allocation (an unbounded table/string loop).
  constexpr std::size_t kMemoryCeilingBytes = 128 * 1024 * 1024;

  std::uint64_t& nextHostId() {
    static std::uint64_t id = 1;
    return id;
  }

  std::atomic<int>& inFlightAsyncCommands() {
    static std::atomic<int> count{0};
    return count;
  }

  std::atomic<int>& inFlightAsyncProcessMatches() {
    static std::atomic<int> count{0};
    return count;
  }

  std::atomic<int>& inFlightDetachedCommands() {
    static std::atomic<int> count{0};
    return count;
  }

  bool acquireDetachedCommandSlot() {
    auto& globalInFlight = inFlightDetachedCommands();
    int current = globalInFlight.load(std::memory_order_relaxed);
    while (current < kMaxGlobalDetachedCommands) {
      if (globalInFlight.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  void releaseDetachedCommandSlot() { inFlightDetachedCommands().fetch_sub(1, std::memory_order_relaxed); }

  bool startDetachedCommandAsync(std::string command) {
    if (command.empty()) {
      return false;
    }
    if (!acquireDetachedCommandSlot()) {
      return false;
    }

    try {
      std::thread([command = std::move(command)]() mutable {
        try {
          (void)process::runAsync(command);
        } catch (...) {
        }
        releaseDetachedCommandSlot();
      }).detach();
    } catch (...) {
      releaseDetachedCommandSlot();
      return false;
    }

    return true;
  }

  bool startDetachedProcessAsync(std::vector<std::string> args) {
    if (args.empty() || args.front().empty()) {
      return false;
    }
    if (!acquireDetachedCommandSlot()) {
      return false;
    }

    try {
      std::thread([args = std::move(args)]() mutable {
        try {
          (void)process::runAsync(args);
        } catch (...) {
        }
        releaseDetachedCommandSlot();
      }).detach();
    } catch (...) {
      releaseDetachedCommandSlot();
      return false;
    }

    return true;
  }

  bool startDetachedCommandInTerminalAsync(std::string command) {
    auto prepared = terminal_launch::prepareCommand(command);
    return prepared.has_value() && startDetachedProcessAsync(std::move(*prepared));
  }

  std::chrono::milliseconds commandTimeoutFromLua(lua_State* L) {
    const double rawTimeout = luaL_optnumber(
        L, 3, static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultCommandTimeout).count())
    );
    const double timeoutMs =
        std::isfinite(rawTimeout) ? rawTimeout : static_cast<double>(kDefaultCommandTimeout.count());
    const double bounded = std::clamp(
        timeoutMs, static_cast<double>(kMinCommandTimeout.count()), static_cast<double>(kMaxCommandTimeout.count())
    );
    return std::chrono::milliseconds(static_cast<int>(bounded));
  }

  void budgetInterrupt(lua_State* L, int /*gc*/) {
    auto* host = static_cast<LuauHost*>(lua_callbacks(L)->userdata);
    if (host != nullptr) {
      host->interruptIfBudgetExceeded(L);
    }
  }

  void setTableInteger(lua_State* L, const char* key, int value) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
  }

  void setTableString(lua_State* L, const char* key, const std::string& value) {
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, key);
  }

  void setTableBool(lua_State* L, const char* key, bool value) {
    lua_pushboolean(L, value ? 1 : 0);
    lua_setfield(L, -2, key);
  }

  LuauHost* hostForState(lua_State* L) {
    lua_getglobal(L, kHostKey);
    auto* host = static_cast<LuauHost*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return host;
  }

  int luau_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    if (auto* host = hostForState(L)) {
      host->scriptLog(msg);
    } else {
      kLog.info("{}", msg);
    }
    return 0;
  }

  int luau_runAsync(lua_State* L) {
    size_t len = 0;
    const char* cmd = luaL_checklstring(L, 1, &len);
    std::string command(cmd, len);

    if (lua_isnoneornil(L, 2)) {
      bool ok = startDetachedCommandAsync(std::move(command));
      lua_pushboolean(L, ok ? 1 : 0);
      return 1;
    }

    luaL_checktype(L, 2, LUA_TFUNCTION);

    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      return 1;
    }

    const auto timeout = commandTimeoutFromLua(L);
    const int callbackRef = lua_ref(L, 2);
    bool ok = host->startAsyncCommand(std::move(command), callbackRef, timeout);
    if (!ok) {
      lua_unref(L, callbackRef);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  int luau_runStream(lua_State* L) {
    size_t len = 0;
    const char* cmd = luaL_checklstring(L, 1, &len);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      return 1;
    }

    const int callbackRef = lua_ref(L, 2);
    bool ok = host->startStream(std::string(cmd, len), callbackRef);
    if (!ok) {
      lua_unref(L, callbackRef);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  int luau_runInTerminal(lua_State* L) {
    size_t len = 0;
    const char* cmd = luaL_checklstring(L, 1, &len);
    bool ok = startDetachedCommandInTerminalAsync(std::string(cmd, len));
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  int luau_commandExists(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, process::commandExists(name) ? 1 : 0);
    return 1;
  }

  int luau_flatpakAppInstalled(lua_State* L) {
    size_t len = 0;
    const char* appId = luaL_checklstring(L, 1, &len);
    lua_pushboolean(L, process::flatpakAppInstalled(std::string_view(appId, len)) ? 1 : 0);
    return 1;
  }

  int luau_portalAvailable(lua_State* L) {
    lua_pushboolean(L, process::desktopPortalAvailable() ? 1 : 0);
    return 1;
  }

  int luau_focusedOutputName(lua_State* L) {
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushnil(L);
      return 1;
    }

    if (auto name = host->scriptFocusedOutputName(); name.has_value() && !name->empty()) {
      lua_pushlstring(L, name->data(), name->size());
      return 1;
    }

    if (host->platform() == nullptr) {
      lua_pushnil(L);
      return 1;
    }

    wl_output* output = host->platform()->preferredInteractiveOutput();
    const auto* info = host->platform()->findOutputByWl(output);
    if (info == nullptr || info->connectorName.empty()) {
      lua_pushnil(L);
      return 1;
    }

    lua_pushlstring(L, info->connectorName.data(), info->connectorName.size());
    return 1;
  }

  int luau_outputs(lua_State* L) {
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_newtable(L);
      return 1;
    }
    const auto outputs = host->api().outputs();
    lua_createtable(L, static_cast<int>(outputs.size()), 0);
    int index = 1;
    for (const auto& out : outputs) {
      lua_createtable(L, 0, 8);
      setTableString(L, "name", out.name);
      setTableString(L, "description", out.description);
      setTableInteger(L, "width", out.width);
      setTableInteger(L, "height", out.height);
      setTableInteger(L, "x", out.x);
      setTableInteger(L, "y", out.y);
      setTableInteger(L, "scale", out.scale);
      setTableBool(L, "focused", out.focused);
      lua_rawseti(L, -2, index++);
    }
    return 1;
  }

  // appIconPath(appIdOrIconName, sizePx?) -> absolute icon file path or nil.
  // Same resolution the native taskbar uses: desktop-entry lookup (id /
  // StartupWMClass) for the icon name, then the XDG icon-theme resolver.
  // Unmatched inputs are treated as raw icon names so plugins can also
  // resolve themed icons directly.
  int luau_appIconPath(lua_State* L) {
    size_t len = 0;
    const char* appId = luaL_checklstring(L, 1, &len);
    const int targetSize = luaL_optinteger(L, 2, 0);

    std::string iconName;
    const auto entries = desktopEntriesSnapshot();
    if (const auto entry = app_identity::findDesktopEntry(std::string_view(appId, len), *entries);
        entry.has_value() && !entry->icon.empty()) {
      iconName = entry->icon;
    } else {
      iconName.assign(appId, len);
    }

    // One resolver (and icon-path cache) per script worker thread; the theme
    // plan it reads is shared across threads and mutex-guarded in IconResolver.
    static thread_local IconResolver resolver;
    const std::string& path = resolver.resolve(iconName, targetSize);
    if (path.empty()) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushlstring(L, path.data(), path.size());
    return 1;
  }

  int luau_setWallpaperEnabled(lua_State* L) {
    size_t len = 0;
    const char* connector = luaL_checklstring(L, 1, &len);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    const bool enabled = lua_toboolean(L, 2) != 0;
    if (auto* host = hostForState(L)) {
      host->scriptSetWallpaperEnabled(std::string(connector, len), enabled);
    }
    return 0;
  }

  // setWallpaper(path) or setWallpaper(connector, path) — apply and persist a
  // wallpaper image. With one argument it targets all outputs.
  int luau_setWallpaper(lua_State* L) {
    std::string connector;
    std::string path;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
      size_t connectorLen = 0;
      const char* connectorStr = luaL_checklstring(L, 1, &connectorLen);
      size_t pathLen = 0;
      const char* pathStr = luaL_checklstring(L, 2, &pathLen);
      connector.assign(connectorStr, connectorLen);
      path.assign(pathStr, pathLen);
    } else {
      size_t pathLen = 0;
      const char* pathStr = luaL_checklstring(L, 1, &pathLen);
      path.assign(pathStr, pathLen);
    }
    if (auto* host = hostForState(L)) {
      host->scriptSetWallpaper(std::move(connector), std::move(path));
    }
    return 0;
  }

  // togglePanel("author/plugin:panel") — toggle a host panel by id.
  int luau_togglePanel(lua_State* L) {
    size_t len = 0;
    const char* panelId = luaL_checklstring(L, 1, &len);
    if (auto* host = hostForState(L)) {
      host->scriptTogglePanel(std::string(panelId, len));
    }
    return 0;
  }

  int luau_isDarkMode(lua_State* L) {
    auto* host = hostForState(L);
    lua_pushboolean(L, host != nullptr && host->api().isDarkMode() ? 1 : 0);
    return 1;
  }

  int luau_wallpaperDirectory(lua_State* L) {
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    const std::string directory = host->api().wallpaperDirectory();
    if (directory.empty()) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushlstring(L, directory.data(), directory.size());
    return 1;
  }

  int luau_processMatches(lua_State* L) {
    const int count = lua_gettop(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);

    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      return 1;
    }

    std::vector<std::string> needles;
    needles.reserve(static_cast<std::size_t>(std::max(0, count - 1)));
    for (int i = 2; i <= count; ++i) {
      size_t len = 0;
      const char* needle = luaL_checklstring(L, i, &len);
      needles.emplace_back(needle, len);
    }

    const int callbackRef = lua_ref(L, 1);
    bool ok = host->startAsyncProcessMatch(std::move(needles), callbackRef);
    if (!ok) {
      lua_unref(L, callbackRef);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  int luau_notify(lua_State* L) {
    const char* title = luaL_checkstring(L, 1);
    const char* body = luaL_optstring(L, 2, "");
    if (auto* host = hostForState(L)) {
      host->scriptNotifyInfo(title, body);
    } else {
      notify::info("Noctalia", title, body);
    }
    return 0;
  }

  int luau_notifyError(lua_State* L) {
    const char* title = luaL_checkstring(L, 1);
    const char* body = luaL_optstring(L, 2, "");
    if (auto* host = hostForState(L)) {
      host->scriptNotifyError(title, body);
    } else {
      notify::error("Noctalia", title, body);
    }
    return 0;
  }

  int luau_copyToClipboard(lua_State* L) {
    size_t textLen = 0;
    const char* text = luaL_checklstring(L, 1, &textLen);
    size_t mimeLen = 0;
    const char* mimeType = luaL_checklstring(L, 2, &mimeLen);

    bool ok = textLen > 0 && mimeLen > 0;
    if (ok) {
      if (auto* host = hostForState(L)) {
        ok = host->scriptCopyToClipboard(std::string(text, textLen), std::string(mimeType, mimeLen));
      } else {
        ok = false;
      }
    }

    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  int luau_clipboardText(lua_State* L) {
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    if (const auto text = host->api().clipboardText(); text.has_value()) {
      lua_pushlstring(L, text->data(), text->size());
    } else {
      lua_pushnil(L);
    }
    return 1;
  }

  int luau_getenv(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* val = std::getenv(name);
    if (val)
      lua_pushstring(L, val);
    else
      lua_pushnil(L);
    return 1;
  }

  int luau_expandPath(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    const std::string expanded = FileUtils::expandUserPath(std::string(path, len)).string();
    lua_pushlstring(L, expanded.data(), expanded.size());
    return 1;
  }

  int luau_formatTime(lua_State* L) {
    size_t patternLen = 0;
    const char* pattern = luaL_checklstring(L, 1, &patternLen);

    std::int64_t unixSeconds = 0;
    if (lua_isnoneornil(L, 2)) {
      unixSeconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    } else {
      const double raw = luaL_checknumber(L, 2);
      if (!std::isfinite(raw)) {
        luaL_argerror(L, 2, "expected finite unix timestamp");
      }
      unixSeconds = static_cast<std::int64_t>(raw);
    }

    const std::string result = formatLocalUnixTime(unixSeconds, std::string_view(pattern, patternLen));
    lua_pushlstring(L, result.data(), result.size());
    return 1;
  }

  int luau_setUpdateInterval(lua_State* L) {
    const int ms = static_cast<int>(luaL_checknumber(L, 1));
    if (auto* host = hostForState(L)) {
      host->scriptSetUpdateInterval(ms);
    }
    return 0;
  }

  // Filesystem path resolution: ~ -> $HOME, absolute paths verbatim, otherwise relative
  // to the plugin's own directory. No sandbox — the trust model allows any path.
  std::filesystem::path resolveHostPath(LuauHost* host, std::string_view path) {
    if (path.empty()) {
      return {};
    }
    if (path[0] == '~') {
      return FileUtils::expandUserPath(std::string(path));
    }
    if (path[0] == '/') {
      return std::filesystem::path(path);
    }
    return host->pluginDir() / path;
  }

  int luau_readFile(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushnil(L);
      lua_pushstring(L, "no host");
      return 2;
    }
    std::ifstream file(resolveHostPath(host, std::string_view(path, len)), std::ios::binary);
    if (!file) {
      lua_pushnil(L);
      lua_pushstring(L, "cannot open file");
      return 2;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string contents = ss.str();
    lua_pushlstring(L, contents.data(), contents.size());
    return 1;
  }

  int luau_loadFont(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushnil(L);
      lua_pushstring(L, "no host");
      return 2;
    }
    const std::string family = text::registerFontFile(resolveHostPath(host, std::string_view(path, len)));
    if (family.empty()) {
      lua_pushnil(L);
      lua_pushstring(L, "failed to load font");
      return 2;
    }
    lua_pushlstring(L, family.data(), family.size());
    return 1;
  }

  int luau_writeFile(lua_State* L) {
    size_t pathLen = 0;
    const char* path = luaL_checklstring(L, 1, &pathLen);
    size_t dataLen = 0;
    const char* data = luaL_checklstring(L, 2, &dataLen);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "no host");
      return 2;
    }
    std::ofstream file(resolveHostPath(host, std::string_view(path, pathLen)), std::ios::binary | std::ios::trunc);
    if (!file) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "cannot open file for writing");
      return 2;
    }
    file.write(data, static_cast<std::streamsize>(dataLen));
    lua_pushboolean(L, file.good() ? 1 : 0);
    return 1;
  }

  int luau_mkdirAll(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "no host");
      return 2;
    }
    const std::filesystem::path dir = resolveHostPath(host, std::string_view(path, len));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, ec.message().c_str());
      return 2;
    }
    if (!std::filesystem::is_directory(dir, ec)) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "path exists and is not a directory");
      return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  int luau_removeFile(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "no host");
      return 2;
    }
    const std::filesystem::path file = resolveHostPath(host, std::string_view(path, len));
    std::error_code ec;
    if (std::filesystem::is_directory(file, ec)) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "is a directory");
      return 2;
    }
    if (!std::filesystem::remove(file, ec)) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, ec ? ec.message().c_str() : "no such file");
      return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  int luau_renameFile(lua_State* L) {
    size_t fromLen = 0;
    const char* from = luaL_checklstring(L, 1, &fromLen);
    size_t toLen = 0;
    const char* to = luaL_checklstring(L, 2, &toLen);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "no host");
      return 2;
    }
    std::error_code ec;
    std::filesystem::rename(
        resolveHostPath(host, std::string_view(from, fromLen)), resolveHostPath(host, std::string_view(to, toLen)), ec
    );
    if (ec) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, ec.message().c_str());
      return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  int luau_fileInfo(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushnil(L);
      lua_pushstring(L, "no host");
      return 2;
    }
    const std::filesystem::path target = resolveHostPath(host, std::string_view(path, len));
    std::error_code ec;
    const auto status = std::filesystem::status(target, ec);
    if (ec || !std::filesystem::exists(status)) {
      lua_pushnil(L);
      lua_pushstring(L, "no such path");
      return 2;
    }
    const bool isDir = std::filesystem::is_directory(status);
    double size = 0.0;
    if (!isDir) {
      if (const auto bytes = std::filesystem::file_size(target, ec); !ec) {
        size = static_cast<double>(bytes);
      }
    }
    double mtime = 0.0;
    if (const auto writeTime = std::filesystem::last_write_time(target, ec); !ec) {
      const auto sysTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          writeTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
      );
      mtime = std::chrono::duration<double>(sysTime.time_since_epoch()).count();
    }
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, size);
    lua_setfield(L, -2, "size");
    lua_pushnumber(L, mtime);
    lua_setfield(L, -2, "mtime");
    lua_pushboolean(L, isDir ? 1 : 0);
    lua_setfield(L, -2, "isDir");
    return 1;
  }

  int luau_fileExists(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      return 1;
    }
    std::error_code ec;
    lua_pushboolean(L, std::filesystem::exists(resolveHostPath(host, std::string_view(path, len)), ec) ? 1 : 0);
    return 1;
  }

  int luau_listDir(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushnil(L);
      lua_pushstring(L, "no host");
      return 2;
    }
    const std::filesystem::path dir = resolveHostPath(host, std::string_view(path, len));
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
      lua_pushnil(L);
      lua_pushstring(L, "not a directory");
      return 2;
    }
    lua_createtable(L, 0, 0);
    int index = 1;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) {
        break;
      }
      const std::string name = entry.path().filename().string();
      lua_pushlstring(L, name.data(), name.size());
      lua_rawseti(L, -2, index++);
    }
    return 1;
  }

  int luau_pluginDir(lua_State* L) {
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    const std::string dir = host->pluginDir().string();
    lua_pushlstring(L, dir.data(), dir.size());
    return 1;
  }

  std::string numberToString(double n) {
    if (std::isfinite(n) && n == std::floor(n)) {
      return std::to_string(static_cast<long long>(n));
    }
    return std::to_string(n);
  }

  // Read an optional `{ name = value }` substitutions table at stack index `idx`.
  std::unordered_map<std::string, std::string> readSubstTable(lua_State* L, int idx) {
    std::unordered_map<std::string, std::string> subst;
    if (lua_gettop(L) < idx || !lua_istable(L, idx)) {
      return subst;
    }
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING) {
        std::string value;
        if (lua_type(L, -1) == LUA_TSTRING) {
          size_t vlen = 0;
          const char* vs = lua_tolstring(L, -1, &vlen);
          value.assign(vs, vlen);
        } else if (lua_type(L, -1) == LUA_TNUMBER) {
          value = numberToString(lua_tonumber(L, -1));
        } else if (lua_type(L, -1) == LUA_TBOOLEAN) {
          value = lua_toboolean(L, -1) != 0 ? "true" : "false";
        }
        subst.emplace(lua_tostring(L, -2), std::move(value));
      }
      lua_pop(L, 1);
    }
    return subst;
  }

  int luau_tr(lua_State* L) {
    size_t keyLen = 0;
    const char* key = luaL_checklstring(L, 1, &keyLen);
    auto* host = hostForState(L);
    const auto subst = readSubstTable(L, 2);
    const std::string result =
        host != nullptr ? host->translate(std::string_view(key, keyLen), subst) : std::string(key, keyLen);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
  }

  int luau_trp(lua_State* L) {
    size_t keyLen = 0;
    const char* key = luaL_checklstring(L, 1, &keyLen);
    const double count = luaL_checknumber(L, 2);
    auto* host = hostForState(L);
    auto subst = readSubstTable(L, 3);
    subst.insert_or_assign("count", numberToString(count));

    const std::string base(key, keyLen);
    if (host == nullptr) {
      lua_pushlstring(L, base.data(), base.size());
      return 1;
    }
    // Plural selection: prefer `<key>.one` / `<key>.other`, else the bare key.
    const std::string variant = base + (count == 1.0 ? ".one" : ".other");
    const std::string useKey = host->hasTranslation(variant) ? variant : base;
    const std::string result = host->translate(useKey, subst);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
  }

  std::string reqStringField(lua_State* L, int tableIdx, const char* key, std::string fallback = {}) {
    lua_getfield(L, tableIdx, key);
    std::string out = lua_isstring(L, -1) ? std::string(lua_tostring(L, -1)) : std::move(fallback);
    lua_pop(L, 1);
    return out;
  }

  bool reqBoolField(lua_State* L, int tableIdx, const char* key, bool fallback) {
    lua_getfield(L, tableIdx, key);
    const bool out = lua_isnil(L, -1) ? fallback : (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    return out;
  }

  int luau_http(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      return 1;
    }

    HttpRequest request;
    request.url = reqStringField(L, 1, "url");
    request.method = reqStringField(L, 1, "method", "GET");
    request.body = reqStringField(L, 1, "body");
    request.basicUsername = reqStringField(L, 1, "basic_username");
    request.basicPassword = reqStringField(L, 1, "basic_password");
    request.followRedirects = reqBoolField(L, 1, "follow_redirects", false);
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1)) {
      const int headersIdx = lua_gettop(L);
      const int count = lua_objlen(L, headersIdx);
      for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, headersIdx, i);
        if (lua_isstring(L, -1)) {
          request.headers.emplace_back(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);

    if (request.url.empty()) {
      lua_pushboolean(L, 0);
      return 1;
    }

    const int callbackRef = lua_ref(L, 2);
    const bool ok = host->startAsyncHttp(std::move(request), callbackRef);
    if (!ok) {
      lua_unref(L, callbackRef);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  int luau_download(lua_State* L) {
    size_t urlLen = 0;
    const char* url = luaL_checklstring(L, 1, &urlLen);
    size_t destLen = 0;
    const char* dest = luaL_checklstring(L, 2, &destLen);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushboolean(L, 0);
      return 1;
    }

    const std::string destPath = resolveHostPath(host, std::string_view(dest, destLen)).string();
    const int callbackRef = lua_ref(L, 3);
    const bool ok = host->startAsyncDownload(std::string(url, urlLen), destPath, callbackRef);
    if (!ok) {
      lua_unref(L, callbackRef);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  // ── Lua <-> JSON (for the shared state store; values cross runtimes as JSON) ──

  nlohmann::json luaToJson(lua_State* L, int idx, int depth = 0) {
    if (depth > 32) {
      return nullptr;
    }
    const int abs = idx > 0 ? idx : lua_gettop(L) + idx + 1;
    switch (lua_type(L, abs)) {
    case LUA_TBOOLEAN:
      return lua_toboolean(L, abs) != 0;
    case LUA_TNUMBER: {
      const double n = lua_tonumber(L, abs);
      if (std::isfinite(n) && n == std::floor(n) && std::abs(n) < 9.007199254740992e15) {
        return static_cast<std::int64_t>(n);
      }
      return n;
    }
    case LUA_TSTRING: {
      size_t len = 0;
      const char* s = lua_tolstring(L, abs, &len);
      return std::string(s, len);
    }
    case LUA_TTABLE: {
      const int len = lua_objlen(L, abs);
      if (len > 0) {
        nlohmann::json array = nlohmann::json::array();
        for (int i = 1; i <= len; ++i) {
          lua_rawgeti(L, abs, i);
          array.push_back(luaToJson(L, -1, depth + 1));
          lua_pop(L, 1);
        }
        return array;
      }
      nlohmann::json object = nlohmann::json::object();
      lua_pushnil(L);
      while (lua_next(L, abs) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
          object[lua_tostring(L, -2)] = luaToJson(L, -1, depth + 1);
        }
        lua_pop(L, 1);
      }
      return object;
    }
    default:
      return nullptr;
    }
  }

  void jsonToLua(lua_State* L, const nlohmann::json& json) {
    switch (json.type()) {
    case nlohmann::json::value_t::boolean:
      lua_pushboolean(L, json.get<bool>() ? 1 : 0);
      break;
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
      lua_pushnumber(L, static_cast<double>(json.get<std::int64_t>()));
      break;
    case nlohmann::json::value_t::number_float:
      lua_pushnumber(L, json.get<double>());
      break;
    case nlohmann::json::value_t::string: {
      const std::string s = json.get<std::string>();
      lua_pushlstring(L, s.data(), s.size());
      break;
    }
    case nlohmann::json::value_t::array: {
      lua_createtable(L, static_cast<int>(json.size()), 0);
      int i = 1;
      for (const auto& item : json) {
        jsonToLua(L, item);
        lua_rawseti(L, -2, i++);
      }
      break;
    }
    case nlohmann::json::value_t::object: {
      lua_createtable(L, 0, static_cast<int>(json.size()));
      for (const auto& [key, value] : json.items()) {
        jsonToLua(L, value);
        lua_setfield(L, -2, key.c_str());
      }
      break;
    }
    default:
      lua_pushnil(L);
      break;
    }
  }

  int luau_state_set(lua_State* L) {
    size_t keyLen = 0;
    const char* key = luaL_checklstring(L, 1, &keyLen);
    auto* host = hostForState(L);
    if (host == nullptr) {
      return 0;
    }
    const nlohmann::json value = lua_gettop(L) >= 2 ? luaToJson(L, 2) : nlohmann::json(nullptr);
    host->stateSet(std::string(key, keyLen), value.dump());
    return 0;
  }

  int luau_state_get(lua_State* L) {
    size_t keyLen = 0;
    const char* key = luaL_checklstring(L, 1, &keyLen);
    auto* host = hostForState(L);
    if (host == nullptr) {
      lua_pushnil(L);
      return 1;
    }
    const auto json = host->stateGet(std::string(key, keyLen));
    if (!json.has_value()) {
      lua_pushnil(L);
      return 1;
    }
    try {
      jsonToLua(L, nlohmann::json::parse(*json));
    } catch (const nlohmann::json::exception&) {
      lua_pushnil(L);
    }
    return 1;
  }

  int luau_state_watch(lua_State* L) {
    size_t keyLen = 0;
    const char* key = luaL_checklstring(L, 1, &keyLen);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto* host = hostForState(L);
    if (host == nullptr) {
      return 0;
    }
    const int callbackRef = lua_ref(L, 2);
    host->stateWatch(std::string(key, keyLen), callbackRef);
    return 0;
  }

  const luaL_Reg kNoctaliaStateLib[] = {
      {"set", luau_state_set},
      {"get", luau_state_get},
      {"watch", luau_state_watch},
      {nullptr, nullptr},
  };

  int luau_json_decode(lua_State* L) {
    size_t len = 0;
    const char* str = luaL_checklstring(L, 1, &len);
    try {
      jsonToLua(L, nlohmann::json::parse(str, str + len));
      return 1;
    } catch (const nlohmann::json::exception& e) {
      lua_pushnil(L);
      lua_pushstring(L, e.what());
      return 2;
    }
  }

  int luau_json_encode(lua_State* L) {
    const nlohmann::json value = lua_gettop(L) >= 1 ? luaToJson(L, 1) : nlohmann::json(nullptr);
    const bool pretty = lua_toboolean(L, 2) != 0;
    try {
      const std::string out = value.dump(pretty ? 2 : -1);
      lua_pushlstring(L, out.data(), out.size());
      return 1;
    } catch (const nlohmann::json::exception& e) {
      lua_pushnil(L);
      lua_pushstring(L, e.what());
      return 2;
    }
  }

  const luaL_Reg kNoctaliaJsonLib[] = {
      {"decode", luau_json_decode},
      {"encode", luau_json_encode},
      {nullptr, nullptr},
  };

  int luau_string_trim(lua_State* L) {
    size_t len = 0;
    const char* str = luaL_checklstring(L, 1, &len);
    const std::string out = StringUtils::trim(std::string_view(str, len));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
  }

  int luau_string_urlEncode(lua_State* L) {
    size_t len = 0;
    const char* str = luaL_checklstring(L, 1, &len);
    const std::string out = StringUtils::urlEncode(std::string_view(str, len));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
  }

  int luau_string_urlDecode(lua_State* L) {
    size_t len = 0;
    const char* str = luaL_checklstring(L, 1, &len);
    const std::string out = StringUtils::urlDecode(std::string_view(str, len));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
  }

  const luaL_Reg kNoctaliaStringLib[] = {
      {"trim", luau_string_trim},
      {"urlEncode", luau_string_urlEncode},
      {"urlDecode", luau_string_urlDecode},
      {nullptr, nullptr},
  };

  int luau_fuzzyScore(lua_State* L) {
    size_t patternLen = 0;
    const char* pattern = luaL_checklstring(L, 1, &patternLen);
    size_t textLen = 0;
    const char* text = luaL_checklstring(L, 2, &textLen);

    const double score = FuzzyMatch::score(std::string_view(pattern, patternLen), std::string_view(text, textLen));
    if (!FuzzyMatch::isMatch(score)) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushnumber(L, score);
    return 1;
  }

  const luaL_Reg kNoctaliaBaseLib[] = {
      {"log", luau_log},
      {"runAsync", luau_runAsync},
      {"runStream", luau_runStream},
      {"runInTerminal", luau_runInTerminal},
      {"commandExists", luau_commandExists},
      {"processMatches", luau_processMatches},
      {"flatpakAppInstalled", luau_flatpakAppInstalled},
      {"portalAvailable", luau_portalAvailable},
      {"focusedOutputName", luau_focusedOutputName},
      {"outputs", luau_outputs},
      {"appIconPath", luau_appIconPath},
      {"setWallpaperEnabled", luau_setWallpaperEnabled},
      {"setWallpaper", luau_setWallpaper},
      {"togglePanel", luau_togglePanel},
      {"isDarkMode", luau_isDarkMode},
      {"wallpaperDirectory", luau_wallpaperDirectory},
      {"notify", luau_notify},
      {"notifyError", luau_notifyError},
      {"copyToClipboard", luau_copyToClipboard},
      {"clipboardText", luau_clipboardText},
      {"getenv", luau_getenv},
      {"expandPath", luau_expandPath},
      {"formatTime", luau_formatTime},
      {"setUpdateInterval", luau_setUpdateInterval},
      {"readFile", luau_readFile},
      {"loadFont", luau_loadFont},
      {"writeFile", luau_writeFile},
      {"mkdirAll", luau_mkdirAll},
      {"removeFile", luau_removeFile},
      {"renameFile", luau_renameFile},
      {"fileExists", luau_fileExists},
      {"fileInfo", luau_fileInfo},
      {"listDir", luau_listDir},
      {"pluginDir", luau_pluginDir},
      {"tr", luau_tr},
      {"trp", luau_trp},
      {"http", luau_http},
      {"download", luau_download},
      {"fuzzyScore", luau_fuzzyScore},
      {"getConfig", scripting::luau_getConfig},
      {nullptr, nullptr},
  };

  void registerNoctaliaLib(lua_State* L) {
    luaL_register(L, "noctalia", kNoctaliaBaseLib);
    // noctalia.state = { set, get, watch }
    lua_createtable(L, 0, 0);
    luaL_register(L, nullptr, kNoctaliaStateLib);
    lua_setfield(L, -2, "state");
    // noctalia.json = { decode, encode }
    lua_createtable(L, 0, 0);
    luaL_register(L, nullptr, kNoctaliaJsonLib);
    lua_setfield(L, -2, "json");
    // noctalia.string = { trim, urlEncode, urlDecode }
    lua_createtable(L, 0, 0);
    luaL_register(L, nullptr, kNoctaliaStringLib);
    lua_setfield(L, -2, "string");
    lua_pop(L, 1);
  }
} // namespace

void* LuauHost::allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
  auto* host = static_cast<LuauHost*>(ud);
  if (nsize == 0) {
    std::free(ptr);
    if (host != nullptr) {
      host->m_memUsed -= osize;
    }
    return nullptr;
  }
  if (host != nullptr && nsize > osize && host->m_memUsed + (nsize - osize) > kMemoryCeilingBytes) {
    return nullptr; // refuse growth past the ceiling -> catchable LUA_ERRMEM
  }
  void* result = std::realloc(ptr, nsize);
  if (result == nullptr) {
    return nullptr; // realloc failed; old block intact, accounting unchanged
  }
  if (host != nullptr) {
    host->m_memUsed += nsize;
    host->m_memUsed -= osize; // osize == 0 for a fresh allocation
  }
  return result;
}

LuauHost::LuauHost(scripting::ScriptApiContext& api, CompositorPlatform* platform) : m_api(api), m_platform(platform) {
  m_hostId = nextHostId()++;

  m_L = lua_newstate(&LuauHost::allocate, this);
  lua_callbacks(m_L)->userdata = this;
  lua_callbacks(m_L)->interrupt = budgetInterrupt;
  luaL_openlibs(m_L);
  registerNoctaliaLib(m_L);
  // Freeze main state's stdlib + globals. The thread we create next inherits
  // reads from this frozen table but gets its own writable globals, so the
  // user script can define `function update()` without touching the parent.
  luaL_sandbox(m_L);

  m_T = lua_newthread(m_L);
  luaL_sandboxthread(m_T);
  lua_pushlightuserdata(m_T, this);
  lua_setglobal(m_T, kHostKey);
  // lua_newthread leaves the thread on the main stack; pin it in the registry
  // so the GC can't collect it, then drop the stack reference.
  m_threadRef = lua_ref(m_L, -1);
  lua_pop(m_L, 1);
}

LuauHost::~LuauHost() {
  // Terminate any long-lived stream subprocesses before tearing down the state.
  stopAllStreams();
  if (m_L) {
    if (m_T != nullptr) {
      for (int callbackRef : m_asyncCommandCallbackRefs) {
        lua_unref(m_T, callbackRef);
      }
      m_asyncCommandCallbackRefs.clear();
      for (int callbackRef : m_asyncProcessMatchCallbackRefs) {
        lua_unref(m_T, callbackRef);
      }
      m_asyncProcessMatchCallbackRefs.clear();
      for (int callbackRef : m_streamCallbackRefs) {
        lua_unref(m_T, callbackRef);
      }
      m_streamCallbackRefs.clear();
    }
    if (m_threadRef != -1)
      lua_unref(m_L, m_threadRef);
    lua_close(m_L);
  }
}

bool LuauHost::startAsyncCommand(std::string command, int callbackRef, std::chrono::milliseconds timeout) {
  if (command.empty() || callbackRef <= LUA_REFNIL || m_asyncCommandCallbackRefs.size() >= kMaxAsyncCommandsPerHost) {
    return false;
  }

  auto& globalInFlight = inFlightAsyncCommands();
  int current = globalInFlight.load(std::memory_order_relaxed);
  while (current < kMaxGlobalAsyncCommands) {
    if (globalInFlight.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
      break;
    }
  }
  if (current >= kMaxGlobalAsyncCommands) {
    return false;
  }

  m_asyncCommandCallbackRefs.insert(callbackRef);
  auto handler = m_asyncCommandResultHandler;
  if (!handler) {
    m_asyncCommandCallbackRefs.erase(callbackRef);
    globalInFlight.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }
  try {
    std::thread([hostId = m_hostId, callbackRef, command = std::move(command), timeout,
                 handler = std::move(handler)]() mutable {
      auto result =
          process::runSyncWithTimeoutAndOutputLimit({"/bin/sh", "-c", command}, timeout, kMaxAsyncCommandOutputBytes);
      inFlightAsyncCommands().fetch_sub(1, std::memory_order_relaxed);
      handler(hostId, callbackRef, std::move(result));
    }).detach();
  } catch (...) {
    m_asyncCommandCallbackRefs.erase(callbackRef);
    globalInFlight.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  return true;
}

bool LuauHost::startAsyncProcessMatch(std::vector<std::string> needles, int callbackRef) {
  if (needles.empty()
      || callbackRef <= LUA_REFNIL
      || m_asyncProcessMatchCallbackRefs.size() >= kMaxAsyncProcessMatchesPerHost) {
    return false;
  }

  if (std::ranges::any_of(needles, [](const auto& needle) { return needle.empty(); })) {
    return false;
  }

  auto& globalInFlight = inFlightAsyncProcessMatches();
  int current = globalInFlight.load(std::memory_order_relaxed);
  while (current < kMaxGlobalAsyncProcessMatches) {
    if (globalInFlight.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
      break;
    }
  }
  if (current >= kMaxGlobalAsyncProcessMatches) {
    return false;
  }

  m_asyncProcessMatchCallbackRefs.insert(callbackRef);
  auto handler = m_asyncProcessMatchResultHandler;
  if (!handler) {
    m_asyncProcessMatchCallbackRefs.erase(callbackRef);
    globalInFlight.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  try {
    std::thread([hostId = m_hostId, callbackRef, needles = std::move(needles), handler = std::move(handler)]() mutable {
      bool matched = false;
      try {
        matched = process::commandLineMatchesAll(needles);
      } catch (...) {
      }
      inFlightAsyncProcessMatches().fetch_sub(1, std::memory_order_relaxed);
      handler(hostId, callbackRef, matched);
    }).detach();
  } catch (...) {
    m_asyncProcessMatchCallbackRefs.erase(callbackRef);
    globalInFlight.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  return true;
}

bool LuauHost::hasAsyncCommandCallback(int callbackRef) const {
  return m_asyncCommandCallbackRefs.contains(callbackRef);
}

bool LuauHost::hasAsyncProcessMatchCallback(int callbackRef) const {
  return m_asyncProcessMatchCallbackRefs.contains(callbackRef);
}

bool LuauHost::hasAsyncHttpCallback(int callbackRef) const { return m_asyncHttpCallbackRefs.contains(callbackRef); }

bool LuauHost::startAsyncHttp(HttpRequest request, int callbackRef) {
  if (m_httpClient == nullptr || callbackRef <= LUA_REFNIL || m_asyncHttpCallbackRefs.size() >= kMaxAsyncHttpPerHost) {
    return false;
  }
  auto handler = m_asyncHttpResultHandler;
  if (!handler) {
    return false;
  }
  m_asyncHttpCallbackRefs.insert(callbackRef);

  // HttpClient must be driven from the main loop; marshal there, then deliver the
  // response back through the handler (which enqueues onto the runtime thread).
  DeferredCall::callLater([client = m_httpClient, request = std::move(request), handler = std::move(handler),
                           hostId = m_hostId, callbackRef]() mutable {
    client->request(std::move(request), [handler, hostId, callbackRef](HttpResponse response) {
      handler(
          hostId, callbackRef, response.transportOk, static_cast<int>(response.status), std::move(response.body), false
      );
    });
  });
  return true;
}

bool LuauHost::startAsyncDownload(std::string url, std::string destPath, int callbackRef) {
  if (m_httpClient == nullptr
      || url.empty()
      || destPath.empty()
      || callbackRef <= LUA_REFNIL
      || m_asyncHttpCallbackRefs.size() >= kMaxAsyncHttpPerHost) {
    return false;
  }
  auto handler = m_asyncHttpResultHandler;
  if (!handler) {
    return false;
  }
  m_asyncHttpCallbackRefs.insert(callbackRef);

  DeferredCall::callLater([client = m_httpClient, url = std::move(url), destPath = std::move(destPath),
                           handler = std::move(handler), hostId = m_hostId, callbackRef]() mutable {
    client->download(url, std::filesystem::path(destPath), [handler, hostId, callbackRef](bool success) {
      handler(hostId, callbackRef, success, 0, std::string(), true);
    });
  });
  return true;
}

bool LuauHost::callAsyncHttpCallback(
    int callbackRef, bool ok, int status, const std::string& body, std::chrono::milliseconds budget
) {
  if (m_T == nullptr) {
    return false;
  }
  const auto it = m_asyncHttpCallbackRefs.find(callbackRef);
  if (it == m_asyncHttpCallbackRefs.end()) {
    return false;
  }
  m_asyncHttpCallbackRefs.erase(it);

  lua_getref(m_T, callbackRef);
  lua_unref(m_T, callbackRef);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }

  lua_createtable(m_T, 0, 3);
  setTableBool(m_T, "ok", ok);
  setTableInteger(m_T, "status", status);
  setTableString(m_T, "body", body);

  return callWithBudget("async http callback", 1, 0, budget);
}

bool LuauHost::callAsyncDownloadCallback(int callbackRef, bool ok, std::chrono::milliseconds budget) {
  if (m_T == nullptr) {
    return false;
  }
  const auto it = m_asyncHttpCallbackRefs.find(callbackRef);
  if (it == m_asyncHttpCallbackRefs.end()) {
    return false;
  }
  m_asyncHttpCallbackRefs.erase(it);

  lua_getref(m_T, callbackRef);
  lua_unref(m_T, callbackRef);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }

  lua_pushboolean(m_T, ok ? 1 : 0);
  return callWithBudget("async download callback", 1, 0, budget);
}

void LuauHost::stateSet(const std::string& key, std::string json) {
  scripting::PluginStateStore::instance().set(m_pluginId, key, std::move(json));
}

std::optional<std::string> LuauHost::stateGet(const std::string& key) const {
  return scripting::PluginStateStore::instance().get(m_pluginId, key);
}

void LuauHost::stateWatch(std::string key, int callbackRef) {
  if (callbackRef <= LUA_REFNIL) {
    return;
  }
  m_stateWatchCallbackRefs.insert(callbackRef);
  if (m_stateWatchHandler) {
    m_stateWatchHandler(std::move(key), callbackRef);
  }
}

bool LuauHost::hasStateWatchCallback(int callbackRef) const { return m_stateWatchCallbackRefs.contains(callbackRef); }

bool LuauHost::startStream(std::string command, int callbackRef) {
  if (command.empty() || callbackRef <= LUA_REFNIL || m_streamCancels.size() >= kMaxStreamsPerHost) {
    return false;
  }
  auto handler = m_streamLineHandler;
  if (!handler) {
    return false;
  }

  m_streamCallbackRefs.insert(callbackRef);
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  m_streamCancels.push_back(cancel);

  const std::uint64_t hostId = m_hostId;
  auto buffer = std::make_shared<std::string>();

  process::RunCallbacks callbacks;
  // Runs on the process worker thread: split chunks into lines and marshal each
  // back to the runtime thread (the handler enqueues into the runtime mailbox).
  callbacks.stdOut = [hostId, callbackRef, handler, buffer](std::string_view chunk) {
    buffer->append(chunk);
    std::size_t pos = 0;
    while ((pos = buffer->find('\n')) != std::string::npos) {
      std::string line = buffer->substr(0, pos);
      buffer->erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      handler(hostId, callbackRef, std::move(line));
    }
    if (buffer->size() > kMaxStreamLineBytes) {
      buffer->clear(); // drop a pathological unbounded line
    }
  };

  process::RunOptions options;
  options.cancel = std::move(cancel);
  // No timeout (long-lived); no onExit so output is never accumulated, only streamed.
  return process::runAsync({"/bin/sh", "-c", std::move(command)}, std::move(callbacks), std::move(options));
}

bool LuauHost::callStreamCallback(int callbackRef, const std::string& line, std::chrono::milliseconds budget) {
  if (m_T == nullptr || !m_streamCallbackRefs.contains(callbackRef)) {
    return false;
  }
  // Stream callbacks fire repeatedly; the ref lives with the lua_State and is
  // cleaned up wholesale on reload (new host).
  lua_getref(m_T, callbackRef);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }
  lua_pushlstring(m_T, line.data(), line.size());
  return callWithBudget("stream callback", 1, 0, budget);
}

bool LuauHost::hasStreamCallback(int callbackRef) const { return m_streamCallbackRefs.contains(callbackRef); }

void LuauHost::stopAllStreams() noexcept {
  for (const auto& cancel : m_streamCancels) {
    if (cancel) {
      cancel->store(true, std::memory_order_relaxed);
    }
  }
  m_streamCancels.clear();
}

bool LuauHost::callStateWatchCallback(int callbackRef, const std::string& json, std::chrono::milliseconds budget) {
  if (m_T == nullptr || !m_stateWatchCallbackRefs.contains(callbackRef)) {
    return false;
  }
  // Watch callbacks fire repeatedly, so the ref is NOT released here — it lives
  // with the lua_State and is cleaned up wholesale on reload (new host).
  lua_getref(m_T, callbackRef);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }
  try {
    jsonToLua(m_T, nlohmann::json::parse(json));
  } catch (const nlohmann::json::exception&) {
    lua_pop(m_T, 1);
    return false;
  }
  return callWithBudget("state watch callback", 1, 0, budget);
}

bool LuauHost::callAsyncCommandCallback(
    int callbackRef, const process::RunResult& result, std::chrono::milliseconds budget
) {
  if (m_T == nullptr) {
    return false;
  }
  const auto it = m_asyncCommandCallbackRefs.find(callbackRef);
  if (it == m_asyncCommandCallbackRefs.end()) {
    return false;
  }
  m_asyncCommandCallbackRefs.erase(it);

  lua_getref(m_T, callbackRef);
  lua_unref(m_T, callbackRef);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }

  lua_createtable(m_T, 0, 6);
  setTableInteger(m_T, "exitCode", result.exitCode);
  setTableString(m_T, "stdout", result.out);
  setTableString(m_T, "stderr", result.err);
  setTableBool(m_T, "timedOut", result.timedOut);
  setTableBool(m_T, "stdoutTruncated", result.outTruncated);
  setTableBool(m_T, "stderrTruncated", result.errTruncated);

  return callWithBudget("async command callback", 1, 0, budget);
}

bool LuauHost::callAsyncProcessMatchCallback(int callbackRef, bool matched, std::chrono::milliseconds budget) {
  if (m_T == nullptr) {
    return false;
  }
  const auto it = m_asyncProcessMatchCallbackRefs.find(callbackRef);
  if (it == m_asyncProcessMatchCallbackRefs.end()) {
    return false;
  }
  m_asyncProcessMatchCallbackRefs.erase(it);

  lua_getref(m_T, callbackRef);
  lua_unref(m_T, callbackRef);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }

  lua_pushboolean(m_T, matched ? 1 : 0);
  return callWithBudget("process match callback", 1, 0, budget);
}

void LuauHost::interruptIfBudgetExceeded(lua_State* L) {
  if (!m_budgetActive) {
    return;
  }
  if (std::chrono::steady_clock::now() <= m_callDeadline) {
    return;
  }
  m_lastCallTimedOut = true;
  m_budgetActive = false;
  luaL_error(L, "script callback '%s' timed out", m_currentCallName.empty() ? "(unknown)" : m_currentCallName.c_str());
}

void LuauHost::loadTranslations() { m_translations.load(m_pluginDir); }

std::string LuauHost::translate(std::string_view key, const std::unordered_map<std::string, std::string>& subst) const {
  return m_translations.translate(key, subst);
}

void LuauHost::scriptSetUpdateInterval(int ms) {
  if (m_scriptContext != nullptr) {
    m_scriptContext->patch.updateIntervalMs = std::max(16, ms);
  }
}

void LuauHost::scriptLog(std::string message) {
  if (m_scriptContext != nullptr) {
    m_scriptContext->sideEffects.push_back(
        {.kind = scripting::ScriptSideEffectKind::Log, .title = std::move(message), .body = {}}
    );
    return;
  }
  kLog.info("{}", message);
}

void LuauHost::scriptNotifyInfo(std::string title, std::string body) {
  if (m_scriptContext != nullptr) {
    m_scriptContext->sideEffects.push_back(
        {.kind = scripting::ScriptSideEffectKind::NotifyInfo, .title = std::move(title), .body = std::move(body)}
    );
    return;
  }
  notify::info("Noctalia", title, body);
}

void LuauHost::scriptNotifyError(std::string title, std::string body) {
  if (m_scriptContext != nullptr) {
    m_scriptContext->sideEffects.push_back(
        {.kind = scripting::ScriptSideEffectKind::NotifyError, .title = std::move(title), .body = std::move(body)}
    );
    return;
  }
  notify::error("Noctalia", title, body);
}

void LuauHost::scriptSetWallpaperEnabled(std::string connector, bool enabled) {
  if (m_scriptContext != nullptr) {
    m_scriptContext->sideEffects.push_back(
        {.kind = scripting::ScriptSideEffectKind::SetWallpaperEnabled,
         .title = std::move(connector),
         .body = {},
         .flag = enabled}
    );
  }
}

void LuauHost::scriptSetWallpaper(std::string connector, std::string path) {
  if (m_scriptContext != nullptr) {
    m_scriptContext->sideEffects.push_back(
        {.kind = scripting::ScriptSideEffectKind::SetWallpaper, .title = std::move(connector), .body = std::move(path)}
    );
  }
}

void LuauHost::scriptTogglePanel(std::string panelId) {
  if (m_scriptContext != nullptr) {
    m_scriptContext->sideEffects.push_back(
        {.kind = scripting::ScriptSideEffectKind::TogglePanel, .title = std::move(panelId), .body = {}}
    );
  }
}

bool LuauHost::scriptCopyToClipboard(std::string text, std::string mimeType) {
  if (m_scriptContext == nullptr || text.empty() || mimeType.empty()) {
    return false;
  }
  m_scriptContext->sideEffects.push_back(
      {.kind = scripting::ScriptSideEffectKind::CopyToClipboard, .title = std::move(text), .body = std::move(mimeType)}
  );
  return true;
}

std::optional<std::string> LuauHost::scriptFocusedOutputName() const {
  if (m_scriptContext == nullptr || m_scriptContext->snapshot.focusedOutputName.empty()) {
    return std::nullopt;
  }
  return m_scriptContext->snapshot.focusedOutputName;
}

void LuauHost::beginBudget(std::string_view name, std::chrono::milliseconds budget) {
  m_currentCallName = std::string(name);
  m_callDeadline = std::chrono::steady_clock::now() + std::max(budget, std::chrono::milliseconds(1));
  m_lastCallTimedOut = false;
  m_budgetActive = true;
}

void LuauHost::endBudget() { m_budgetActive = false; }

bool LuauHost::callWithBudget(const char* name, int args, int results, std::chrono::milliseconds budget) {
  beginBudget(name != nullptr ? name : "(unknown)", budget);
  int rc = lua_pcall(m_T, args, results, 0);
  endBudget();
  if (rc != 0) {
    const char* err = lua_tostring(m_T, -1);
    if (!m_muteErrors) {
      kLog.error("call to '{}' failed: {}", name ? name : "(unknown)", err ? err : "(no error)");
    }
    lua_pop(m_T, 1);
    return false;
  }
  return true;
}

bool LuauHost::callGlobalInternal(const char* name, int args, std::chrono::milliseconds budget) {
  return callWithBudget(name, args, 0, budget);
}

bool LuauHost::loadString(std::string_view chunkName, std::string_view source) {
  size_t bytecodeSize = 0;
  char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
  if (!bytecode) {
    kLog.error("luau_compile returned null for chunk '{}'", std::string(chunkName));
    return false;
  }
  std::string name(chunkName);
  int loadResult = luau_load(m_T, name.c_str(), bytecode, bytecodeSize, 0);
  std::free(bytecode);
  if (loadResult != 0) {
    const char* err = lua_tostring(m_T, -1);
    kLog.error("luau_load failed for '{}': {}", name, err ? err : "(no error)");
    lua_pop(m_T, 1);
    return false;
  }
  return true;
}

bool LuauHost::run() { return callWithBudget("chunk", 0, 0, std::chrono::milliseconds(100)); }

bool LuauHost::hasGlobal(const char* name) {
  lua_getglobal(m_T, name);
  bool exists = lua_isfunction(m_T, -1);
  lua_pop(m_T, 1);
  return exists;
}

bool LuauHost::callGlobal(const char* name) { return callGlobalWithBudget(name, std::chrono::milliseconds(25)); }

bool LuauHost::callGlobalWithBudget(const char* name, std::chrono::milliseconds budget) {
  lua_getglobal(m_T, name);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }
  return callGlobalInternal(name, 0, budget);
}

bool LuauHost::callGlobalWithBool(const char* name, bool value) {
  return callGlobalWithBoolAndBudget(name, value, std::chrono::milliseconds(25));
}

bool LuauHost::callGlobalWithBoolAndBudget(const char* name, bool value, std::chrono::milliseconds budget) {
  lua_getglobal(m_T, name);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }
  lua_pushboolean(m_T, value ? 1 : 0);
  return callGlobalInternal(name, 1, budget);
}

bool LuauHost::callGlobalWithStrings(const char* name, std::string_view first, std::string_view second) {
  return callGlobalWithStringsAndBudget(name, first, second, std::chrono::milliseconds(25));
}

bool LuauHost::callGlobalWithStringsAndBudget(
    const char* name, std::string_view first, std::string_view second, std::chrono::milliseconds budget
) {
  lua_getglobal(m_T, name);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return false;
  }
  lua_pushlstring(m_T, first.data(), first.size());
  lua_pushlstring(m_T, second.data(), second.size());
  return callGlobalInternal(name, 2, budget);
}

std::optional<std::string> LuauHost::callGlobalReturningString(const char* name) {
  lua_getglobal(m_T, name);
  if (!lua_isfunction(m_T, -1)) {
    lua_pop(m_T, 1);
    return std::nullopt;
  }
  int rc = lua_pcall(m_T, 0, 1, 0);
  if (rc != 0) {
    const char* err = lua_tostring(m_T, -1);
    kLog.error("call to '{}' failed: {}", name, err ? err : "(no error)");
    lua_pop(m_T, 1);
    return std::nullopt;
  }
  std::optional<std::string> result;
  if (lua_isstring(m_T, -1)) {
    size_t len = 0;
    const char* s = lua_tolstring(m_T, -1, &len);
    result = std::string(s, len);
  }
  lua_pop(m_T, 1);
  return result;
}
