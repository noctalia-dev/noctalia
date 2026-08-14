#pragma once

#include <string>

namespace net {

  // Launches the user's configured default application for a MIME type (the same
  // association GNOME/KDE "Default Applications" settings and `xdg-mime` read/write),
  // with no file argument -- just starts the app. Returns false when no default is
  // set or the launch fails.
  bool openDefaultAppForMimeType(const std::string& mimeType);

} // namespace net
