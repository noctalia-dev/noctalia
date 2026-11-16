#include "filemanager/directory_metadata.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
  const std::vector<std::string> fallback{"system-folder", "folder"};
  const auto parse = [&](const std::string& text) {
    return filemanager::directoryMetadataIconNames(text.data(), text.size(), fallback);
  };

  const auto valid =
      parse("[Desktop Entry]\nType=Directory\nName=Ignored label\nExec=must-not-run\nIcon=custom-folder\n");
  assert((valid == std::vector<std::string>{"custom-folder", "system-folder", "folder"}));

  assert((parse("[Desktop Entry]\nType=Application\nIcon=wrong-type\n") == fallback));
  assert((parse("[Desktop Entry]\nIcon=missing-type\n") == fallback));
  assert((parse("[Desktop Entry]\nType=Directory\nName=Only a name\nExec=ignored\n") == fallback));
  assert((parse("this is not a desktop entry") == fallback));

  const std::string prefix = "[Desktop Entry]\nType=Directory\nIcon=folder";
  const std::string embeddedNull = prefix + std::string(1, '\0') + "bad";
  assert((filemanager::directoryMetadataIconNames(embeddedNull.data(), embeddedNull.size(), fallback) == fallback));
  assert((filemanager::directoryMetadataIconNames(nullptr, 20, fallback) == fallback));
  assert((filemanager::directoryMetadataIconNames("", 0, fallback) == fallback));
  const std::string oversized(filemanager::directoryMetadataMaxBytes + 1, 'x');
  assert((filemanager::directoryMetadataIconNames(oversized.data(), oversized.size(), fallback) == fallback));

  for (const auto* unsafe : {"/tmp/icon", "../icon", "./icon", "icons/custom", "icons\\custom", "file:custom"}) {
    const std::string content = std::string("[Desktop Entry]\nType=Directory\nIcon=") + unsafe + "\n";
    assert((parse(content) == fallback));
  }
  assert(
      (parse("[Desktop Entry]\nType=Directory\nIcon=folder\n") == std::vector<std::string>{"folder", "system-folder"})
  );
}
