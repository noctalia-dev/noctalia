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

  // Entry point for the hidden `noctalia pam-helper <service>` CLI mode that
  // authenticateCurrentUser() re-execs into. Reads a length-prefixed password
  // from stdin, runs the PAM conversation, writes the Result to stdout.
  // Exit codes: 0 authenticated, 1 rejected, 2 protocol/setup error.
  [[nodiscard]] static int runHelperMode(int argc, char* argv[]);
};
