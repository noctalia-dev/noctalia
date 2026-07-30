#include "dbus/tray/fcitx_status.h"

#include "dbus/tray/tray_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <utility>

namespace {

  bool isFcitxItem(const TrayItemInfo& item) {
    const std::string itemName = StringUtils::toLower(item.itemName);
    const std::string processName = StringUtils::toLower(item.processName);
    return itemName == "fcitx" || processName == "fcitx5";
  }

} // namespace

std::optional<FcitxInputMethodState> resolveFcitxInputMethodState(std::span<const TrayItemInfo> items) {
  const auto it = std::ranges::find_if(items, isFcitxItem);
  if (it == items.end() || it->statusNotifierTitle.empty()) {
    return std::nullopt;
  }

  return FcitxInputMethodState{
      .label = it->statusNotifierTitle,
  };
}

std::optional<FcitxInputMethodState> FcitxInputMethodTracker::update(std::span<const TrayItemInfo> items) {
  auto next = resolveFcitxInputMethodState(items);
  if (!next.has_value()) {
    m_current.reset();
    m_initialized = false;
    return std::nullopt;
  }

  if (!m_initialized) {
    m_current = std::move(next);
    m_initialized = true;
    return std::nullopt;
  }

  if (next == m_current) {
    return std::nullopt;
  }

  m_current = next;
  return next;
}

bool FcitxInputMethodTracker::available() const noexcept { return m_current.has_value(); }
