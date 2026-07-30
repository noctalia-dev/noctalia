#pragma once

#include <optional>
#include <span>
#include <string>

struct TrayItemInfo;

struct FcitxInputMethodState {
  std::string label;

  bool operator==(const FcitxInputMethodState&) const = default;
};

[[nodiscard]] std::optional<FcitxInputMethodState> resolveFcitxInputMethodState(std::span<const TrayItemInfo> items);

class FcitxInputMethodTracker {
public:
  [[nodiscard]] std::optional<FcitxInputMethodState> update(std::span<const TrayItemInfo> items);
  [[nodiscard]] bool available() const noexcept;

private:
  std::optional<FcitxInputMethodState> m_current;
  bool m_initialized = false;
};
