#include "scripting/plugin_git.h"

#include "core/log.h"
#include "core/process/process.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace scripting::plugin_git {

  namespace {
    using namespace std::chrono_literals;

    constexpr Logger kLog("plugins.git");
    // Git ops slower than this are logged at info so plugin-export stalls surface without
    // enabling debug; everything else is debug-only.
    constexpr double kSlowGitMs = 1000.0;

    // Hard backstop that kills a wedged git subprocess (e.g. a connect/DNS hang behind a
    // proxy). Network ops also abort early via http.lowSpeed* below. Note object reads
    // from the blobless clone (show / cat-file) lazy-fetch missing blobs, so even
    // "local" ops can hit the network; run every git op off the main thread.
    constexpr auto kNetworkTimeout = 60s;
    constexpr auto kLocalTimeout = 20s;
    // File bodies we read (catalog.toml / plugin.toml) are small; cap defensively.
    constexpr std::size_t kFileCap = 4 * 1024 * 1024;
    constexpr std::size_t kProgressCap = 64 * 1024;

    std::string nonInteractiveSshCommand() {
      if (const char* existing = std::getenv("GIT_SSH_COMMAND"); existing != nullptr && existing[0] != '\0') {
        return std::string(existing) + " -oBatchMode=yes";
      }
      return "ssh -oBatchMode=yes";
    }

    std::vector<process::EnvOverride> nonInteractiveGitEnv() {
      return {
          {.name = "GIT_TERMINAL_PROMPT", .value = "0"},
          {.name = "GIT_ASKPASS", .value = "/bin/false"},
          {.name = "SSH_ASKPASS", .value = "/bin/false"},
          {.name = "SSH_ASKPASS_REQUIRE", .value = "never"},
          {.name = "GIT_SSH_COMMAND", .value = nonInteractiveSshCommand()},
      };
    }

    GitResult
    run(std::vector<std::string> args, std::chrono::milliseconds timeout, std::size_t cap,
        std::vector<process::EnvOverride> extraEnv = {}) {
      // The caller's command, before the injected -c hardening flags, for timing logs.
      std::string desc;
      for (const auto& arg : args) {
        if (!desc.empty()) {
          desc += ' ';
        }
        desc += arg;
      }

      if (!args.empty() && args.front() == "git") {
        // Abort an HTTP transfer stalled below ~1 KB/s for 20s (a half-up proxy / wedged
        // tunnel) instead of waiting out the hard timeout. Ignored by non-HTTP transports.
        args.insert(
            args.begin() + 1,
            {"-c", "credential.interactive=false", "-c", "core.askPass=/bin/false", "-c", "http.lowSpeedLimit=1000",
             "-c", "http.lowSpeedTime=20"}
        );
      }
      process::RunOptions options;
      options.timeout = timeout;
      options.maxOutputBytes = cap;
      options.env = nonInteractiveGitEnv();
      options.env.insert(options.env.end(), extraEnv.begin(), extraEnv.end());

      const auto start = std::chrono::steady_clock::now();
      const auto r = process::runSync(args, std::move(options));
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      if (ms >= kSlowGitMs) {
        kLog.info("{} took {:.0f}ms (exit {})", desc, ms, r.exitCode);
      } else {
        kLog.debug("{} took {:.0f}ms (exit {})", desc, ms, r.exitCode);
      }

      return GitResult{
          .ok = static_cast<bool>(r),
          .exitCode = r.exitCode,
          .out = r.out,
          .err = r.err,
          .timedOut = r.timedOut,
      };
    }

    std::string trimTrailingNewline(std::string s) {
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
      }
      return s;
    }
  } // namespace

  bool available() { return process::commandExists("git"); }

  GitResult cloneBlobless(const std::string& url, const std::filesystem::path& dest) {
    // Blobless but NOT shallow: full commit/tree history (still tiny — no file
    // blobs until needed), so later fetches can inspect history normally. A `--depth 1`
    // shallow clone grafts fetched commits as disjoint roots ("unrelated histories").
    return run(
        {"git", "clone", "--filter=blob:none", "--no-checkout", url, dest.string()}, kNetworkTimeout, kProgressCap
    );
  }

  GitResult
  showFile(const std::filesystem::path& dest, std::string_view repoPath, std::string_view rev, bool localOnly) {
    std::vector<process::EnvOverride> env;
    if (localOnly) {
      // Error out on a blob missing from the local object store instead of lazy-fetching
      // it (git >= 2.45; older git ignores the variable and may still fetch).
      env.push_back({.name = "GIT_NO_LAZY_FETCH", .value = "1"});
    }
    return run(
        {"git", "-C", dest.string(), "show", std::string(rev) + ":" + std::string(repoPath)}, kLocalTimeout, kFileCap,
        std::move(env)
    );
  }

  GitResult exportSubdir(
      const std::filesystem::path& dest, std::string_view rev, std::string_view subdir,
      const std::filesystem::path& workTree
  ) {
    std::error_code ec;
    std::filesystem::create_directories(workTree, ec);
    if (ec) {
      return GitResult{.ok = false, .exitCode = -1, .out = {}, .err = ec.message(), .timedOut = false};
    }
    std::vector<process::EnvOverride> env{
        {.name = "GIT_INDEX_FILE", .value = (workTree / ".git-index").string()},
    };
    return run(
        {"git", "-C", dest.string(), "--work-tree", workTree.string(), "checkout", std::string(rev), "--",
         std::string(subdir)},
        kNetworkTimeout, kProgressCap, std::move(env)
    );
  }

  GitResult fetch(const std::filesystem::path& dest) {
    // Updates remote-tracking refs + FETCH_HEAD without touching the working tree.
    return run({"git", "-C", dest.string(), "fetch", "origin"}, kNetworkTimeout, kProgressCap);
  }

  GitResult remoteHead(const std::filesystem::path& dest) {
    auto r = run({"git", "-C", dest.string(), "rev-parse", "FETCH_HEAD"}, kLocalTimeout, kProgressCap);
    r.out = trimTrailingNewline(std::move(r.out));
    return r;
  }

  GitResult setHead(const std::filesystem::path& dest, std::string_view rev) {
    return run({"git", "-C", dest.string(), "update-ref", "HEAD", std::string(rev)}, kLocalTimeout, kProgressCap);
  }

  GitResult headRevision(const std::filesystem::path& dest) {
    auto r = run({"git", "-C", dest.string(), "rev-parse", "HEAD"}, kLocalTimeout, kProgressCap);
    r.out = trimTrailingNewline(std::move(r.out));
    return r;
  }

  bool hasPath(const std::filesystem::path& dest, std::string_view repoPath, std::string_view rev) {
    return run({"git", "-C", dest.string(), "cat-file", "-e", std::string(rev) + ":" + std::string(repoPath)},
               kLocalTimeout, kProgressCap)
        .ok;
  }

} // namespace scripting::plugin_git
