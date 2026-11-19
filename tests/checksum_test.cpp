#include "filemanager/checksum.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  constexpr auto md5 = "900150983cd24fb0d6963f7d28e17f72";
  constexpr auto sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

  void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), bytes.size());
    out.close();
    assert(out);
  }
} // namespace

int main() {
  using namespace filemanager::checksum;
  const auto dir = std::filesystem::temp_directory_path() / ("nythera-checksum-" + std::to_string(getpid()));
  assert(std::filesystem::create_directory(dir));
  const auto file = dir / "file with spaces.txt";
  write(file, "abc");
  for (const auto& [algorithm, expected] :
       {std::pair{Algorithm::Sha256, sha256},
        std::pair{
            Algorithm::Sha512,
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
            "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"
        },
        std::pair{Algorithm::Md5, md5}}) {
    Result result;
    GError* error = nullptr;
    assert(calculate(file, algorithm, nullptr, result, &error));
    assert(error == nullptr && result.hash == expected && result.bytes == 3);
    assert(compare(result.hash, expected) == Comparison::Match);
    assert(compare(result.hash, std::string(result.hash.size(), '0')) == Comparison::Mismatch);
  }
  assert(compare(md5, " 900150983CD24FB0D6963F7D28E17F72\r\n") == Comparison::Match);
  assert(fromHash(std::string(32, 'a')) == Algorithm::Md5);
  assert(fromHash(std::string(64, 'a')) == Algorithm::Sha256);
  assert(fromHash(std::string(128, 'a')) == Algorithm::Sha512);
  assert(!fromHash("abc"));
  assert(fromManifestName("FILE.SHA256") == Algorithm::Sha256);
  assert(fromManifestName("FILE.sha512") == Algorithm::Sha512);
  assert(fromManifestName("FILE.md5") == Algorithm::Md5);
  assert(!fromManifestName("SUMS"));
  assert(validHash(md5, Algorithm::Md5));
  assert(!validHash(std::string(32, 'z'), Algorithm::Md5));
  assert(!validHash(md5, Algorithm::Sha256));

  const auto manifest = dir / "test.md5";
  std::string expected;
  GError* error = nullptr;
  const auto load = [&](const std::filesystem::path& target) {
    return loadExpected(manifest, target, Algorithm::Md5, nullptr, expected, &error);
  };
  const auto expectError = [&](GIOErrorEnum code) {
    assert(g_error_matches(error, G_IO_ERROR, static_cast<int>(code)));
    g_clear_error(&error);
  };
  write(manifest, "# comment\r\n\r\n" + std::string(md5) + " *file with spaces.txt\r\n");
  assert(load(file) && expected == md5);
  write(manifest, std::string(md5) + "\n");
  assert(load(file) && expected == md5);
  const auto special = dir / " *quotes'\\slash\nline\r.txt";
  write(manifest, "\\" + std::string(md5) + "   *quotes'\\\\slash\\nline\\r.txt\n");
  assert(load(special) && expected == md5);
  write(manifest, std::string(md5) + "  sub/file with spaces.txt\n");
  assert(load(dir / "sub" / file.filename()) && expected == md5);
  assert(!load(file));
  expectError(G_IO_ERROR_INVALID_DATA);

  const auto named = std::string(md5) + "  file with spaces.txt\n";
  for (const auto& malformed :
       {std::string("bad file with spaces.txt"), "\\" + std::string(md5) + "  invalid\\x",
        std::string(md5) + "  ../file with spaces.txt", std::string(md5) + "  /file with spaces.txt",
        named + std::string(32, '0') + "  file with spaces.txt\n", std::string(md5) + "\n" + named,
        std::string(4 * 1024 * 1024 + 1, 'a')}) {
    write(manifest, malformed);
    assert(!load(file));
    expectError(G_IO_ERROR_INVALID_DATA);
  }

  const auto link = dir / "link";
  const auto fifo = dir / "fifo";
  std::filesystem::create_symlink(file, link);
  assert(mkfifo(fifo.c_str(), 0600) == 0);
  assert(eligibleSelection(file, 1, true));
  assert(!eligibleSelection(file, 0, true));
  assert(!eligibleSelection(file, 2, true));
  assert(!eligibleSelection(file, 1, false));
  assert(!eligibleSelection(file.filename(), 1, true));
  Result result;
  for (const auto& unsupported : {link, dir, fifo}) {
    assert(!eligibleSelection(unsupported, 1, true));
    assert(!calculate(unsupported, Algorithm::Sha256, nullptr, result, &error));
    expectError(G_IO_ERROR_NOT_REGULAR_FILE);
  }
  assert(!calculate(dir / "missing", Algorithm::Sha256, nullptr, result, &error));
  expectError(G_IO_ERROR_NOT_FOUND);
  assert(chmod(file.c_str(), 0000) == 0);
  if (geteuid() != 0) {
    assert(!calculate(file, Algorithm::Sha256, nullptr, result, &error));
    expectError(G_IO_ERROR_PERMISSION_DENIED);
  }
  assert(chmod(file.c_str(), 0600) == 0);
  // This regular procfs entry fails read(2) at offset zero with EIO.
  assert(!calculate("/proc/self/mem", Algorithm::Sha256, nullptr, result, &error));
  expectError(G_IO_ERROR_FAILED);

  GCancellable* cancel = g_cancellable_new();
  assert(!calculate(file, Algorithm::Sha256, cancel, result, &error, [&](auto done, auto total) {
    assert(total == 3);
    if (done > 0)
      g_cancellable_cancel(cancel);
  }));
  expectError(G_IO_ERROR_CANCELLED);
  assert(!calculate(file, Algorithm::Sha256, cancel, result, &error));
  expectError(G_IO_ERROR_CANCELLED);
  assert(!loadExpected(manifest, file, Algorithm::Md5, cancel, expected, &error));
  expectError(G_IO_ERROR_CANCELLED);
  g_object_unref(cancel);

  for (const bool replace : {false, true}) {
    write(file, "abc");
    assert(!calculate(file, Algorithm::Sha256, nullptr, result, &error, [&](auto done, auto) {
      if (done == 0)
        return;
      if (replace) {
        std::filesystem::rename(file, dir / "original");
        write(file, "abc");
      } else {
        write(file, "def");
      }
    }));
    expectError(G_IO_ERROR_BUSY);
  }
  std::filesystem::remove_all(dir);
}
