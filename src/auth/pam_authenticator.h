#pragma once

#include <string>
#include <string_view>

class PamAuthenticator {
public:
  struct [[nodiscard]] Result {
    bool success = false;
    std::string message;
  };

  // Pre-translated texts: authentication runs off the main thread.
  struct Messages {
    std::string startFailed;
    std::string userUnavailable;
    std::string authenticationFailed;
  };

  Result authenticateCurrentUser(std::string_view password, const Messages& messages) const;
  static std::string currentUsername();
};
