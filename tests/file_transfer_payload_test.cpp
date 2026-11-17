#include "filemanager/file_transfer_payload.h"
#include "tests/test_check.h"

#include <stdexcept>

using filemanager::decodeFileTransfer;
using filemanager::encodeFileTransfer;

namespace {
  template <typename Callable> void expectInvalid(Callable&& callable) {
    bool threw = false;
    try {
      callable();
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    TEST_CHECK(threw);
  }
} // namespace

int main() {
  const auto copy = encodeFileTransfer({"/tmp/space name", "/tmp/é"}, false);
  TEST_CHECK(copy.at("text/uri-list") == "file:///tmp/space%20name\r\nfile:///tmp/%C3%A9\r\n");
  TEST_CHECK(copy.at("x-special/gnome-copied-files") == "copy\nfile:///tmp/space%20name\nfile:///tmp/%C3%A9\n");
  TEST_CHECK(copy.at("application/x-kde-cutselection") == "0");

  const std::string trickyPath = "/tmp/line\nbreak/#percent%";
  const auto tricky = encodeFileTransfer({trickyPath}, false);
  TEST_CHECK(tricky.at("text/uri-list") == "file:///tmp/line%0Abreak/%23percent%25\r\n");
  const auto trickyDecoded = decodeFileTransfer(tricky.at("text/uri-list"), false);
  TEST_CHECK(trickyDecoded.paths == std::vector<std::string>{trickyPath});

  const auto cut = encodeFileTransfer({"/tmp/source"}, true);
  TEST_CHECK(cut.at("x-special/gnome-copied-files") == "cut\nfile:///tmp/source\n");
  TEST_CHECK(cut.at("application/x-kde-cutselection") == "1");

  const auto decodedGnome = decodeFileTransfer("cut\nfile:///tmp/a%20b\n", true);
  TEST_CHECK(decodedGnome.cut && decodedGnome.paths == std::vector<std::string>{"/tmp/a b"});
  const auto decodedKde = decodeFileTransfer("# file list\r\nfile://localhost/tmp/item%23one\r\n", false, true);
  TEST_CHECK(decodedKde.cut && decodedKde.paths == std::vector<std::string>{"/tmp/item#one"});
  const auto decodedUriList = decodeFileTransfer("file:///tmp/one\r\n\r\n", false);
  TEST_CHECK(!decodedUriList.cut && decodedUriList.paths == std::vector<std::string>{"/tmp/one"});

  expectInvalid([] { (void)decodeFileTransfer("file://example.com/tmp/item\n", false); });
  expectInvalid([] { (void)decodeFileTransfer("https://example.com/tmp/item\n", false); });
  expectInvalid([] { (void)decodeFileTransfer("file:///tmp/%Q0\n", false); });
  expectInvalid([] { (void)decodeFileTransfer("file:///tmp/item?query=1\n", false); });
  expectInvalid([] { (void)decodeFileTransfer("move\nfile:///tmp/item\n", true); });
  expectInvalid([] { (void)encodeFileTransfer({"relative/path"}, false); });
  expectInvalid([] { (void)encodeFileTransfer({std::string("/tmp/a\0b", 8)}, false); });
  expectInvalid([] { (void)decodeFileTransfer(std::string_view("file:///tmp/a\0b", 15), false); });
}
