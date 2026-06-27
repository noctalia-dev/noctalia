#pragma once

#include <string>
#include <string_view>

class PamAuthenticator {
public:
  struct Result {
    bool success = false;
    std::string message;
  };

  [[nodiscard]] Result authenticateCurrentUser(std::string_view password, std::string_view service = "login") const;
  [[nodiscard]] static std::string currentUsername();
};
