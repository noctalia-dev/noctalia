#include "config/atomic_file.h"
#include "tests/test_check.h"
#include "theme/template_engine.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using noctalia::theme::TemplateEngine;

  constexpr std::string_view kPrevious = "previous output\n";
  constexpr std::string_view kRendered = "surface = #123456\n";

  void writeFile(const fs::path& path, std::string_view text) {
    std::ofstream out(path);
    out << text;
    out.close();
    TEST_CHECK(out.good());
  }

  std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  struct stat fileStatus(const fs::path& path) {
    struct stat status{};
    TEST_CHECK(::stat(path.c_str(), &status) == 0);
    return status;
  }

  void atomicReplacement(const fs::path& root, TemplateEngine& engine) {
    const auto output = root / "atomic.conf";
    writeFile(output, kPrevious);
    fs::permissions(output, fs::perms(0640));
    const auto before = fileStatus(output);
    std::ifstream oldReader(output);

    const auto result = engine.renderFile(root / "input.tmpl", output);
    TEST_CHECK(result.success && result.wrote);
    TEST_CHECK(readFile(output) == kRendered);
    TEST_CHECK(fileStatus(output).st_ino != before.st_ino);
    TEST_CHECK((fileStatus(output).st_mode & 0777) == 0640);
    const std::string oldText{std::istreambuf_iterator<char>(oldReader), std::istreambuf_iterator<char>()};
    TEST_CHECK(oldText == kPrevious);
    TEST_CHECK(!fs::exists(output.string() + ".tmp"));

    const auto unchanged = engine.renderFile(root / "input.tmpl", output);
    TEST_CHECK(unchanged.success && !unchanged.wrote);

    const auto link = root / "link.conf";
    fs::create_symlink(output.filename(), link);
    writeFile(output, kPrevious);
    const auto linked = engine.renderFile(root / "input.tmpl", link);
    TEST_CHECK(linked.success && linked.wrote);
    TEST_CHECK(fs::is_symlink(link));
    TEST_CHECK(fs::read_symlink(link) == output.filename());
    TEST_CHECK(readFile(output) == kRendered);
  }

  void protectedDirectory(const fs::path& root, TemplateEngine& engine) {
    if (::geteuid() == 0) {
      return; // Root bypasses directory and file write permissions.
    }
    const auto dir = root / "protected";
    fs::create_directory(dir);
    const auto output = dir / "theme.conf";
    const auto link = root / "protected-link.conf";
    const auto fifo = dir / "fifo";
    writeFile(output, kPrevious);
    fs::permissions(output, fs::perms(0640));
    const auto before = fileStatus(output);
    fs::create_symlink(output, link);
    TEST_CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    fs::permissions(dir, fs::perms(0555));

    // Other callers must still require atomic replacement by default.
    TEST_CHECK(!writeTextFileAtomic(output, "strict atomic write"));
    TEST_CHECK(readFile(output) == kPrevious);
    const auto result = engine.renderFile(root / "input.tmpl", output);
    TEST_CHECK(result.success && result.wrote);
    TEST_CHECK(readFile(output) == kRendered);
    const auto after = fileStatus(output);
    TEST_CHECK(after.st_ino == before.st_ino);
    TEST_CHECK(after.st_uid == before.st_uid && after.st_gid == before.st_gid);
    TEST_CHECK((after.st_mode & 0777) == 0640);
    TEST_CHECK(!fs::exists(output.string() + ".tmp"));

    writeFile(output, kPrevious);
    const auto linked = engine.renderFile(root / "input.tmpl", link);
    TEST_CHECK(linked.success && linked.wrote);
    TEST_CHECK(fs::is_symlink(link));
    TEST_CHECK(readFile(output) == kRendered);

    const auto missing = engine.renderFile(root / "input.tmpl", dir / "missing.conf");
    TEST_CHECK(!missing.success && !missing.wrote);
    TEST_CHECK(!fs::exists(dir / "missing.conf"));
    TEST_CHECK(!writeTextFileAtomic(fifo, "not a regular file", std::nullopt, AtomicWriteFallback::ExistingFile));

    writeFile(output, kPrevious);
    fs::permissions(output, fs::perms(0440));
    const auto denied = engine.renderFile(root / "input.tmpl", output);
    TEST_CHECK(!denied.success && !denied.wrote);
    TEST_CHECK(readFile(output) == kPrevious);
    fs::permissions(dir, fs::perms(0755));
  }

  void unrelatedTempFailure(const fs::path& root, TemplateEngine& engine) {
    const auto output = root / "blocked.conf";
    const auto temporary = fs::path(output.string() + ".tmp");
    writeFile(output, kPrevious);
    fs::create_directory(temporary);
    const auto result = engine.renderFile(root / "input.tmpl", output);
    TEST_CHECK(!result.success && !result.wrote);
    TEST_CHECK(readFile(output) == kPrevious);
    fs::remove(temporary);

    if (::geteuid() != 0) {
      // An unrelated unwritable .tmp file must not disable atomic protection.
      writeFile(temporary, "unrelated file");
      fs::permissions(temporary, fs::perms(0444));
      const auto denied = engine.renderFile(root / "input.tmpl", output);
      TEST_CHECK(!denied.success && !denied.wrote);
      TEST_CHECK(readFile(output) == kPrevious);
      TEST_CHECK(readFile(temporary) == "unrelated file");
      fs::remove(temporary);
    }
  }

  void failedWritePreservesOutput(const fs::path& root, TemplateEngine& engine) {
    const auto input = root / "large.tmpl";
    const auto output = root / "limited.conf";
    writeFile(input, std::string(8192, 'x'));
    writeFile(output, kPrevious);
    const pid_t child = ::fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
      // Force a partial write followed by EFBIG without filling a real
      // filesystem.
      std::signal(SIGXFSZ, SIG_IGN);
      const rlimit limit{.rlim_cur = 1024, .rlim_max = 1024};
      TEST_CHECK(::setrlimit(RLIMIT_FSIZE, &limit) == 0);
      const auto result = engine.renderFile(input, output);
      TEST_CHECK(!result.success && !result.wrote);
      TEST_CHECK(readFile(output) == kPrevious);
      TEST_CHECK(!fs::exists(output.string() + ".tmp"));
      ::_exit(EXIT_SUCCESS);
    }
    int status = 0;
    pid_t waited;
    do {
      waited = ::waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);
    TEST_CHECK(waited == child);
    TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
  }
} // namespace

int main() {
  const std::string pattern = (fs::temp_directory_path() / "noctalia-template-output-XXXXXX").string();
  auto buffer = pattern;
  const char* directory = ::mkdtemp(buffer.data());
  TEST_CHECK(directory != nullptr);
  const fs::path root(directory);
  writeFile(root / "input.tmpl", "surface = {{ colors.surface.default.hex }}\n");
  TemplateEngine::Options options;
  options.verbose = false;
  TemplateEngine engine(TemplateEngine::ThemeData{{"dark", {{"surface", "#123456"}}}}, options);
  atomicReplacement(root, engine);
  protectedDirectory(root, engine);
  unrelatedTempFailure(root, engine);
  failedWritePreservesOutput(root, engine);
  fs::remove_all(root);
}
