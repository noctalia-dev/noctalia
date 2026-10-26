#include "core/toml.h"
#include "scripting/luau_host.h"
#include "scripting/script_api_context.h"

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

  namespace fs = std::filesystem;

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_file_privacy_test: {}", message);
    }
    return condition;
  }

  unsigned mode(const fs::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
      return 0;
    }
    return st.st_mode & 07777;
  }

} // namespace

int main() {
  ::umask(022);
  char templ[] = "/tmp/noctalia_plugin_file_privacy_XXXXXX";
  const char* made = ::mkdtemp(templ);
  if (!expect(made != nullptr, "mkdtemp")) {
    return 1;
  }
  const fs::path root(made);
  ::setenv("NOCTALIA_STATE_HOME", root.c_str(), 1);

  // A data dir left world-readable by an older release must be tightened.
  const fs::path dataDir = root / "noctalia" / "plugins" / "data" / "test" / "plugin";
  fs::create_directories(dataDir);
  fs::permissions(dataDir, fs::perms(0755), fs::perm_options::replace);
  const fs::path shared = root / "shared.txt";
  {
    const int fd = ::open(shared.c_str(), O_WRONLY | O_CREAT, 0644);
    ::close(fd);
  }
  ::chmod(shared.c_str(), 0644);

  scripting::ScriptApiContext api;
  api.setConfigSnapshot(std::make_shared<const toml::table>(toml::parse("[shell]\noffline_mode = true")));
  LuauHost host(api, "test/plugin:service");

  const std::string source = R"(
local dir = assert(noctalia.pluginDataDir())
assert(noctalia.writeFile(dir .. "/history.json", "private"))
assert(noctalia.writeFile(")"
      + shared.string()
      + R"(", "shared"))
)";
  bool ok = expect(host.exec("=privacy", source.c_str()), "script should run");
  ok = expect(mode(dataDir) == 0700, "pluginDataDir should be owner-only") && ok;
  ok = expect(mode(dataDir / "history.json") == 0600, "new file should be created 0600") && ok;
  ok = expect(mode(shared) == 0644, "existing file should keep its mode") && ok;

  fs::remove_all(root);
  return ok ? 0 : 1;
}
