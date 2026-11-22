#include "filemanager/file_operation_manager.h"
#include "test_isolation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

// Exercise the production signal router and hold the real GIO worker at its
// first partial-copy callback until the main thread can request suspension.
class FileOperationManagerTestAccess {
public:
  static void boundary(FileOperationManager& manager, std::function<void(std::uint64_t, std::uint64_t)> observer) {
    manager.m_transferBoundary = std::move(observer);
  }
  static void signal(FileOperationManager& manager, std::uint64_t id, const char* name) {
    const std::string path = "/test/transfer";
    manager.m_jobPaths[id] = path;
    FileOperationManager::jobSignal(nullptr, nullptr, path.c_str(), nullptr, name, nullptr, &manager);
    manager.m_jobPaths.erase(id);
  }
};

namespace {
  using namespace std::chrono_literals;
  void pump() {
    while (g_main_context_iteration(nullptr, false)) {
    }
  }
  template <class Predicate> void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      pump();
      std::this_thread::sleep_for(1ms);
    }
    pump();
    TEST_CHECK(predicate());
  }
  const FileOperationManager::Operation& operation(FileOperationManager& manager, std::uint64_t id) {
    const auto found = std::ranges::find(manager.operations(), id, &FileOperationManager::Operation::id);
    TEST_CHECK(found != manager.operations().end());
    return *found;
  }
  std::filesystem::path staged(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory))
      if (entry.path().filename().string().starts_with(".nythera-partial-"))
        return entry.path();
    return {};
  }
} // namespace

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-pause"});
    const auto& root = env.work();
    // Crossing filesystems avoids a whole-file kernel fast copy completing
    // before the first GIO progress callback on small local fixtures.
    nythera::test::ScopedTempDir destinationDir("nythera-pause", "/var/tmp");
    const auto& destinationRoot = destinationDir.path();
    const auto source = root / "payload.bin";
    constexpr std::uint64_t size = 16 * 1024 * 1024;
    const std::string chunk(64 * 1024, 'p');
    {
      std::ofstream file(source, std::ios::binary);
      for (std::uint64_t written = 0; written < size; written += chunk.size())
        file.write(chunk.data(), chunk.size());
      TEST_CHECK(file.good());
    }
    for (const bool cancel : {false, true}) {
      const auto destination = destinationRoot / (cancel ? "cancel" : "resume");
      std::filesystem::create_directory(destination);
      std::atomic_bool reached = false;
      std::mutex mutex;
      std::condition_variable changed;
      bool released = false;
      FileOperationManager manager;
      const auto releaseWorkerGuard = nythera::test::makeScopeExit([&] {
        {
          std::lock_guard lock(mutex);
          released = true;
        }
        changed.notify_all();
      });
      FileOperationManagerTestAccess::boundary(manager, [&](std::uint64_t, std::uint64_t bytes) {
        if (bytes == 0 || bytes >= size || reached.exchange(true))
          return;
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return released; });
      });
      const auto id = cancel ? manager.move({source.string()}, destination.string())
                             : manager.copy({source.string()}, destination.string());
      await([&] { return reached.load(); });
      FileOperationManagerTestAccess::signal(manager, id, "suspendRequested");
      {
        std::lock_guard lock(mutex);
        released = true;
      }
      changed.notify_all();
      await([&] { return operation(manager, id).paused; });
      const auto partial = staged(destination);
      TEST_CHECK(!partial.empty());
      const auto stoppedSize = std::filesystem::file_size(partial);
      const auto stoppedBytes = operation(manager, id).processedBytes;
      TEST_CHECK(stoppedSize > 0 && stoppedSize < size);
      TEST_CHECK(stoppedBytes > 0 && stoppedBytes < size);
      const auto until = std::chrono::steady_clock::now() + 300ms;
      while (std::chrono::steady_clock::now() < until) {
        pump();
        TEST_CHECK(std::filesystem::file_size(partial) == stoppedSize);
        TEST_CHECK(operation(manager, id).processedBytes == stoppedBytes);
        TEST_CHECK(!operation(manager, id).finished);
        std::this_thread::sleep_for(5ms);
      }
      TEST_CHECK(operation(manager, id).displaySpeedBytesPerSecond == 0);
      TEST_CHECK(operation(manager, id).etaBytesPerSecond == 0);
      FileOperationManagerTestAccess::signal(manager, id, cancel ? "cancelRequested" : "resumeRequested");
      await([&] { return operation(manager, id).finished; });
      TEST_CHECK(operation(manager, id).succeeded == !cancel);
      TEST_CHECK(!operation(manager, id).paused);
      TEST_CHECK(staged(destination).empty());
      TEST_CHECK(std::filesystem::file_size(source) == size);
      if (cancel) {
        TEST_CHECK(!std::filesystem::exists(destination / source.filename()));
        TEST_CHECK(operation(manager, id).completedEntries.empty());
      } else {
        const auto copied = destination / source.filename();
        TEST_CHECK(std::filesystem::file_size(copied) == size);
        std::ifstream file(copied, std::ios::binary);
        std::string contents(chunk.size(), '\0');
        for (std::uint64_t read = 0; read < size; read += chunk.size()) {
          file.read(contents.data(), contents.size());
          TEST_CHECK(contents == chunk);
        }
        TEST_CHECK(operation(manager, id).completedEntries.size() == 1);
      }
    }
  });
}
