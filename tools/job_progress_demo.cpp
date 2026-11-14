#include "dbus/session_bus.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  constexpr auto kService = "org.kde.JobViewServer";

  struct SyntheticJob {
    sdbus::ObjectPath path;
    std::atomic<bool> paused{false};
    std::atomic<bool> cancelled{false};
  };

  sdbus::ObjectPath registerJob(sdbus::IConnection& connection, std::int32_t capabilities,
                                const std::string& desktopEntry) {
    auto proxy = sdbus::createProxy(connection, sdbus::ServiceName{kService}, sdbus::ObjectPath{"/JobViewServer"});
    sdbus::ObjectPath path;
    proxy->callMethod("requestView").onInterface("org.kde.JobViewServerV2")
        .withArguments(desktopEntry, capabilities, std::map<std::string, sdbus::Variant>{}).storeResultsTo(path);
    return path;
  }

  void update(sdbus::IConnection& connection, const SyntheticJob& job, std::uint32_t percent,
              std::string title, std::uint64_t totalBytes, std::uint64_t totalFiles, unsigned elapsedSeconds) {
    const auto processedBytes = totalBytes * percent / 100;
    const auto processedFiles = totalFiles * percent / 100;
    const std::string description = title == "Copying"
        ? std::to_string(processedFiles) + " of " + std::to_string(totalFiles) + " files to Test destination"
        : "Synthetic read-only progress; no files are accessed";
    auto proxy = sdbus::createProxy(connection, sdbus::ServiceName{kService}, job.path);
    proxy->callMethod("update").onInterface("org.kde.JobViewV3").withArguments(
        std::map<std::string, sdbus::Variant>{
            {"title", sdbus::Variant(std::move(title))},
            {"infoMessage", sdbus::Variant(description)},
            {"destUrl", sdbus::Variant(std::string{"file:///tmp/nythera-synthetic-destination"})},
            {"percent", sdbus::Variant(percent)},
            {"suspended", sdbus::Variant(job.paused.load())},
            {"processedBytes", sdbus::Variant(processedBytes)},
            {"totalBytes", sdbus::Variant(totalBytes)},
            {"speed", sdbus::Variant(std::uint64_t{4 * 1024 * 1024})},
            {"elapsedTime", sdbus::Variant(static_cast<std::int64_t>(elapsedSeconds))},
            {"processedFiles", sdbus::Variant(processedFiles)},
            {"totalFiles", sdbus::Variant(totalFiles)},
            {"processedDirectories", sdbus::Variant(std::uint64_t{1})},
            {"totalDirectories", sdbus::Variant(std::uint64_t{1})},
            {"processedItems", sdbus::Variant(processedFiles + 1)},
            {"totalItems", sdbus::Variant(totalFiles + 1)},
            {"descriptionLabel1", sdbus::Variant(std::string{"Source"})},
            {"descriptionValue1", sdbus::Variant(std::string{"Synthetic source folder"})},
            {"descriptionLabel2", sdbus::Variant(std::string{"Destination"})},
            {"descriptionValue2", sdbus::Variant(std::string{"Synthetic destination folder"})},
        }
    );
  }

  void terminate(sdbus::IConnection& connection, const SyntheticJob& job, std::uint32_t code, std::string error) {
    auto proxy = sdbus::createProxy(connection, sdbus::ServiceName{kService}, job.path);
    proxy->callMethod("terminate").onInterface("org.kde.JobViewV3")
        .withArguments(code, error, std::map<std::string, sdbus::Variant>{});
  }
}

int main(int argc, char** argv) {
  try {
    const unsigned durationSeconds = argc > 1 ? static_cast<unsigned>(std::stoul(argv[1])) : 90U;
    if (durationSeconds < 10 || durationSeconds > 3600) {
      std::cerr << "Duration must be between 10 and 3600 seconds\n";
      return 2;
    }

    SessionBus bus;
    bus.connection().enterEventLoopAsync();
    auto controllable = std::make_shared<SyntheticJob>();
    auto readOnly = std::make_shared<SyntheticJob>();
    controllable->path = registerJob(bus.connection(), 3, "dev.nythera.JobProgressDemo");
    readOnly->path = registerJob(bus.connection(), 0, "dev.nythera.JobProgressDemo");

    const auto bindControls = [&bus](const std::shared_ptr<SyntheticJob>& job) {
      auto proxy = sdbus::createProxy(bus.connection(), sdbus::ServiceName{kService}, job->path);
      proxy->uponSignal("suspendRequested").onInterface("org.kde.JobViewV3").call([job] {
        job->paused.store(true);
        std::cout << "Pause requested for " << job->path << '\n';
      });
      proxy->uponSignal("resumeRequested").onInterface("org.kde.JobViewV3").call([job] {
        job->paused.store(false);
        std::cout << "Resume requested for " << job->path << '\n';
      });
      proxy->uponSignal("cancelRequested").onInterface("org.kde.JobViewV3").call([job] {
        job->cancelled.store(true);
        std::cout << "Cancel requested for " << job->path << '\n';
      });
      return proxy;
    };
    std::vector<std::unique_ptr<sdbus::IProxy>> controlProxies;
    controlProxies.push_back(bindControls(controllable));
    controlProxies.push_back(bindControls(readOnly));

    std::cout << "Synthetic jobs are running for up to " << durationSeconds
              << " seconds. First job supports Pause/Resume/Cancel; second is read-only.\n"
              << "Open Control Center > Notifications to inspect and expand them.\n"
              << "No filesystem operations will be performed.\n" << std::flush;

    std::uint32_t controllablePercent = 0;
    for (unsigned elapsed = 0; elapsed <= durationSeconds; ++elapsed) {
      const auto percent = static_cast<std::uint32_t>(elapsed * 100U / durationSeconds);
      if (!controllable->cancelled.load()) {
        if (!controllable->paused.load()) controllablePercent = percent;
        // Keep reporting while paused so the shell sees the client's acknowledged state.
        update(bus.connection(), *controllable, controllablePercent, "Copying", 900000000, 30, elapsed);
      }
      if (!readOnly->paused.load()) update(bus.connection(), *readOnly, percent, "Indexing", 420000000, 12, elapsed);
      if (controllable->cancelled.load()) break;
      if (elapsed == durationSeconds) break;
      std::this_thread::sleep_for(1s);
    }

    if (controllable->cancelled.load()) terminate(bus.connection(), *controllable, 1, "Synthetic job cancelled");
    else terminate(bus.connection(), *controllable, 0, {});
    terminate(bus.connection(), *readOnly, 0, {});
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "job_progress_demo: " << error.what() << '\n';
    return 1;
  }
}
