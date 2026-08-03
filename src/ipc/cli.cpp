#include "ipc/cli.h"

#include "cli/parse.h"
#include "cli/schema_ipc.h"
#include "ipc/ipc_client.h"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <string_view>

namespace noctalia::ipc {

  int runCli(int argc, char* argv[]) {
    if (argc < 3) {
      std::println(stderr, "error: msg requires a command (try: noctalia msg --help)");
      return 1;
    }

    const std::string_view subcmd = argv[2];

    if (subcmd == "--help") {
      if (IpcClient::send("--help") == 0) {
        return 0;
      }

      // Fallback
      std::println("Usage: noctalia msg <command> [args...]\n");
      std::println("Commands:");
      for (const auto& cmd : getIpcSubcommands()) {
        std::println("  {:<24} {}", cmd.name, cmd.summary);
      }
      return 0;
    }

    if (subcmd == "notification-show") {
      const auto parsed = cli_schema::parseArgs(argc, argv, 3, kNotificationCmd);
      if (!parsed) {
        std::println(stderr, "error: {}", parsed.error());
        std::println(stderr, "Run 'noctalia msg notification-show --help' for usage.");
        return 1;
      }

      if (parsed->helpRequested) {
        return 0;
      }

      nlohmann::json payload = {
          {"summary", parsed->positionals[0]},
          {"body", parsed->positionals[1]},
      };

      if (parsed->positionals.size() > 2) {
        std::string body = parsed->positionals[1];
        for (std::size_t i = 2; i < parsed->positionals.size(); ++i) {
          body += ' ';
          body += parsed->positionals[i];
        }
        payload["body"] = std::move(body);
      }

      return IpcClient::send(std::string(subcmd) + " " + payload.dump());
    }

    std::string cmd = argv[2];
    for (int i = 3; i < argc; ++i) {
      cmd += ' ';
      cmd += argv[i];
    }
    return IpcClient::send(cmd);
  }

} // namespace noctalia::ipc
