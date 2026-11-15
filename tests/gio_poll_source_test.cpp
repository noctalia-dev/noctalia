#include "filemanager/gio_poll_source.h"

#include <cassert>
#include <poll.h>
#include <vector>

namespace {
  gboolean markDispatched(gpointer data) {
    *static_cast<bool*>(data) = true;
    return G_SOURCE_REMOVE;
  }
}

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
  }
  g_main_context_unref(context);
  return 0;
}
