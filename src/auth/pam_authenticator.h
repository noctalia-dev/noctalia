#pragma once

#include <string>
#include <string_view>

class PamAuthenticator {
public:
  struct [[nodiscard]] Result {
    bool success = false;
    std::string message;
  };

  Result authenticateCurrentUser(
      std::string_view password, std::string_view service, std::string_view language,
      std::string_view startFailureMessage
  ) const;
  static std::string currentUsername();
  [[nodiscard]] static int runHelperMode(int argc, char* argv[]);
};
