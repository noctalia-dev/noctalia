#include "config/config_types.h"
#include "test_check.h"

#include <print>
#include <set>
#include <string>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "capsule_group_reconcile_test: FAIL: {}", message);
      return false;
    }
    return true;
  }

  BarCapsuleGroupStyle group(std::string id, std::vector<std::string> members, float padding = 6.0F) {
    BarCapsuleGroupStyle style;
    style.id = std::move(id);
    style.members = std::move(members);
    style.padding = padding;
    return style;
  }

  std::vector<std::string> ids(const std::vector<BarCapsuleGroupStyle>& groups) {
    std::vector<std::string> out;
    out.reserve(groups.size());
    for (const auto& g : groups) {
      out.push_back(g.id);
    }
    return out;
  }

} // namespace

int main() {
  bool ok = true;

  const std::vector<BarCapsuleGroupStyle> base{
      group("left", {"launcher", "wallpaper", "recorder"}),
      group("monit", {"cpu", "weather", "brightness"}),
      group("right", {"network", "battery"}),
  };

  // Ungrouping "left" drops it from the override array while its lane is exploded.
  {
    const std::vector<BarCapsuleGroupStyle> current{base[1], base[2]};
    const std::set<std::string> referenced{"monit", "right"};
    ok = expect(reconcileCapsuleGroups(current, base, referenced) == current, "unreferenced file group stays dropped")
        && ok;
  }

  // Resetting that lane brings the file's token back, so its group must come back too.
  {
    const std::vector<BarCapsuleGroupStyle> current{base[1], base[2]};
    const std::set<std::string> referenced{"left", "monit", "right"};
    ok = expect(
             reconcileCapsuleGroups(current, base, referenced) == base,
             "referenced file group returns in file order, matching the file array"
         )
        && ok;
  }

  // A style edit on another group survives the repair.
  {
    const std::vector<BarCapsuleGroupStyle> current{group("monit", {"cpu", "weather", "brightness"}, 12.0F), base[2]};
    const std::set<std::string> referenced{"left", "monit", "right"};
    const auto reconciled = reconcileCapsuleGroups(current, base, referenced);
    ok =
        expect(ids(reconciled) == std::vector<std::string>{"left", "monit", "right"}, "repaired array keeps file order")
        && ok;
    ok = expect(reconciled[1].padding == 12.0F, "overridden group keeps its edited style") && ok;
  }

  // GUI-created groups have no file entry: they are appended while referenced, dropped once not.
  {
    const std::vector<BarCapsuleGroupStyle> current{base[1], base[2], group("g1", {"clock", "notifications"})};
    ok = expect(
             ids(reconcileCapsuleGroups(current, base, {"left", "monit", "right", "g1"}))
                 == std::vector<std::string>{"left", "monit", "right", "g1"},
             "referenced gui-created group is appended"
         )
        && ok;
    ok = expect(
             ids(reconcileCapsuleGroups(current, base, {"left", "monit", "right"}))
                 == std::vector<std::string>{"left", "monit", "right"},
             "unreferenced gui-created group is dropped"
         )
        && ok;
  }

  // Duplicate ids in the config file pair up one-to-one with the override entries.
  {
    const std::vector<BarCapsuleGroupStyle> dupBase{group("dup", {"a"}), group("dup", {"b"})};
    const std::vector<BarCapsuleGroupStyle> current{group("dup", {"a"}, 20.0F)};
    const auto reconciled = reconcileCapsuleGroups(current, dupBase, {"dup"});
    ok = expect(reconciled.size() == 2, "both duplicate-id file groups are present") && ok;
    ok = expect(
             reconciled.size() == 2 && reconciled[0].padding == 20.0F && reconciled[1].members == dupBase[1].members,
             "the override entry consumes one duplicate, the file supplies the other"
         )
        && ok;
  }

  // Lane reference collection walks every lane, and monitor lanes that read the bar's array.
  {
    BarConfig bar;
    bar.startWidgets = {makeCapsuleGroupToken("left"), "clock"};
    bar.centerWidgets = {"workspaces"};
    bar.endWidgets = {makeCapsuleGroupToken("right")};

    BarMonitorOverride inherits;
    inherits.match = "DP-1";
    inherits.centerWidgets = std::vector<std::string>{makeCapsuleGroupToken("monit")};
    BarMonitorOverride owns;
    owns.match = "DP-2";
    owns.centerWidgets = std::vector<std::string>{makeCapsuleGroupToken("own")};
    owns.widgetCapsuleGroups = std::vector<BarCapsuleGroupStyle>{group("own", {"cpu"})};
    bar.monitorOverrides = {inherits, owns};

    ok = expect(
             capsuleGroupRefsForBarScope(bar) == std::set<std::string>{"left", "right", "monit"},
             "bar scope collects its own lanes plus the lanes of monitors without their own array"
         )
        && ok;
    ok = expect(
             capsuleGroupRefsForMonitorScope(bar, owns) == std::set<std::string>{"left", "own", "right"},
             "monitor scope collects its own lanes, falling back to the bar's lane where unset"
         )
        && ok;
  }

  // Nested groups (a member entry that is itself a group token) are reachable through their
  // parent, so scopes keep them and reconcile never drops them while referenced.
  {
    BarCapsuleGroupStyle outer;
    outer.id = "outer";
    outer.members = {"clock", makeCapsuleGroupToken("inner")};
    BarCapsuleGroupStyle inner;
    inner.id = "inner";
    inner.members = {"network", "volume"};
    BarConfig bar;
    bar.startWidgets = {makeCapsuleGroupToken("outer")};
    bar.widgetCapsuleGroups = {outer, inner};

    ok = expect(capsuleGroupHasNestedRef(outer), "outer reports a nested ref") && ok;
    ok = expect(!capsuleGroupHasNestedRef(inner), "inner reports no nested ref") && ok;
    ok = expect(nestedCapsuleGroupIds(outer) == std::vector<std::string>{"inner"}, "nested id listed") && ok;
    ok = expect(
             capsuleGroupRefsForBarScope(bar) == std::set<std::string>{"inner", "outer"},
             "bar scope includes transitively nested groups"
         )
        && ok;

    // Reconcile keeps the nested-only group while the outer is referenced (GUI-created groups
    // have no file entry, so `base` is empty and survival depends entirely on `referenced`)...
    const std::vector<BarCapsuleGroupStyle> current{outer, inner};
    const std::set<std::string> nestedRefs = capsuleGroupRefsForBarScope(bar);
    ok = expect(
             ids(reconcileCapsuleGroups(current, {}, nestedRefs)) == std::vector<std::string>{"outer", "inner"},
             "referenced nested group survives reconcile"
         )
        && ok;
    // ...and drops it once nothing reaches it anymore.
    ok =
        expect(
            reconcileCapsuleGroups(current, {}, std::set<std::string>{}).empty(), "unreferenced nested group is dropped"
        )
        && ok;
  }

  // Resetting a lane must restore the whole file-defined subtree, even when its
  // parent is absent from the current override array and the child precedes it.
  {
    const auto outer = group("outer", {"clock", "group:inner"});
    const auto inner = group("inner", {"network", "volume"});
    const std::vector<BarCapsuleGroupStyle> nestedBase{inner, outer};
    TEST_CHECK(reconcileCapsuleGroups({}, nestedBase, {"outer"}) == nestedBase);

    // A retained override owns membership; do not revive children removed there.
    const auto editedOuter = group("outer", {"clock"});
    TEST_CHECK(
        reconcileCapsuleGroups({editedOuter}, nestedBase, {"outer"}) == std::vector<BarCapsuleGroupStyle>{editedOuter}
    );

    // A restored parent may also reference a GUI-created child with edited style.
    const auto editedInner = group("inner", {"network", "volume"}, 12.0F);
    TEST_CHECK(
        reconcileCapsuleGroups({editedInner}, {outer}, {"outer"})
        == (std::vector<BarCapsuleGroupStyle>{outer, editedInner})
    );
  }

  return ok ? 0 : 1;
}
