#pragma once

namespace scripting {

  // A `ui.*` callback prop may be given a function instead of the name of a
  // plugin global. The tree crosses to the UI thread by value and cannot carry
  // a VM value, so the function is registered under a generated name and only
  // that name travels; the closure stays in the plugin thread.
  //
  // Handlers live in their own table rather than in globals: every render()
  // registers into a fresh table that replaces the live one wholesale, so the
  // handlers of a node that disappeared are dropped with the render that
  // produced them, and a click on a name the current tree no longer defines
  // resolves to nothing instead of a stale closure.
  //
  // Both names open with a character no Luau identifier can contain, so plugin
  // code can neither collide with a generated name nor shadow the table.
  inline constexpr const char* kUiHandlerTable = "@ui_handlers";
  inline constexpr const char* kUiHandlerPrefix = "@ui:";

} // namespace scripting
