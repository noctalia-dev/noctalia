#include "scripting/luau_host.h"
#include "scripting/script_api_context.h"
#include "tests/test_check.h"

int main() {
  scripting::ScriptApiContext api;

  {
    LuauHost host(api, "test/idle-inhibitors:widget");
    TEST_CHECK(host.exec("=empty", R"(
      local inhibitors = noctalia.idle.inhibitors()
      assert(type(inhibitors) == 'table')
      assert(#inhibitors == 0)
      assert(noctalia.idle.isInhibited() == false)
    )"));
  }

  api.setIdleInhibitors({
      {"Google Chrome", "Playing audio", "screensaver"},
      {"Caffeine", "", "caffeine"},
  });

  {
    LuauHost host(api, "test/idle-inhibitors:widget");
    TEST_CHECK(host.exec("=populated", R"(
      local inhibitors = noctalia.idle.inhibitors()
      assert(#inhibitors == 2)
      assert(inhibitors[1].app == 'Google Chrome')
      assert(inhibitors[1].reason == 'Playing audio')
      assert(inhibitors[1].type == 'screensaver')
      assert(inhibitors[2].app == 'Caffeine')
      assert(inhibitors[2].reason == '')
      assert(inhibitors[2].type == 'caffeine')
      assert(noctalia.idle.isInhibited() == true)
    )"));
  }

  // Clearing the snapshot is visible to already-created hosts.
  LuauHost liveHost(api, "test/idle-inhibitors:service");
  TEST_CHECK(liveHost.exec("=before-clear", "assert(noctalia.idle.isInhibited() == true)\n"));
  api.setIdleInhibitors({});
  TEST_CHECK(liveHost.exec("=after-clear", R"(
    assert(#noctalia.idle.inhibitors() == 0)
    assert(noctalia.idle.isInhibited() == false)
  )"));

  return 0;
}
