#include "config/toml.h"
#include "scripting/luau_host.h"
#include "scripting/script_api_context.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

  namespace fs = std::filesystem;
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
  TEST_CHECK(made != nullptr);
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
  const bool ran = host.exec("=privacy", source.c_str());
  const unsigned dataDirMode = mode(dataDir);
  const unsigned historyMode = mode(dataDir / "history.json");
  const unsigned sharedMode = mode(shared);

  fs::remove_all(root);

  TEST_CHECK(ran);
  TEST_CHECK(dataDirMode == 0700);
  // New file should be created 0600.
  TEST_CHECK(historyMode == 0600);
  // Existing file should keep its mode.
  TEST_CHECK(sharedMode == 0644);
  return 0;
}
