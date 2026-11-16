#include "filemanager/gio_poll_source.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <poll.h>
#include <vector>

namespace {
  gboolean markDispatched(gpointer data) {
    *static_cast<bool*>(data) = true;
    return G_SOURCE_REMOVE;
  }
} // namespace

int main() {
  GMainContext* context = g_main_context_new();
  {
    GioPollSource pollSource(context);
    std::vector<pollfd> fds;
    pollSource.addPollFds(fds);
    assert(g_main_context_is_owner(context));
    pollSource.finishPoll();
    assert(!g_main_context_is_owner(context));

    bool dispatched = false;
    GSource* idle = g_idle_source_new();
    g_source_set_callback(idle, markDispatched, &dispatched, nullptr);
    g_source_attach(idle, context);
    g_source_unref(idle);
    fds.clear();
    const auto start = pollSource.addPollFds(fds);
    assert(g_main_context_is_owner(context));
    pollSource.dispatch(fds, start);
    assert(dispatched);
    pollSource.finishPoll();
    assert(!g_main_context_is_owner(context));

    const auto pass = [&](int maxWait) {
      std::vector<pollfd> readyFds;
      const auto offset = pollSource.addPollFds(readyFds);
      const int timeout = pollSource.pollTimeoutMs();
      const int wait = timeout < 0 ? maxWait : std::min(timeout, maxWait);
      assert(poll(readyFds.data(), readyFds.size(), wait) >= 0);
      pollSource.dispatch(readyFds, offset);
      pollSource.finishPoll();
      return timeout;
    };
    // A continuing ready source legitimately votes zero on each iteration.
    // The adapter must stop voting zero once the source removes itself.
    unsigned calls = 0;
    GSource* burst = g_idle_source_new();
    g_source_set_name(burst, "finite-fixture-burst");
    g_source_set_callback(
        burst,
        [](gpointer data) -> gboolean {
          return ++*static_cast<unsigned*>(data) < 64 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
        },
        &calls, nullptr
    );
    g_source_attach(burst, context);
    g_source_unref(burst);
    for (unsigned i = 0; i < 64; ++i)
      assert(pass(0) == 0);
    assert(calls == 64);
    assert(pass(0) != 0);

    // Real GTask completions use the same bridge as folder scans/metadata jobs.
    bool taskDone = false;
    g_main_context_push_thread_default(context);
    auto* task = g_task_new(
        nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer data) {
          assert(g_task_propagate_boolean(G_TASK(result), nullptr));
          *static_cast<bool*>(data) = true;
        },
        &taskDone
    );
    g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer, GCancellable*) {
      g_task_return_boolean(worker, TRUE);
    });
    g_object_unref(task);
    g_main_context_pop_thread_default(context);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!taskDone && std::chrono::steady_clock::now() < deadline)
      pass(100);
    assert(taskDone);
    assert(pass(0) != 0);

    // A local monitor burst drains too; no user mounts or files are consulted.
    char* raw = g_dir_make_tmp("nythera-gio-monitor-XXXXXX", nullptr);
    assert(raw);
    const std::filesystem::path directory(raw);
    g_free(raw);
    auto* file = g_file_new_for_path(directory.c_str());
    g_main_context_push_thread_default(context);
    auto* monitor = g_file_monitor_directory(file, G_FILE_MONITOR_NONE, nullptr, nullptr);
    g_main_context_pop_thread_default(context);
    assert(monitor);
    bool changed = false;
    g_signal_connect(
        monitor, "changed", G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer data) {
          *static_cast<bool*>(data) = true;
        }),
        &changed
    );
    std::ofstream(directory / "fixture") << "local";
    const auto monitorDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!changed && std::chrono::steady_clock::now() < monitorDeadline)
      pass(100);
    assert(changed);
    g_file_monitor_cancel(monitor);
    g_object_unref(monitor);
    g_object_unref(file);
    for (unsigned i = 0; i < 4; ++i)
      pass(0);
    assert(pass(0) != 0);
    std::filesystem::remove_all(directory);
  }
  g_main_context_unref(context);
  return 0;
}
