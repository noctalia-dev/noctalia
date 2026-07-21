#include "ui/controls/select_dropdown_popup.h"

#include "core/log.h"
#include "ui/controls/context_menu.h"
#include "ui/popup_parent.h"
#include "ui/style.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("select-dropdown-popup");

} // namespace

SelectDropdownPopup::SelectDropdownPopup(WaylandConnection& wayland, RenderContext& renderContext)
    : m_popup(wayland, renderContext) {}

SelectDropdownPopup::~SelectDropdownPopup() = default;

void SelectDropdownPopup::setParent(
    zwlr_layer_surface_v1* layerSurface, wl_surface* parentWlSurface, wl_output* output
) {
  m_parentLayerSurface = layerSurface;
  m_parentXdgSurface = nullptr;
  m_parentWlSurface = parentWlSurface;
  m_parentOutput = output;
}

void SelectDropdownPopup::setParent(xdg_surface* xdgSurface, wl_surface* parentWlSurface, wl_output* output) {
  m_parentLayerSurface = nullptr;
  m_parentXdgSurface = xdgSurface;
  m_parentWlSurface = parentWlSurface;
  m_parentOutput = output;
}

void SelectDropdownPopup::setShadowConfig(const ShellConfig::ShadowConfig& shadow) { m_popup.setShadowConfig(shadow); }

void SelectDropdownPopup::openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) {
  // Close first so a previously open dropdown delivers its dismiss to its own owner before the
  // callbacks are rebound to the new one.
  m_popup.close();

  if (m_parentLayerSurface == nullptr && m_parentXdgSurface == nullptr) {
    kLog.warn("no parent surface set");
    if (callbacks.onDismiss) {
      callbacks.onDismiss();
    }
    return;
  }

  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(request.options.size());
  for (std::size_t i = 0; i < request.options.size(); ++i) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = static_cast<std::int32_t>(i),
            .label = request.options[i],
            .checkmark = true,
            .toggleState = i == request.selectedIndex ? 1 : 0,
            .indicatorColor = i < request.indicatorColors.size() ? std::optional<ColorSpec>(request.indicatorColors[i])
                                                                 : std::nullopt,
            .swatchPreview =
                i < request.optionSwatchPreviews.size() ? request.optionSwatchPreviews[i] : ColorSwatchPreview{},
        }
    );
  }

  m_popup.setOnActivate([onSelect = std::move(callbacks.onSelect)](const ContextMenuControlEntry& entry) {
    if (onSelect) {
      onSelect(static_cast<std::size_t>(std::max<std::int32_t>(0, entry.id)));
    }
  });
  m_popup.setOnDismissed([onDismiss = std::move(callbacks.onDismiss)]() {
    if (onDismiss) {
      onDismiss();
    }
  });

  // The menu row font is kMenuFontSize == fontSizeCaption, so scaling by fontSize/fontSizeCaption
  // renders options at exactly the requested (pre-scaled) font size, with row metrics to match.
  const float contentScale = request.fontSize > 0.0f ? request.fontSize / Style::fontSizeCaption : 1.0f;

  m_popup.open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .minMenuWidth = request.menuWidth,
          .maxMenuWidth = std::max(request.menuWidth, request.maxMenuWidth),
          .contentScale = contentScale,
          .maxVisible = std::max<std::size_t>(1, request.maxVisibleOptions),
          .initialHighlight = request.selectedIndex,
          .anchor =
              PopupAnchorRect{
                  .x = request.anchorX,
                  .y = request.anchorY,
                  .width = request.anchorWidth,
                  .height = request.anchorHeight,
              },
          // parent.wlSurface stays null: select parents (panels, settings window) are already
          // keyboard OnDemand; the bar-style interactivity flip must not touch them. The parent
          // surface is passed only for pointer-capture mapping (scrollbar drags).
          .parent =
              PopupSurfaceParent{
                  .layerSurface = m_parentLayerSurface,
                  .xdgSurface = m_parentXdgSurface,
                  .output = m_parentOutput,
              },
          .pointerParentSurface = m_parentWlSurface,
      }
  );
}

void SelectDropdownPopup::closeSelectDropdown() { m_popup.close(); }

bool SelectDropdownPopup::isSelectDropdownOpen() const { return m_popup.isOpen(); }

bool SelectDropdownPopup::onPointerEvent(const PointerEvent& event) { return m_popup.onPointerEvent(event); }

void SelectDropdownPopup::onKeyboardEvent(const KeyboardEvent& event) { m_popup.onKeyboardEvent(event); }

wl_surface* SelectDropdownPopup::wlSurface() const noexcept { return m_popup.wlSurface(); }

xdg_surface* SelectDropdownPopup::xdgSurface() const noexcept { return m_popup.xdgSurface(); }

std::uint32_t SelectDropdownPopup::popupWidth() const noexcept { return m_popup.width(); }

std::uint32_t SelectDropdownPopup::popupHeight() const noexcept { return m_popup.height(); }
