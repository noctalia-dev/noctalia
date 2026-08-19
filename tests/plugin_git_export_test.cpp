#include "core/process/process.h"
#include "scripting/plugin_api.h"
#include "scripting/plugin_catalog.h"
#include "scripting/plugin_git.h"

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

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_git_export_test: {}", message);
    }
    return condition;
  }

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

} // namespace

int main() {
  std::vector<scripting::CatalogEntry> catalog(1);
  catalog.front().id = "alexander/game-launcher";

  bool ok = true;
  ok = expect(
           scripting::findCatalogEntry(catalog, "alexander/game-launcher") != nullptr, "exact catalog id was not found"
       )
      && ok;
  ok = expect(
           scripting::findCatalogEntry(catalog, "leo/game-launcher") == nullptr,
           "catalog lookup matched a different author with the same slug"
       )
      && ok;

  const auto root = makeTempDir();
  if (!expect(!root.empty(), "failed to create temp dir")) {
    return 1;
  }

  const auto source = root / "source";
  const auto repo = root / "repo";
  const auto exported = root / "exported";

  std::filesystem::create_directories(source);
  ok = runGit({"git", "-C", source.string(), "init", "-q"}) && ok;
  ok = writeText(source / "clock/plugin.toml", "id = \"noctalia/clock\"\nversion = \"1.0.0\"\nplugin_api = 3\n") && ok;
  ok = writeText(source / "clock/main.luau", "barWidget.setText(\"ok\")\n") && ok;
  ok = runGit({"git", "-C", source.string(), "add", "clock/plugin.toml", "clock/main.luau"}) && ok;
  ok = runGit(
           {"git", "-C", source.string(), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit",
            "-q", "-m", "init"}
       )
      && ok;

  const auto cloned = scripting::plugin_git::cloneBlobless(source.string(), repo);
  ok = expect(static_cast<bool>(cloned), "cloneBlobless failed") && ok;

  const auto exportResult = scripting::plugin_git::exportSubdir(repo, "HEAD", "clock", exported);
  ok = expect(static_cast<bool>(exportResult), "exportSubdir failed") && ok;
  ok = expect(std::filesystem::exists(exported / "clock/plugin.toml"), "exported manifest missing") && ok;
  ok = expect(!std::filesystem::exists(repo / "clock/plugin.toml"), "repo cache was checked out") && ok;

  const auto initialHead = scripting::plugin_git::headRevision(repo);
  ok = expect(static_cast<bool>(initialHead), "failed to resolve initial HEAD") && ok;

  ok = writeText(source / "cat/plugin.toml", "id = \"dotnetrob/cat\"\nversion = \"1.0.0\"\nplugin_api = 3\n") && ok;
  ok = writeText(source / "cat/main.luau", "barWidget.setText(\"cat\")\n") && ok;
  ok = runGit({"git", "-C", source.string(), "add", "cat/plugin.toml", "cat/main.luau"}) && ok;
  ok = runGit(
           {"git", "-C", source.string(), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit",
            "-q", "-m", "add cat"}
       )
      && ok;

  const auto fetchResult = scripting::plugin_git::fetch(repo, source.string());
  ok = expect(static_cast<bool>(fetchResult), "fetch failed") && ok;
  const auto fetchedHead = scripting::plugin_git::remoteHead(repo);
  ok = expect(static_cast<bool>(fetchedHead), "failed to resolve FETCH_HEAD") && ok;
  ok = expect(fetchedHead.out != initialHead.out, "fetch did not advance the remote revision") && ok;

  const auto staleExport = scripting::plugin_git::exportSubdir(repo, "HEAD", "cat", root / "stale-export");
  ok = expect(!staleExport, "stale HEAD unexpectedly exported a newly fetched plugin") && ok;

  const auto fetchedExport = scripting::plugin_git::exportSubdir(repo, fetchedHead.out, "cat", root / "fetched-export");
  ok = expect(static_cast<bool>(fetchedExport), "exact fetched revision did not export the new plugin") && ok;
  ok = expect(
           std::filesystem::exists(root / "fetched-export/cat/plugin.toml"),
           "new plugin manifest was not exported from the fetched revision"
       )
      && ok;

  // A plugin whose tip moves past the supported API range must still be exportable at the
  // older revision a catalog release row names, straight out of the blobless clone.
  ok =
      writeText(
          source / "clock/plugin.toml",
          std::format(
              "id = \"noctalia/clock\"\nversion = \"2.0.0\"\nplugin_api = {}\n", scripting::kCurrentPluginApiVersion + 1
          )
      )
      && ok;
  ok = runGit({"git", "-C", source.string(), "add", "clock/plugin.toml"}) && ok;
  ok = runGit(
           {"git", "-C", source.string(), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit",
            "-q", "-m", "clock requires a newer api"}
       )
      && ok;
  ok = expect(static_cast<bool>(scripting::plugin_git::fetch(repo, source.string())), "fetch after the api bump failed")
      && ok;
  const auto bumpedHead = scripting::plugin_git::remoteHead(repo);
  ok = expect(static_cast<bool>(bumpedHead), "failed to resolve the bumped revision") && ok;

  const auto tipExport = scripting::plugin_git::exportSubdir(repo, bumpedHead.out, "clock", root / "tip-export");
  ok = expect(static_cast<bool>(tipExport), "exporting the bumped tip failed") && ok;
  ok = expect(
           readText(root / "tip-export/clock/plugin.toml")
               .contains(std::format("plugin_api = {}", scripting::kCurrentPluginApiVersion + 1)),
           "the tip export did not carry the bumped api level"
       )
      && ok;

  const auto olderExport = scripting::plugin_git::exportSubdir(repo, initialHead.out, "clock", root / "older-export");
  ok = expect(static_cast<bool>(olderExport), "exporting an older revision failed") && ok;
  const auto olderManifest = readText(root / "older-export/clock/plugin.toml");
  ok = expect(olderManifest.contains("plugin_api = 3"), "the older export did not carry its own api level") && ok;
  ok = expect(olderManifest.contains("version = \"1.0.0\""), "the older export did not carry its own version") && ok;

  // A checkout retained after its configured source location changes must fetch
  // the new canonical location, never the stale origin recorded by the clone.
  const auto replacementSource = root / "replacement-source";
  std::filesystem::create_directories(replacementSource);
  ok = runGit({"git", "-C", replacementSource.string(), "init", "-q"}) && ok;
  ok = writeText(replacementSource / "replacement.txt", "replacement\n") && ok;
  ok = runGit({"git", "-C", replacementSource.string(), "add", "replacement.txt"}) && ok;
  ok = runGit(
           {"git", "-C", replacementSource.string(), "-c", "user.name=test", "-c", "user.email=test@example.invalid",
            "commit", "-q", "-m", "replacement source"}
       )
      && ok;
  const auto replacementHead = scripting::plugin_git::headRevision(replacementSource);
  ok = expect(static_cast<bool>(replacementHead), "failed to resolve replacement source HEAD") && ok;
  ok = expect(
           static_cast<bool>(scripting::plugin_git::fetch(repo, replacementSource.string())),
           "fetch did not accept the replacement source location"
       )
      && ok;
  const auto reboundHead = scripting::plugin_git::remoteHead(repo);
  ok = expect(static_cast<bool>(reboundHead), "failed to resolve rebound FETCH_HEAD") && ok;
  ok = expect(
           reboundHead.out == replacementHead.out, "fetch used the stale clone origin instead of the configured source"
       )
      && ok;

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return ok ? 0 : 1;
}
