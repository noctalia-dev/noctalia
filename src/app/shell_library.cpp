#include "app/shell_library.h"

#include "app/shell_identity.h"
#include "core/ui_strings.h"
#include "i18n/i18n.h"
#include "render/codecs/codecs.h"

#include <string>
#include <string_view>

namespace {

  // Keys without a printable glyph; literal i18n keys so tools/i18n-check.py can see them.
  std::string keyNameLabel(std::string_view name) {
    if (name == "Return") {
      return i18n::tr("ui.controls.key-names.enter");
    }
    if (name == "KP_Enter") {
      return i18n::tr("ui.controls.key-names.numpad-enter");
    }
    if (name == "space") {
      return i18n::tr("ui.controls.key-names.space");
    }
    if (name == "Prior") {
      return i18n::tr("ui.controls.key-names.page-up");
    }
    if (name == "Next") {
      return i18n::tr("ui.controls.key-names.page-down");
    }
    if (name == "Escape") {
      return i18n::tr("ui.controls.key-names.escape");
    }
    if (name == "Tab" || name == "ISO_Left_Tab") {
      return i18n::tr("ui.controls.key-names.tab");
    }
    if (name == "BackSpace") {
      return i18n::tr("ui.controls.key-names.backspace");
    }
    if (name == "Delete") {
      return i18n::tr("ui.controls.key-names.delete");
    }
    if (name == "Insert") {
      return i18n::tr("ui.controls.key-names.insert");
    }
    if (name == "Home") {
      return i18n::tr("ui.controls.key-names.home");
    }
    if (name == "End") {
      return i18n::tr("ui.controls.key-names.end");
    }
    if (name == "Left") {
      return i18n::tr("ui.controls.key-names.left");
    }
    if (name == "Right") {
      return i18n::tr("ui.controls.key-names.right");
    }
    if (name == "Up") {
      return i18n::tr("ui.controls.key-names.up");
    }
    if (name == "Down") {
      return i18n::tr("ui.controls.key-names.down");
    }
    return {};
  }

} // namespace

void configureSharedLibrary() {
  setApplicationIdentity(shellApplicationIdentity());
  setUiStrings({
      .keyName = keyNameLabel,
      .selectPlaceholder = [] { return i18n::tr("ui.controls.select.placeholder"); },
  });
  codecs::installImageDecoder();
}
