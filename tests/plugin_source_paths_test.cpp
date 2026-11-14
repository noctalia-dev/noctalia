#include "config/config_types.h"
#include "scripting/plugin_source_paths.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <string>

int main() {
  ::setenv("NOCTALIA_STATE_HOME", "/tmp/noctalia-path-test-state", 1);
  ::setenv("NOCTALIA_DATA_HOME", "/tmp/noctalia-path-test-data", 1);

  const PluginSourceConfig gitSource{
      .kind = PluginSourceKind::Git,
      .name = "official",
      .location = "https://example.invalid/plugins.git",
  };
  const PluginSourceConfig pathSource{
      .kind = PluginSourceKind::Path,
      .name = "dev",
      .location = "~/dev/noctalia-plugins",
  };

  const std::filesystem::path stateRoot = "/tmp/noctalia-path-test-state/noctalia";
  TEST_CHECK(
      scripting::plugin_paths::localSourceRoot()
      == std::filesystem::path("/tmp/noctalia-path-test-data/noctalia/plugins")
  );
  TEST_CHECK(scripting::plugin_paths::sourceStorageRoot(gitSource) == stateRoot / "plugins/sources/official");
  TEST_CHECK(scripting::plugin_paths::gitRepoRoot(gitSource) == stateRoot / "plugins/sources/official/repo");
  TEST_CHECK(scripting::plugin_paths::gitMaterializedRoot(gitSource) == stateRoot / "plugins/materialized/official");
  TEST_CHECK(scripting::plugin_paths::registryRoot(gitSource) == stateRoot / "plugins/materialized/official");
  TEST_CHECK(isDefaultPluginSourceName("official"));
  TEST_CHECK(isDefaultPluginSourceName("community"));
  TEST_CHECK(!isDefaultPluginSourceName("dev"));
  // Path source registry root expands the user path
  TEST_CHECK(scripting::plugin_paths::registryRoot(pathSource).string().ends_with("/dev/noctalia-plugins"));
  TEST_CHECK(scripting::plugin_paths::pathIsInside("/tmp/noctalia/a/b", "/tmp/noctalia"));
  // Parent path must not count as inside itself
  TEST_CHECK(!scripting::plugin_paths::pathIsInside("/tmp/noctalia", "/tmp/noctalia"));
  // Sibling prefix must not count as inside parent
  TEST_CHECK(!scripting::plugin_paths::pathIsInside("/tmp/noctalia-other/a", "/tmp/noctalia"));

  return 0;
}
