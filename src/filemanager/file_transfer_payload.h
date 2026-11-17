#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace filemanager {

  struct FileTransferPayload {
    std::vector<std::string> paths;
    bool cut = false;
  };

  // Throws std::invalid_argument if a path cannot be represented as a local file URI.
  [[nodiscard]] std::map<std::string, std::string> encodeFileTransfer(std::vector<std::string> paths, bool cut);

  // Decode either text/uri-list (gnome=false) or x-special/gnome-copied-files
  // (gnome=true). When kdeCut is set, KDE's cutselection metadata marks the
  // URI-list as cut. Malformed or non-local URIs throw std::invalid_argument.
  [[nodiscard]] FileTransferPayload decodeFileTransfer(std::string_view payload, bool gnome, bool kdeCut = false);

} // namespace filemanager
