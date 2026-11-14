#include "scripting/plugin_api.h"
#include "scripting/plugin_catalog.h"
#include "scripting/plugin_git.h"
#include "system/process.h"
#include "tests/test_check.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

  std::filesystem::path makeTempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-plugin-git-export-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* result = ::mkdtemp(buffer.data());
    return result != nullptr ? std::filesystem::path(result) : std::filesystem::path{};
  }

  bool writeText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out << text;
    return out.good();
  }

  std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  bool runGit(const std::vector<std::string>& args) {
    auto result = process::runSync(args);
    if (!result) {
      std::print(stderr, "plugin_git_export_test: command failed:");
      for (const auto& arg : args) {
        std::print(stderr, " {}", arg);
      }
      std::println(stderr, "\n{}", result.err);
    }
    return result;
  }

  std::string gitConfigValue(const std::filesystem::path& repo, std::string_view key) {
    auto result = process::runSync({"git", "-C", repo.string(), "config", "--get", std::string(key)});
    std::string value = result.out;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
      value.pop_back();
    }
    return value;
  }

  std::filesystem::path tempRoot;

  void removeTempRoot() {
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
  }

} // namespace

int main() {
  std::vector<scripting::CatalogEntry> catalog(1);
  catalog.front().id = "alexander/game-launcher";

  TEST_CHECK(scripting::findCatalogEntry(catalog, "alexander/game-launcher") != nullptr);
  TEST_CHECK(scripting::findCatalogEntry(catalog, "leo/game-launcher") == nullptr);

  const auto root = makeTempDir();
  TEST_CHECK(!root.empty());
  tempRoot = root;
  std::atexit(removeTempRoot);

  // Every git command in this test runs against a user config that renames the default
  // clone remote, the configuration that used to leave source caches without `origin`.
  const auto gitConfigFile = root / "gitconfig";
  TEST_CHECK(writeText(gitConfigFile, "[clone]\n\tdefaultRemoteName = up\n"));
  ::setenv("GIT_CONFIG_GLOBAL", gitConfigFile.c_str(), 1);
  ::setenv("GIT_CONFIG_SYSTEM", "/dev/null", 1);

  const auto source = root / "source";
  const auto repo = root / "repo";
  const auto exported = root / "exported";

  std::filesystem::create_directories(source);
  TEST_CHECK(runGit({"git", "-C", source.string(), "init", "-q"}));
  TEST_CHECK(writeText(source / "clock/plugin.toml", "id = \"noctalia/clock\"\nversion = \"1.0.0\"\nplugin_api = 3\n"));
  TEST_CHECK(writeText(source / "clock/main.luau", "barWidget.setText(\"ok\")\n"));
  TEST_CHECK(runGit({"git", "-C", source.string(), "add", "clock/plugin.toml", "clock/main.luau"}));
  TEST_CHECK(runGit(
      {"git", "-C", source.string(), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-q",
       "-m", "init"}
  ));

  const auto cloned = scripting::plugin_git::cloneBlobless(source.string(), repo);
  TEST_CHECK(cloned);
  TEST_CHECK(gitConfigValue(repo, "remote.origin.url") == source.string());

  const auto exportResult = scripting::plugin_git::exportSubdir(repo, "HEAD", "clock", exported);
  TEST_CHECK(exportResult);
  TEST_CHECK(std::filesystem::exists(exported / "clock/plugin.toml"));
  TEST_CHECK(!std::filesystem::exists(repo / "clock/plugin.toml"));

  const auto initialHead = scripting::plugin_git::headRevision(repo);
  TEST_CHECK(initialHead);

  TEST_CHECK(writeText(source / "cat/plugin.toml", "id = \"dotnetrob/cat\"\nversion = \"1.0.0\"\nplugin_api = 3\n"));
  TEST_CHECK(writeText(source / "cat/main.luau", "barWidget.setText(\"cat\")\n"));
  TEST_CHECK(runGit({"git", "-C", source.string(), "add", "cat/plugin.toml", "cat/main.luau"}));
  TEST_CHECK(runGit(
      {"git", "-C", source.string(), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-q",
       "-m", "add cat"}
  ));

  TEST_CHECK(scripting::plugin_git::ensureRepo(repo, source.string()));
  const auto fetchResult = scripting::plugin_git::fetch(repo);
  TEST_CHECK(fetchResult);
  const auto fetchedHead = scripting::plugin_git::remoteHead(repo);
  TEST_CHECK(fetchedHead);
  TEST_CHECK(fetchedHead.out != initialHead.out);

  const auto staleExport = scripting::plugin_git::exportSubdir(repo, "HEAD", "cat", root / "stale-export");
  TEST_CHECK(!staleExport);

  const auto fetchedExport = scripting::plugin_git::exportSubdir(repo, fetchedHead.out, "cat", root / "fetched-export");
  TEST_CHECK(fetchedExport);
  TEST_CHECK(std::filesystem::exists(root / "fetched-export/cat/plugin.toml"));

  // A plugin whose tip moves past the supported API range must still be exportable at the
  // older revision a catalog release row names, straight out of the blobless clone.
  TEST_CHECK(writeText(
      source / "clock/plugin.toml",
      std::format(
          "id = \"noctalia/clock\"\nversion = \"2.0.0\"\nplugin_api = {}\n", scripting::kCurrentPluginApiVersion + 1
      )
  ));
  TEST_CHECK(runGit({"git", "-C", source.string(), "add", "clock/plugin.toml"}));
  TEST_CHECK(runGit(
      {"git", "-C", source.string(), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "-q",
       "-m", "clock requires a newer api"}
  ));
  TEST_CHECK(scripting::plugin_git::fetch(repo));
  const auto bumpedHead = scripting::plugin_git::remoteHead(repo);
  TEST_CHECK(bumpedHead);

  const auto tipExport = scripting::plugin_git::exportSubdir(repo, bumpedHead.out, "clock", root / "tip-export");
  TEST_CHECK(tipExport);
  TEST_CHECK(readText(root / "tip-export/clock/plugin.toml")
                 .contains(std::format("plugin_api = {}", scripting::kCurrentPluginApiVersion + 1)));

  const auto olderExport = scripting::plugin_git::exportSubdir(repo, initialHead.out, "clock", root / "older-export");
  TEST_CHECK(olderExport);
  const auto olderManifest = readText(root / "older-export/clock/plugin.toml");
  TEST_CHECK(olderManifest.contains("plugin_api = 3"));
  TEST_CHECK(olderManifest.contains("version = \"1.0.0\""));

  // A checkout retained after its configured source location changes must fetch
  // the new canonical location, never the stale origin recorded by the clone.
  const auto replacementSource = root / "replacement-source";
  std::filesystem::create_directories(replacementSource);
  TEST_CHECK(runGit({"git", "-C", replacementSource.string(), "init", "-q"}));
  TEST_CHECK(writeText(replacementSource / "replacement.txt", "replacement\n"));
  TEST_CHECK(runGit({"git", "-C", replacementSource.string(), "add", "replacement.txt"}));
  TEST_CHECK(runGit(
      {"git", "-C", replacementSource.string(), "-c", "user.name=test", "-c", "user.email=test@example.invalid",
       "commit", "-q", "-m", "replacement source"}
  ));
  const auto replacementHead = scripting::plugin_git::headRevision(replacementSource);
  TEST_CHECK(replacementHead);
  TEST_CHECK(scripting::plugin_git::ensureRepo(repo, replacementSource.string()));
  TEST_CHECK(scripting::plugin_git::fetch(repo));
  const auto reboundHead = scripting::plugin_git::remoteHead(repo);
  TEST_CHECK(reboundHead);
  TEST_CHECK(reboundHead.out == replacementHead.out);

  // A cache cloned without the pinned remote name (git config renaming it, or an
  // interrupted clone) has no `origin`, so nothing in it can fetch. Preparing it must
  // rebuild the cache instead of failing every later git operation.
  const auto strayRemoteRepo = root / "stray-remote-repo";
  TEST_CHECK(runGit({"git", "clone", "-q", "--no-checkout", source.string(), strayRemoteRepo.string()}));
  TEST_CHECK(gitConfigValue(strayRemoteRepo, "remote.origin.url").empty());
  TEST_CHECK(scripting::plugin_git::ensureRepo(strayRemoteRepo, source.string()));
  TEST_CHECK(gitConfigValue(strayRemoteRepo, "remote.origin.url") == source.string());
  TEST_CHECK(gitConfigValue(strayRemoteRepo, "remote.up.url").empty());
  TEST_CHECK(scripting::plugin_git::fetch(strayRemoteRepo));

  return 0;
}
