#pragma once

#include <filesystem>
#include <string>

namespace net {

  // Queue a URL launch through the system default handler. Launch failures are logged asynchronously.
  bool openInBrowser(const std::string& url, const std::string& activationToken = {});

  // Opens a local file with its default application. Returns false for an empty or relative path.
  bool openFile(const std::filesystem::path& path, const std::string& activationToken = {});

  // Shows a local file selected in the file manager (org.freedesktop.FileManager1.ShowItems), falling
  // back to opening its parent folder. Returns false for an empty or relative path.
  bool revealFile(const std::filesystem::path& path, const std::string& activationToken = {});

} // namespace net
