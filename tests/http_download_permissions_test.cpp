#include "core/deferred_call.h"
#include "net/http_client.h"
#include "test_check.h"

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  void finish(HttpClient& client) {
    const std::vector<pollfd> fds;
    for (int i = 0; i < 20; ++i) {
      client.dispatch(fds, 0);
      for (auto& callback : DeferredCall::takePending()) {
        callback();
      }
    }
  }

  std::string read(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  void checkPrivateTemps(const std::filesystem::path& directory, std::string_view prefix, std::size_t expected) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (!entry.path().filename().string().starts_with(prefix)) {
        continue;
      }
      ++count;
      TEST_CHECK(
          (entry.status().permissions() & std::filesystem::perms::all) == static_cast<std::filesystem::perms>(0600)
      );
      bool closeOnExec = false;
      for (const auto& descriptor : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code ec;
        if (std::filesystem::read_symlink(descriptor.path(), ec) == entry.path()) {
          const int fd = std::stoi(descriptor.path().filename().string());
          closeOnExec = (::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
        }
      }
      TEST_CHECK(closeOnExec);
    }
    TEST_CHECK(count == expected);
  }
} // namespace

int main() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / ("noctalia-http-download-test-" + std::to_string(::getpid()));
  fs::create_directories(root);
  const auto source = root / "source.json";
  const auto destination = root / "result.json";
  const auto sentinel = root / "sentinel";
  std::ofstream(source) << "private coordinates";
  std::ofstream(sentinel) << "unchanged";
  std::ofstream(destination) << "old data";
  fs::permissions(destination, static_cast<fs::perms>(0644));
  const std::string url = "file://" + source.string();
  const mode_t oldUmask = ::umask(0000);

  fs::create_symlink(sentinel, destination.string() + ".part");
  {
    HttpClient client;
    int completed = 0;
    const auto callback = [&completed](bool ok) {
      TEST_CHECK(ok);
      ++completed;
    };
    client.download(url, destination, callback);
    client.download(url, destination, callback);
    checkPrivateTemps(root, "result.json.part.", 1);
    finish(client);
    TEST_CHECK(completed == 2);
    TEST_CHECK(read(destination) == "private coordinates");
    TEST_CHECK(read(sentinel) == "unchanged");
    TEST_CHECK((fs::status(destination).permissions() & fs::perms::all) == static_cast<fs::perms>(0600));
    checkPrivateTemps(root, "result.json.part.", 0);
  }

  fs::remove(destination.string() + ".part");
  TEST_CHECK(::mkfifo((destination.string() + ".part").c_str(), 0600) == 0);
  {
    HttpClient client;
    bool completed = false;
    client.download(url, destination, [&](bool ok) {
      TEST_CHECK(ok);
      completed = true;
    });
    finish(client);
    TEST_CHECK(completed);
  }

  {
    HttpClient client;
    bool failed = false;
    client.download("file://" + (root / "missing").string(), root / "failed", [&](bool ok) { failed = !ok; });
    finish(client);
    TEST_CHECK(failed);

    failed = false;
    client.download(url, root / "missing-directory/file", [&](bool ok) { failed = !ok; });
    TEST_CHECK(!failed);
    finish(client);
    TEST_CHECK(failed);
    TEST_CHECK(!fs::exists(root / "missing-directory"));
    TEST_CHECK(!fs::exists(root / "failed"));
    checkPrivateTemps(root, "failed.part.", 0);

    fs::create_directory(root / "directory");
    client.download(url, root / "directory", [&](bool ok) { failed = !ok; });
    failed = false;
    finish(client);
    TEST_CHECK(failed);
    TEST_CHECK(fs::is_directory(root / "directory"));
    checkPrivateTemps(root, "directory.part.", 0);

    failed = false;
    client.download(url, {}, [&](bool ok) { failed = !ok; });
    TEST_CHECK(!failed);
    finish(client);
    TEST_CHECK(failed);
  }

  {
    HttpClient client;
    client.download(url, root / "cancelled", [](bool) { TEST_CHECK(false); });
    checkPrivateTemps(root, "cancelled.part.", 1);
  }
  checkPrivateTemps(root, "cancelled.part.", 0);
  ::umask(oldUmask);
  fs::remove_all(root);
}
