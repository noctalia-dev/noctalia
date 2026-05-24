#include "shell/settings/widget_add_popup.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace settings {
  namespace {

    std::string laneLabel(std::string_view lane) {
      if (lane == "start") {
        return i18n::tr("settings.entities.widget.lanes.start");
      }
      if (lane == "center") {
        return i18n::tr("settings.entities.widget.lanes.center");
      }
      if (lane == "end") {
        return i18n::tr("settings.entities.widget.lanes.end");
      }
      return std::string(lane);
    }

    void sortSearchOptions(std::vector<SearchPickerOption>& options) {
      std::sort(options.begin(), options.end(), [](const SearchPickerOption& a, const SearchPickerOption& b) {
        const std::string aLabel = StringUtils::toLower(a.label);
        const std::string bLabel = StringUtils::toLower(b.label);
        if (aLabel == bLabel) {
          return a.value < b.value;
        }
        return aLabel < bLabel;
      });
    }

    void collectWidgetReferenceNames(const std::vector<std::string>& widgets, std::unordered_set<std::string>& seen) {
      for (const auto& widget : widgets) {
        seen.insert(widget);
      }
    }

    bool widgetReferenceNameExists(const Config& cfg, std::string_view name) {
      const std::string key(name);
      if (isBuiltInWidgetType(name) || cfg.widgets.contains(key)) {
        return true;
      }

      std::unordered_set<std::string> seen;
      for (const auto& bar : cfg.bars) {
        collectWidgetReferenceNames(bar.startWidgets, seen);
        collectWidgetReferenceNames(bar.centerWidgets, seen);
        collectWidgetReferenceNames(bar.endWidgets, seen);
        for (const auto& ovr : bar.monitorOverrides) {
          if (ovr.startWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.startWidgets, seen);
          }
          if (ovr.centerWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.centerWidgets, seen);
          }
          if (ovr.endWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.endWidgets, seen);
          }
        }
      }
      return seen.contains(key);
    }

    bool isValidWidgetInstanceId(std::string_view id) {
      if (id.empty()) {
        return false;
      }
      for (char c : id) {
        const auto uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
          return false;
        }
      }
      return true;
    }

    std::string normalizedWidgetInstanceBase(std::string_view type) {
      std::string out;
      out.reserve(type.size());
      bool lastUnderscore = false;
      for (char c : type) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
          out.push_back(static_cast<char>(std::tolower(uc)));
          lastUnderscore = false;
        } else if (!lastUnderscore && !out.empty()) {
          out.push_back('_');
          lastUnderscore = true;
        }
      }
      while (!out.empty() && out.back() == '_') {
        out.pop_back();
      }
      return out.empty() ? std::string("widget") : out;
    }

    std::string nextWidgetInstanceId(const Config& cfg, std::string_view type) {
      const std::string base = normalizedWidgetInstanceBase(type);
      for (std::size_t index = 2; index < 10000; ++index) {
        const std::string candidate = base + "_" + std::to_string(index);
        if (!widgetReferenceNameExists(cfg, candidate)) {
          return candidate;
        }
      }
      return base + "_custom";
    }

    PopupSurfaceConfig centeredPopupConfig(
        std::uint32_t parentWidth, std::uint32_t parentHeight, std::uint32_t width, std::uint32_t height,
        std::uint32_t serial
    ) {
      return PopupSurfaceConfig{
          .anchorX = static_cast<std::int32_t>(parentWidth / 2),
          .anchorY = static_cast<std::int32_t>(parentHeight / 2),
          .anchorWidth = 1,
          .anchorHeight = 1,
          .width = std::max<std::uint32_t>(1, width),
          .height = std::max<std::uint32_t>(1, height),
          .anchor = XDG_POSITIONER_ANCHOR_NONE,
          .gravity = XDG_POSITIONER_GRAVITY_NONE,
          .constraintAdjustment =
              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y,
          .offsetX = 0,
          .offsetY = 0,
          .serial = serial,
          .grab = true,
      };
    }

  } // namespace

  WidgetAddPopup::~WidgetAddPopup() { destroyPopup(); }

  void WidgetAddPopup::initialize(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext) {
    initializeBase(wayland, config, renderContext);
  }

  void WidgetAddPopup::setOnSelect(SelectCallback callback) { m_onSelect = std::move(callback); }

  void WidgetAddPopup::setOnDismissed(std::function<void()> callback) { m_onDismissed = std::move(callback); }

  void WidgetAddPopup::open(
      xdg_surface* parentXdgSurface, wl_output* output, std::uint32_t serial, wl_surface* parentWlSurface,
      std::uint32_t parentWidth, std::uint32_t parentHeight, const std::vector<std::string>& lanePath,
      const Config& config, float scale
  ) {
    if (parentXdgSurface == nullptr || parentWlSurface == nullptr) {
      return;
    }

    const auto pickerEntries = widgetPickerEntries(config);
    std::vector<SearchPickerOption> normalOptions;
    std::vector<SearchPickerOption> instanceOptions;
    std::unordered_map<std::string, std::string> presetScripts;
    normalOptions.reserve(pickerEntries.size());
    instanceOptions.reserve(pickerEntries.size());

    for (const auto& entry : pickerEntries) {
      normalOptions.push_back(
          SearchPickerOption{
              .value = entry.value,
              .label = entry.label,
              .description = entry.description,
              .enabled = true,
              .icon = entry.icon,
          }
      );

      if (entry.kind == WidgetReferenceKind::Preset && !entry.script.empty()) {
        presetScripts[entry.value] = entry.script;
      }

      if (entry.kind != WidgetReferenceKind::BuiltIn) {
        continue;
      }
      for (const auto& spec : widgetTypeSpecs()) {
        if (spec.type != entry.value || !spec.supportsMultipleInstances) {
          continue;
        }
        instanceOptions.push_back(
            SearchPickerOption{
                .value = entry.value,
                .label = entry.label,
                .description = i18n::tr("settings.entities.widget.picker.instance-description", "type", entry.value),
                .enabled = true,
                .icon = entry.icon,
            }
        );
        break;
      }
    }

    if (normalOptions.empty()) {
      return;
    }

    sortSearchOptions(normalOptions);
    sortSearchOptions(instanceOptions);

    if (isOpen()) {
      close();
    }

    m_scale = std::max(0.1f, scale);
    m_config = &config;
    m_normalOptions = std::move(normalOptions);
    m_instanceOptions = std::move(instanceOptions);
    m_presetScripts = std::move(presetScripts);
    m_lanePath = lanePath;
    m_root = nullptr;
    m_createActions = nullptr;
    m_searchPicker = nullptr;
    m_instanceDescription = nullptr;
    m_instanceInput = nullptr;
    m_instanceModeEnabled = false;
    m_createFormVisible = false;
    m_createType.clear();
    m_createLabel.clear();

    m_parentXdgSurface = parentXdgSurface;
    m_parentWlSurface = parentWlSurface;
    m_output = output;
    m_serial = serial;
    m_parentWidth = parentWidth;
    m_parentHeight = parentHeight;

    reopenForCurrentMode();
  }

  void WidgetAddPopup::close() { destroyPopup(); }

  bool WidgetAddPopup::isOpen() const noexcept { return DialogPopupHost::isOpen(); }

  bool WidgetAddPopup::onPointerEvent(const PointerEvent& event) { return DialogPopupHost::onPointerEvent(event); }

  void WidgetAddPopup::onKeyboardEvent(const KeyboardEvent& event) { DialogPopupHost::onKeyboardEvent(event); }

  wl_surface* WidgetAddPopup::wlSurface() const noexcept { return DialogPopupHost::wlSurface(); }

  void WidgetAddPopup::requestLayout() { DialogPopupHost::requestLayout(); }

  void WidgetAddPopup::requestRedraw() { DialogPopupHost::requestRedraw(); }

  void WidgetAddPopup::refreshPickerOptions() {
    if (m_searchPicker == nullptr) {
      return;
    }
    m_searchPicker->setOptions(m_instanceModeEnabled ? m_instanceOptions : m_normalOptions);
  }

  void WidgetAddPopup::refreshBodyState() {
    if (m_searchPicker != nullptr) {
      m_searchPicker->setVisible(!m_createFormVisible);
      m_searchPicker->setParticipatesInLayout(!m_createFormVisible);
      if (!m_createFormVisible) {
        refreshPickerOptions();
      }
    }
    if (m_instanceDescription != nullptr) {
      m_instanceDescription->setVisible(m_createFormVisible);
      m_instanceDescription->setParticipatesInLayout(m_createFormVisible);
    }
    if (m_instanceInput != nullptr) {
      m_instanceInput->setVisible(m_createFormVisible);
      m_instanceInput->setParticipatesInLayout(m_createFormVisible);
    }
    if (m_createActions != nullptr) {
      m_createActions->setVisible(m_createFormVisible);
      m_createActions->setParticipatesInLayout(m_createFormVisible);
    }

    if (!m_createFormVisible && m_searchPicker != nullptr) {
      if (auto* filter = m_searchPicker->filterInputArea(); filter != nullptr) {
        inputDispatcher().setFocus(filter);
      }
    }
    if (m_createFormVisible && m_instanceInput != nullptr && m_instanceInput->inputArea() != nullptr) {
      inputDispatcher().setFocus(m_instanceInput->inputArea());
    }
  }

  std::string WidgetAddPopup::suggestedInstanceId(std::string_view type) const {
    if (m_config == nullptr) {
      return std::string(type);
    }
    return nextWidgetInstanceId(*m_config, type);
  }

  bool WidgetAddPopup::canCreateInstanceId(std::string_view id) const {
    if (m_config == nullptr) {
      return false;
    }
    return isValidWidgetInstanceId(id) && !widgetReferenceNameExists(*m_config, id);
  }

  void WidgetAddPopup::beginCreateFlow(const SearchPickerOption& option) {
    m_createType = option.value;
    m_createLabel = option.label;
    m_createFormVisible = true;
    if (m_instanceInput != nullptr) {
      m_instanceInput->setValue(suggestedInstanceId(m_createType));
      m_instanceInput->setInvalid(false);
    }
    reopenForCurrentMode();
  }

  void WidgetAddPopup::finishCreateFlow() {
    if (m_instanceInput == nullptr) {
      return;
    }
    const std::string id = StringUtils::trim(m_instanceInput->value());
    if (!canCreateInstanceId(id)) {
      m_instanceInput->setInvalid(true);
      return;
    }
    m_instanceInput->setInvalid(false);
    if (m_onSelect) {
      m_onSelect(m_lanePath, id, m_createType, id, {});
    }
    DeferredCall::callLater([this]() { close(); });
  }

  void WidgetAddPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
    const float panelPadding = Style::spaceSm * m_scale;
    const float panelGap = Style::spaceSm * m_scale;
    const std::string lane = laneLabel(m_lanePath.empty() ? "" : m_lanePath.back());
    const std::string title = m_createFormVisible
                                  ? instanceFormTitle()
                                  : i18n::tr("settings.entities.widget.inspector.add-title", "lane", lane);

    auto root = ui::column({
        .out = &m_root,
        .align = FlexAlign::Stretch,
        .gap = panelGap,
        .padding = panelPadding,
    });

    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * m_scale,
    });

    auto titleLabel = ui::label({
        .text = title,
        .fontSize = Style::fontSizeBody * m_scale,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .fontWeight = FontWeight::Bold,
    });
    if (m_createFormVisible) {
      titleLabel->setMaxLines(2);
    }
    header->addChild(std::move(titleLabel));
    header->addChild(ui::spacer());

    if (!m_createFormVisible) {
      header->addChild(
          ui::label({
              .text = i18n::tr("settings.entities.widget.picker.instance-toggle"),
              .fontSize = Style::fontSizeCaption * m_scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );

      header->addChild(
          ui::toggle({
              .checked = m_instanceModeEnabled,
              .scale = m_scale,
              .onChange = [this](bool value) {
                m_instanceModeEnabled = value;
                m_createFormVisible = false;
                m_createType.clear();
                m_createLabel.clear();
                if (m_instanceInput != nullptr) {
                  m_instanceInput->setInvalid(false);
                }
                refreshBodyState();
                requestLayout();
              },
          })
      );
    }

    header->addChild(
        ui::button({
            .glyph = "close",
            .glyphSize = Style::fontSizeBody * m_scale,
            .variant = ButtonVariant::Default,
            .minWidth = Style::controlHeightSm * m_scale,
            .minHeight = Style::controlHeightSm * m_scale,
            .padding = Style::spaceXs * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [this]() { DeferredCall::callLater([this]() { close(); }); },
        })
    );
    root->addChild(std::move(header));

    root->addChild(
        ui::searchPicker({
            .out = &m_searchPicker,
            .placeholder = i18n::tr("settings.entities.widget.picker.placeholder"),
            .emptyText = i18n::tr("settings.entities.widget.picker.empty"),
            .options = m_normalOptions,
            .flexGrow = 1.0f,
            .onActivated =
                [this](const SearchPickerOption& option) {
                  if (option.value.empty()) {
                    return;
                  }
                  // Bundled scripted widget (manifest preset): one-click add, no naming form.
                  if (const auto it = m_presetScripts.find(option.value); it != m_presetScripts.end()) {
                    const std::string instanceId =
                        m_config != nullptr && !widgetReferenceNameExists(*m_config, option.value)
                            ? option.value
                            : suggestedInstanceId(option.value);
                    if (m_onSelect) {
                      m_onSelect(m_lanePath, option.value, "scripted", instanceId, {{"script", it->second}});
                    }
                    DeferredCall::callLater([this]() { close(); });
                    return;
                  }
                  if (m_instanceModeEnabled || widgetTypeRequiresNamedConfig(option.value)) {
                    beginCreateFlow(option);
                    return;
                  }
                  if (m_onSelect) {
                    m_onSelect(m_lanePath, option.value, {}, {}, {});
                  }
                  DeferredCall::callLater([this]() { close(); });
                },
            .onCancel = [this]() { DeferredCall::callLater([this]() { close(); }); },
            .configure =
                [](SearchPicker& picker) {
                  picker.clearFill();
                  picker.clearBorder();
                  picker.setRadius(0.0f);
                  picker.setPadding(0.0f);
                },
        })
    );

    root->addChild(
        ui::label({
            .out = &m_instanceDescription,
            .text = i18n::tr("settings.entities.widget.instance.id-description"),
            .fontSize = Style::fontSizeCaption * m_scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
            .visible = false,
            .participatesInLayout = false,
        })
    );

    root->addChild(
        ui::input({
            .out = &m_instanceInput,
            .placeholder = i18n::tr("settings.entities.widget.instance.id-placeholder"),
            .fontSize = Style::fontSizeBody * m_scale,
            .controlHeight = Style::controlHeight * m_scale,
            .horizontalPadding = Style::spaceSm * m_scale,
            .visible = false,
            .participatesInLayout = false,
            .onChange =
                [this](const std::string& /*value*/) {
                  if (m_instanceInput != nullptr) {
                    m_instanceInput->setInvalid(false);
                  }
                },
            .onSubmit = [this](const std::string& /*value*/) { finishCreateFlow(); },
        })
    );

    root->addChild(
        ui::row(
            {
                .out = &m_createActions,
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * m_scale,
                .visible = false,
                .participatesInLayout = false,
            },
            ui::button({
                .text = i18n::tr("common.actions.cancel"),
                .fontSize = Style::fontSizeCaption * m_scale,
                .variant = ButtonVariant::Ghost,
                .minHeight = Style::controlHeightSm * m_scale,
                .paddingV = Style::spaceXs * m_scale,
                .paddingH = Style::spaceSm * m_scale,
                .radius = Style::scaledRadiusSm(m_scale),
                .onClick =
                    [this]() {
                      m_createFormVisible = false;
                      m_createType.clear();
                      m_createLabel.clear();
                      if (m_instanceInput != nullptr) {
                        m_instanceInput->setInvalid(false);
                      }
                      reopenForCurrentMode();
                    },
            }),
            ui::button({
                .text = i18n::tr("settings.entities.widget.instance.create-save"),
                .fontSize = Style::fontSizeCaption * m_scale,
                .variant = ButtonVariant::Default,
                .minHeight = Style::controlHeightSm * m_scale,
                .paddingV = Style::spaceXs * m_scale,
                .paddingH = Style::spaceSm * m_scale,
                .radius = Style::scaledRadiusSm(m_scale),
                .onClick = [this]() { finishCreateFlow(); },
            })
        )
    );

    contentParent->addChild(std::move(root));

    refreshBodyState();
  }

  void WidgetAddPopup::layoutSheet(float contentWidth, float contentHeight) {
    if (m_root == nullptr || renderContext() == nullptr) {
      return;
    }

    m_root->setSize(contentWidth, contentHeight);
    m_root->layout(*renderContext());
  }

  std::string WidgetAddPopup::instanceFormTitle() const {
    const std::string lane = laneLabel(m_lanePath.empty() ? "" : m_lanePath.back());
    return i18n::tr("settings.entities.widget.inspector.add-instance-title", "widget", m_createLabel, "lane", lane);
  }

  std::pair<float, float> WidgetAddPopup::popupSize() const {
    constexpr float kPickerWidth = 520.0f;
    constexpr float kPickerHeight = 420.0f;
    constexpr float kCreateMinWidth = 360.0f;
    constexpr float kCreateHeight = 190.0f;
    constexpr float kCreateMaxWidth = 640.0f;
    constexpr float kParentMargin = 48.0f;

    if (!m_createFormVisible) {
      return {kPickerWidth * m_scale, kPickerHeight * m_scale};
    }

    float contentWidth = kCreateMinWidth * m_scale;
    if (m_renderContext != nullptr && !m_createLabel.empty()) {
      const float fontSize = Style::fontSizeBody * m_scale;
      const TextMetrics titleMetrics = m_renderContext->measureText(instanceFormTitle(), fontSize, FontWeight::Bold);
      const float closeBtn = Style::controlHeightSm * m_scale;
      const float headerGap = Style::spaceSm * m_scale;
      const float rootPadding = Style::spaceSm * m_scale * 2.0f;
      const float sheetPadding = computePadding(m_scale) * 2.0f;

      const float measured = titleMetrics.width + headerGap + closeBtn + rootPadding + sheetPadding;

      float maxWidth = kCreateMaxWidth * m_scale;
      if (m_parentWidth > 0) {
        maxWidth = std::min(
            maxWidth,
            std::max(kCreateMinWidth * m_scale, static_cast<float>(m_parentWidth) * m_scale - kParentMargin * m_scale)
        );
      }
      contentWidth = std::clamp(measured, kCreateMinWidth * m_scale, maxWidth);
    }

    return {contentWidth, kCreateHeight * m_scale};
  }

  void WidgetAddPopup::reopenForCurrentMode() {
    if (m_parentXdgSurface == nullptr || m_parentWlSurface == nullptr) {
      return;
    }

    const auto [panelWidth, panelHeight] = popupSize();
    const auto cfg = centeredPopupConfig(
        m_parentWidth, m_parentHeight, static_cast<std::uint32_t>(std::max(1.0f, panelWidth)),
        static_cast<std::uint32_t>(std::max(1.0f, panelHeight)), m_serial
    );

    m_internalReopen = true;
    const bool opened = openPopupAsChild(cfg, m_parentXdgSurface, m_parentWlSurface, m_output);
    m_internalReopen = false;
    if (!opened) {
      close();
    }
  }

  void WidgetAddPopup::cancelToFacade() {}

  InputArea* WidgetAddPopup::initialFocusArea() {
    if (m_createFormVisible && m_instanceInput != nullptr) {
      return m_instanceInput->inputArea();
    }
    return m_searchPicker != nullptr ? m_searchPicker->filterInputArea() : nullptr;
  }

  void WidgetAddPopup::onSheetClose() {
    if (m_internalReopen) {
      return;
    }
    m_normalOptions.clear();
    m_instanceOptions.clear();
    m_presetScripts.clear();
    m_config = nullptr;
    m_parentXdgSurface = nullptr;
    m_parentWlSurface = nullptr;
    m_output = nullptr;
    m_serial = 0;
    m_parentWidth = 0;
    m_parentHeight = 0;
    m_lanePath.clear();
    m_root = nullptr;
    m_createActions = nullptr;
    m_searchPicker = nullptr;
    m_instanceDescription = nullptr;
    m_instanceInput = nullptr;
    m_instanceModeEnabled = false;
    m_createFormVisible = false;
    m_createType.clear();
    m_createLabel.clear();
    if (m_onDismissed) {
      m_onDismissed();
    }
  }

} // namespace settings
