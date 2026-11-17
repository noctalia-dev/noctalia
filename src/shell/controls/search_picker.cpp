#include "shell/controls/search_picker.h"

#include "core/input/keybind_matcher.h"
#include "i18n/i18n.h"
#include "launcher/fuzzy_match.h"
#include "ui/builders.h"
#include "ui/controls/color_swatch_preview.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

  constexpr float kDefaultWidth = 320.0F;
  constexpr float kDefaultHeight = 360.0F;
  constexpr auto kFilterDebounceInterval = std::chrono::milliseconds(120);

  constexpr float kChildIndent = Style::spaceLg;

  class SearchPickerRow : public Flex {
  public:
    SearchPickerRow() {
      setDirection(FlexDirection::Horizontal);
      setAlign(FlexAlign::Center);
      setGap(Style::spaceSm);
      setPadding(Style::spaceXs, Style::spaceSm);
      setRadius(Style::scaledRadiusMd());
      setFillWidth(true);

      auto preview = std::make_unique<ColorSwatchPreviewStrip>();
      preview->setVisible(false);
      preview->setParticipatesInLayout(false);
      m_preview = static_cast<ColorSwatchPreviewStrip*>(addChild(std::move(preview)));

      addChild(
          ui::glyph({
              .out = &m_icon,
              .glyphSize = Style::baseGlyphSize,
              .visible = false,
              .participatesInLayout = false,
          })
      );

      auto text = ui::column({
          .out = &m_text,
          .align = FlexAlign::Stretch,
          .justify = FlexJustify::Center,
          .flexGrow = 1.0F,
      });
      text->addChild(
          ui::label({
              .out = &m_title,
              .fontSize = Style::fontSizeBody,
          })
      );
      text->addChild(
          ui::label({
              .out = &m_detail,
              .fontSize = Style::fontSizeCaption,
              .maxLines = 1,
              .visible = false,
          })
      );
      addChild(std::move(text));

      addChild(
          ui::label({
              .out = &m_count,
              .fontSize = Style::fontSizeCaption,
              .visible = false,
              .participatesInLayout = false,
          })
      );
      addChild(
          ui::glyph({
              .out = &m_chevron,
              .glyphSize = Style::baseGlyphSize,
              .visible = false,
              .participatesInLayout = false,
          })
      );
    }

    void bind(
        const SearchPickerOption& option, std::string_view label, bool child, bool highlighted, bool selected,
        bool hovered
    ) {
      const bool hasDetail = !option.description.empty();
      const bool hasIcon = !option.icon.empty();
      const bool hasPreview = !option.preview.empty();
      const Colors colors = applyState(option.enabled, highlighted, selected, hovered);

      setPadding(Style::spaceXs, Style::spaceSm, Style::spaceXs, Style::spaceSm + (child ? kChildIndent : 0.0F));
      bindPreview(option.preview, hasPreview);
      if (m_icon != nullptr) {
        if (hasIcon && !hasPreview) {
          m_icon->setGlyph(option.icon);
          m_icon->setColor(colors.foreground);
        }
        setShown(m_icon, hasIcon && !hasPreview);
      }
      if (m_title != nullptr) {
        m_title->setText(label);
        m_title->setFontWeight(hasDetail ? FontWeight::Bold : FontWeight::Normal);
        m_title->setColor(colors.foreground);
      }
      if (m_detail != nullptr) {
        setShown(m_detail, hasDetail);
        m_detail->setText(option.description);
        m_detail->setColor(colors.detail);
      }
      setShown(m_count, false);
      setShown(m_chevron, false);
    }

    void bindGroup(
        std::string_view name, std::size_t count, const ColorSwatchPreview& preview, bool expanded, bool highlighted,
        bool selected, bool hovered
    ) {
      const Colors colors = applyState(true, highlighted, selected, hovered);

      setPadding(Style::spaceXs, Style::spaceSm);
      bindPreview(preview, !preview.empty());
      setShown(m_icon, false);
      if (m_title != nullptr) {
        m_title->setText(name);
        m_title->setFontWeight(FontWeight::Bold);
        m_title->setColor(colors.foreground);
      }
      setShown(m_detail, false);
      if (m_count != nullptr) {
        m_count->setText(std::to_string(count));
        m_count->setColor(colors.detail);
        setShown(m_count, true);
      }
      if (m_chevron != nullptr) {
        m_chevron->setGlyph(expanded ? "chevron-down" : (Style::rtl() ? "chevron-left" : "chevron-right"));
        m_chevron->setColor(colors.detail);
        setShown(m_chevron, true);
      }
    }

  private:
    struct Colors {
      ColorSpec foreground;
      ColorSpec detail;
    };

    static void setShown(Node* node, bool shown) {
      if (node != nullptr) {
        node->setVisible(shown);
        node->setParticipatesInLayout(shown);
      }
    }

    Colors applyState(bool enabled, bool highlighted, bool selected, bool hovered) {
      if (highlighted) {
        setFill(colorSpecFromRole(ColorRole::Primary));
      } else if (hovered) {
        setFill(colorSpecFromRole(ColorRole::Hover));
      } else if (selected) {
        setFill(colorSpecFromRole(ColorRole::PrimaryContainer));
      } else {
        setFill(clearColorSpec());
      }

      if (highlighted) {
        return {colorSpecFromRole(ColorRole::OnPrimary), colorSpecFromRole(ColorRole::OnPrimary)};
      }
      if (hovered) {
        return {colorSpecFromRole(ColorRole::OnHover), colorSpecFromRole(ColorRole::OnHover)};
      }
      if (selected && enabled) {
        return {colorSpecFromRole(ColorRole::OnPrimaryContainer), colorSpecFromRole(ColorRole::OnPrimaryContainer)};
      }
      return {
          enabled ? colorSpecFromRole(ColorRole::OnSurface) : colorSpecFromRole(ColorRole::OnSurface, 0.55F),
          colorSpecFromRole(ColorRole::OnSurfaceVariant, enabled ? 1.0F : 0.55F),
      };
    }

    void bindPreview(const ColorSwatchPreview& preview, bool shown) {
      if (m_preview != nullptr) {
        m_preview->setMetricsFromFontSize(Style::fontSizeBody);
        m_preview->setPreview(preview);
        setShown(m_preview, shown);
      }
    }

    ColorSwatchPreviewStrip* m_preview = nullptr;
    Glyph* m_icon = nullptr;
    Flex* m_text = nullptr;
    Label* m_title = nullptr;
    Label* m_detail = nullptr;
    Label* m_count = nullptr;
    Glyph* m_chevron = nullptr;
  };

  // A child row drops its group's name when the label starts with it: "Catppuccin Mocha" under "Catppuccin" shows
  // "Mocha".
  std::string_view childLabel(const SearchPickerOption& option) {
    const std::string_view label = option.label;
    if (label.size() > option.group.size() + 1
        && label.starts_with(option.group)
        && label[option.group.size()] == ' ') {
      return label.substr(option.group.size() + 1);
    }
    return label;
  }

} // namespace

SearchPicker::SearchPicker() {
  m_emptyText = i18n::tr("ui.controls.search-picker.empty");

  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm);
  setPadding(Style::spaceSm);
  setFill(colorSpecFromRole(ColorRole::Surface));
  setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
  setRadius(Style::scaledRadiusMd());
  setSize(kDefaultWidth, kDefaultHeight);

  addChild(
      ui::input({
          .out = &m_input,
          .placeholder = i18n::tr("ui.controls.search-picker.placeholder"),
          .controlHeight = Style::controlHeight,
          .onChange =
              [this](const std::string& value) {
                if (value == m_filter) {
                  return;
                }
                m_filter = value;
                m_filterDebounceTimer.start(kFilterDebounceInterval, [this] { applyFilter(); });
              },
          .onKeyEvent =
              [this](std::uint32_t sym, std::uint32_t modifiers) {
                if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
                  moveHighlight(1);
                  return true;
                }
                if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
                  moveHighlight(-1);
                  return true;
                }
                if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
                  activateHighlighted();
                  return true;
                }
                if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
                  if (m_onCancel) {
                    m_onCancel();
                  }
                  return true;
                }
                return false;
              },
      })
  );

  addChild(
      ui::label({
          .out = &m_emptyLabel,
          .text = m_emptyText,
          .fontSize = Style::fontSizeBody,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      })
  );

  addChild(
      ui::virtualListView({
          .out = &m_list,
          .itemGap = Style::spaceXs,
          .overscanItems = 4,
          .adapter = this,
          .flexGrow = 1.0F,
      })
  );
}

void SearchPicker::setOptions(std::vector<SearchPickerOption> options) {
  m_options = std::move(options);
  buildGroups();
  expandSelectedGroup();
  applyFilter();
}

void SearchPicker::setPlaceholder(std::string_view placeholder) {
  if (m_input != nullptr) {
    m_input->setPlaceholder(placeholder);
  }
}

void SearchPicker::setEmptyText(std::string_view text) {
  m_emptyText = std::string(text);
  if (m_emptyLabel != nullptr) {
    m_emptyLabel->setText(m_emptyText);
  }
  markLayoutDirty();
}

void SearchPicker::setSelectedValue(std::string_view value) {
  m_selectedValue = std::string(value);
  if (expandSelectedGroup()) {
    rebuildRows();
    m_highlightedVisibleIndex = std::min(m_highlightedVisibleIndex, m_visible.empty() ? 0 : m_visible.size() - 1);
  }
  ++m_revision;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  markLayoutDirty();
}

void SearchPicker::setOnActivated(std::function<void(const SearchPickerOption&)> callback) {
  m_onActivated = std::move(callback);
}

void SearchPicker::setOnCancel(std::function<void()> callback) { m_onCancel = std::move(callback); }

InputArea* SearchPicker::filterInputArea() const noexcept {
  return m_input != nullptr ? m_input->inputArea() : nullptr;
}

void SearchPicker::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_input != nullptr) {
    m_input->setEnabled(enabled);
  }
  ++m_revision;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  setOpacity(enabled ? 1.0F : 0.55F);
}

void SearchPicker::doLayout(Renderer& renderer) { Flex::doLayout(renderer); }

LayoutSize SearchPicker::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void SearchPicker::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

std::size_t SearchPicker::itemCount() const { return m_visible.size(); }

std::uint64_t SearchPicker::itemKey(std::size_t index) const {
  if (index >= m_visible.size()) {
    return 0;
  }
  const Row& row = m_visible[index];
  const auto key = static_cast<std::uint64_t>(row.index);
  return row.kind == RowKind::Group ? (key | (1ULL << 63)) : key;
}

std::uint64_t SearchPicker::itemRevision(std::size_t index) const {
  std::uint64_t revision = m_revision;
  if (index >= m_visible.size()) {
    return revision;
  }
  const Row& row = m_visible[index];
  if (row.kind == RowKind::Group) {
    const Group& group = m_groups[row.index];
    if (!group.expanded && groupContainsSelected(group)) {
      revision ^= (1ULL << 32);
    }
    if (group.expanded) {
      revision ^= (1ULL << 34);
    }
  } else if (m_options[row.index].value == m_selectedValue) {
    revision ^= (1ULL << 32);
  }
  if (index == m_highlightedVisibleIndex) {
    revision ^= (1ULL << 33);
  }
  if (row.child) {
    revision ^= (1ULL << 35);
  }
  return revision;
}

bool SearchPicker::itemInteractive(std::size_t index) const {
  if (!m_enabled || index >= m_visible.size()) {
    return false;
  }
  const SearchPickerOption* option = rowOption(index);
  return option == nullptr || option->enabled;
}

float SearchPicker::measureItem(Renderer& /*renderer*/, std::size_t index, float /*width*/) {
  const SearchPickerOption* option = rowOption(index);
  const bool hasDetail = option != nullptr && !option->description.empty();
  return hasDetail ? Style::controlHeightLg : Style::controlHeight;
}

std::unique_ptr<Node> SearchPicker::createItem() { return std::make_unique<SearchPickerRow>(); }

void SearchPicker::bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float /*width*/, bool hovered) {
  auto* rowNode = dynamic_cast<SearchPickerRow*>(&item);
  if (rowNode == nullptr || index >= m_visible.size()) {
    return;
  }
  const Row& row = m_visible[index];
  const bool highlighted = index == m_highlightedVisibleIndex;
  if (row.kind == RowKind::Group) {
    const Group& group = m_groups[row.index];
    rowNode->bindGroup(
        group.name, group.members.size(), m_options[group.members.front()].preview, group.expanded, highlighted,
        !group.expanded && groupContainsSelected(group), hovered
    );
    return;
  }

  const auto& option = m_options[row.index];
  const std::string_view label = row.child ? childLabel(option) : std::string_view(option.label);
  rowNode->bind(option, label, row.child, highlighted, option.value == m_selectedValue, hovered);
}

void SearchPicker::onActivate(std::size_t index) { activateRow(index); }

void SearchPicker::buildGroups() {
  m_groups.clear();
  m_optionGroup.assign(m_options.size(), kNoGroup);

  std::unordered_map<std::string_view, std::size_t> byName;
  for (std::size_t i = 0; i < m_options.size(); ++i) {
    const std::string& name = m_options[i].group;
    if (name.empty()) {
      continue;
    }
    const auto [it, inserted] = byName.try_emplace(name, m_groups.size());
    if (inserted) {
      m_groups.push_back(Group{.name = name, .members = {}, .expanded = false});
    }
    m_groups[it->second].members.push_back(i);
  }

  // A family with one member is just a palette; it stays a plain row.
  std::erase_if(m_groups, [](const Group& group) { return group.members.size() < 2; });
  for (std::size_t g = 0; g < m_groups.size(); ++g) {
    for (const std::size_t member : m_groups[g].members) {
      m_optionGroup[member] = g;
    }
  }
}

bool SearchPicker::expandSelectedGroup() {
  for (std::size_t i = 0; i < m_options.size(); ++i) {
    if (m_options[i].value != m_selectedValue) {
      continue;
    }
    const std::size_t g = i < m_optionGroup.size() ? m_optionGroup[i] : kNoGroup;
    if (g == kNoGroup || m_groups[g].expanded) {
      return false;
    }
    m_groups[g].expanded = true;
    return true;
  }
  return false;
}

bool SearchPicker::groupContainsSelected(const Group& group) const {
  return std::ranges::any_of(group.members, [this](std::size_t member) {
    return m_options[member].value == m_selectedValue;
  });
}

void SearchPicker::rebuildRows() {
  struct ScoredOption {
    std::size_t index = 0;
    double score = 0.0;
  };

  m_visible.clear();
  const std::string query = StringUtils::trim(m_filter);

  if (query.empty()) {
    for (std::size_t i = 0; i < m_options.size(); ++i) {
      const std::size_t g = m_optionGroup[i];
      if (g == kNoGroup) {
        m_visible.push_back(Row{.kind = RowKind::Option, .index = i, .child = false});
        continue;
      }
      const Group& group = m_groups[g];
      if (group.members.front() != i) {
        continue;
      }
      m_visible.push_back(Row{.kind = RowKind::Group, .index = g, .child = false});
      if (group.expanded) {
        for (const std::size_t member : group.members) {
          m_visible.push_back(Row{.kind = RowKind::Option, .index = member, .child = true});
        }
      }
    }
    return;
  }

  std::vector<ScoredOption> scored;
  for (std::size_t i = 0; i < m_options.size(); ++i) {
    const double score = matchScore(m_options[i], query);
    if (FuzzyMatch::isMatch(score)) {
      scored.push_back(ScoredOption{.index = i, .score = score});
    }
  }
  std::ranges::stable_sort(scored, std::ranges::greater{}, &ScoredOption::score);
  m_visible.reserve(scored.size());
  for (const auto& item : scored) {
    m_visible.push_back(Row{.kind = RowKind::Option, .index = item.index, .child = false});
  }
}

void SearchPicker::toggleGroup(std::size_t groupIndex) {
  m_groups[groupIndex].expanded = !m_groups[groupIndex].expanded;
  rebuildRows();
  for (std::size_t i = 0; i < m_visible.size(); ++i) {
    if (m_visible[i].kind == RowKind::Group && m_visible[i].index == groupIndex) {
      m_highlightedVisibleIndex = i;
      break;
    }
  }
  ++m_revision;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  markLayoutDirty();
}

void SearchPicker::activateRow(std::size_t visibleIndex) {
  if (!m_enabled || visibleIndex >= m_visible.size()) {
    return;
  }
  const Row& row = m_visible[visibleIndex];
  if (row.kind == RowKind::Group) {
    toggleGroup(row.index);
    return;
  }
  const auto& option = m_options[row.index];
  if (option.enabled && m_onActivated) {
    m_onActivated(option);
  }
}

const SearchPickerOption* SearchPicker::rowOption(std::size_t visibleIndex) const {
  if (visibleIndex >= m_visible.size() || m_visible[visibleIndex].kind != RowKind::Option) {
    return nullptr;
  }
  return &m_options[m_visible[visibleIndex].index];
}

void SearchPicker::applyFilter() {
  rebuildRows();

  m_highlightedVisibleIndex = 0;
  if (StringUtils::trim(m_filter).empty() && !m_selectedValue.empty()) {
    for (std::size_t i = 0; i < m_visible.size(); ++i) {
      const SearchPickerOption* option = rowOption(i);
      if (option != nullptr && option->value == m_selectedValue) {
        m_highlightedVisibleIndex = i;
        break;
      }
    }
  }
  ++m_revision;
  if (m_emptyLabel != nullptr) {
    m_emptyLabel->setVisible(m_visible.empty());
  }
  if (m_list != nullptr) {
    m_list->setVisible(!m_visible.empty());
    m_list->scrollView().setScrollOffset(0.0F);
    m_list->notifyDataChanged();
    if (!m_visible.empty() && m_highlightedVisibleIndex > 0) {
      m_list->scrollToIndex(m_highlightedVisibleIndex);
    }
  }
  markLayoutDirty();
}

void SearchPicker::setHighlightedVisibleIndex(std::size_t index) {
  if (index >= m_visible.size()) {
    return;
  }
  const std::size_t previous = m_highlightedVisibleIndex;
  m_highlightedVisibleIndex = index;
  notifyHighlightedChanged(previous, m_highlightedVisibleIndex);
  ensureHighlightedVisible();
}

void SearchPicker::moveHighlight(int delta) {
  if (m_visible.empty()) {
    return;
  }
  const int count = static_cast<int>(m_visible.size());
  const int next = (static_cast<int>(m_highlightedVisibleIndex) + delta + count) % count;
  setHighlightedVisibleIndex(static_cast<std::size_t>(next));
}

void SearchPicker::activateHighlighted() { activateRow(m_highlightedVisibleIndex); }

void SearchPicker::ensureHighlightedVisible() {
  if (m_list == nullptr || m_highlightedVisibleIndex >= m_visible.size()) {
    return;
  }
  m_list->scrollToIndex(m_highlightedVisibleIndex);
}

void SearchPicker::notifyHighlightedChanged(std::size_t previous, std::size_t next) {
  if (m_list == nullptr) {
    return;
  }
  if (previous < m_visible.size()) {
    m_list->notifyItemChanged(previous);
  }
  if (next < m_visible.size() && next != previous) {
    m_list->notifyItemChanged(next);
  }
  markPaintDirty();
}

double SearchPicker::matchScore(const SearchPickerOption& option, std::string_view query) const {
  const std::string haystack = option.label + " " + option.value + " " + option.description + " " + option.group;
  return FuzzyMatch::score(query, haystack);
}

namespace ui {

  std::unique_ptr<SearchPicker> searchPicker(SearchPickerProps props) {
    auto control = std::make_unique<SearchPicker>();
    if (props.placeholder.has_value()) {
      control->setPlaceholder(*props.placeholder);
    }
    if (props.emptyText.has_value()) {
      control->setEmptyText(*props.emptyText);
    }
    if (props.selectedValue.has_value()) {
      control->setSelectedValue(*props.selectedValue);
    }
    if (props.options.has_value()) {
      control->setOptions(std::move(*props.options));
    }
    if (props.onActivated) {
      control->setOnActivated(std::move(props.onActivated));
    }
    if (props.onCancel) {
      control->setOnCancel(std::move(props.onCancel));
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    if (props.width.has_value() || props.height.has_value()) {
      control->setSize(props.width.value_or(control->width()), props.height.value_or(control->height()));
    }
    if (props.flexGrow.has_value()) {
      control->setFlexGrow(*props.flexGrow);
    }
    if (props.opacity.has_value()) {
      control->setOpacity(*props.opacity);
    }
    if (props.visible.has_value()) {
      control->setVisible(*props.visible);
    }
    if (props.participatesInLayout.has_value()) {
      control->setParticipatesInLayout(*props.participatesInLayout);
    }
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

} // namespace ui
