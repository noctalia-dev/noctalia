#include "compositors/sway/sway_keyboard_backend.h"

#include "compositors/sway/sway_runtime.h"
#include "system/process.h"

#include <chrono>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  constexpr auto kCurrentLayoutCacheTtl = std::chrono::seconds(1);

  struct CurrentLayoutCache {
    std::optional<std::string> value;
    std::chrono::steady_clock::time_point fetchedAt;
    bool valid = false;
  };

  CurrentLayoutCache& currentLayoutCache() {
    static CurrentLayoutCache cache;
    return cache;
  }

  void invalidateCurrentLayoutCache() { currentLayoutCache() = CurrentLayoutCache{}; }

  [[nodiscard]] std::optional<std::string> runAndCapture(const std::vector<std::string>& args) {
    if (args.empty() || args.front().empty()) {
      return std::nullopt;
    }

    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) != 0) {
      return std::nullopt;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
      ::close(pipefd[0]);
      ::close(pipefd[1]);
      return std::nullopt;
    }

    if (pid == 0) {
      ::close(pipefd[0]);
      ::dup2(pipefd[1], STDOUT_FILENO);
      ::close(pipefd[1]);

      std::vector<char*> argv;
      argv.reserve(args.size() + 1);
      for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
      }
      argv.push_back(nullptr);
      ::execvp(argv[0], argv.data());
      ::_exit(127);
    }

    ::close(pipefd[1]);
    std::string output;
    char buffer[4096];
    ssize_t count = 0;
    while ((count = ::read(pipefd[0], buffer, sizeof(buffer))) > 0) {
      output.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(pipefd[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      return std::nullopt;
    }

    return output;
  }

  // The `change` values of a sway input event that can move a keyboard's active layout.
  [[nodiscard]] bool isLayoutChange(std::string_view payload) {
    try {
      const auto json = nlohmann::json::parse(payload);
      if (!json.is_object()) {
        return false;
      }
      const std::string change = json.value("change", "");
      return change == "xkb_layout" || change == "xkb_keymap";
    } catch (const nlohmann::json::exception&) {
      return false;
    }
  }

} // namespace

SwayKeyboardBackend::SwayKeyboardBackend(compositors::sway::SwayRuntime& runtime)
    : compositors::sway::SwayEventHandler(runtime) {}

bool SwayKeyboardBackend::isAvailable() const noexcept { return m_runtime.hasMsgCommand(); }

bool SwayKeyboardBackend::cycleLayout() const {
  if (!isAvailable()) {
    return false;
  }
  const bool ok = process::runSync({m_runtime.msgCommand(), "input", "type:keyboard", "xkb_switch_layout", "next"});
  if (ok) {
    invalidateCurrentLayoutCache();
  }
  return ok;
}

std::optional<KeyboardLayoutState> SwayKeyboardBackend::layoutState() const {
  const auto current = currentLayoutName();
  if (!current.has_value()) {
    return std::nullopt;
  }
  return KeyboardLayoutState{{*current}, 0};
}

std::optional<std::string> SwayKeyboardBackend::currentLayoutName() const {
  if (!isAvailable()) {
    return std::nullopt;
  }

  const auto now = std::chrono::steady_clock::now();
  auto& cache = currentLayoutCache();
  if (cache.valid && now - cache.fetchedAt < kCurrentLayoutCacheTtl) {
    return cache.value;
  }

  auto finish = [&](std::optional<std::string> value) {
    cache.value = std::move(value);
    cache.fetchedAt = now;
    cache.valid = true;
    return cache.value;
  };

  const auto payload = runAndCapture({m_runtime.msgCommand(), "-t", "get_inputs", "--raw"});
  if (!payload.has_value() || payload->empty()) {
    return finish(std::nullopt);
  }

  try {
    const auto json = nlohmann::json::parse(*payload);
    if (!json.is_array()) {
      return finish(std::nullopt);
    }
    for (const auto& input : json) {
      if (!input.is_object()) {
        continue;
      }
      if (input.value("type", "") != "keyboard") {
        continue;
      }
      const std::string layout = input.value("xkb_active_layout_name", "");
      if (!layout.empty()) {
        return finish(layout);
      }
    }
  } catch (const nlohmann::json::exception&) {
    return finish(std::nullopt);
  }

  return finish(std::nullopt);
}

void SwayKeyboardBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void SwayKeyboardBackend::handleMessage(std::uint32_t type, std::string_view payload) {
  if (type == compositors::sway::ipc::kInputEvent && !m_layoutChanged) {
    m_layoutChanged = isLayoutChange(payload);
  }
}

// sway sends one input event per keyboard device on a single switch; notify once per read so a
// switch costs one layout lookup instead of one per device.
void SwayKeyboardBackend::handleBatchEnd() {
  if (!m_layoutChanged) {
    return;
  }
  m_layoutChanged = false;
  notifyChanged();
}

void SwayKeyboardBackend::handleDisconnect() {
  m_layoutChanged = false;
  notifyChanged();
}

void SwayKeyboardBackend::notifyChanged() {
  // Drop the cached name so the callback's refresh reads the layout sway just switched to.
  invalidateCurrentLayoutCache();
  if (m_changeCallback) {
    m_changeCallback();
  }
}
