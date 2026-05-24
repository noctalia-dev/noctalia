#include "ui/dialogs/file_dialog_view.h"

#include "core/deferred_call.h"
#include "core/key_modifiers.h"
#include "core/key_symbols.h"
#include "core/keybind_matcher.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/thumbnail_service.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr std::size_t kListRowOverscan = 3;
  constexpr std::size_t kGridRowOverscan = 1;
  constexpr float kGridMinCellWidth = 140.0f;

} // namespace

class FileListAdapter final : public VirtualGridAdapter {
public:
  explicit FileListAdapter(float scale) : m_scale(scale) {}

  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setEntries(const std::vector<FileEntry>* entries) { m_entries = entries; }
  void setSelectableFn(std::function<bool(std::size_t)> fn) { m_isSelectable = std::move(fn); }
  void setOnActivate(std::function<void(std::size_t)> fn) { m_onActivate = std::move(fn); }

  [[nodiscard]] std::size_t itemCount() const override { return m_entries == nullptr ? 0 : m_entries->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<FileEntryRow>(m_scale); }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_entries == nullptr || index >= m_entries->size()) {
      return;
    }
    auto* row = static_cast<FileEntryRow*>(&tile);
    const bool disabled = m_isSelectable && !m_isSelectable(index);
    row->bind(*m_renderer, (*m_entries)[index], index, row->width(), selected, hovered && !selected, disabled);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

private:
  float m_scale = 1.0f;
  Renderer* m_renderer = nullptr;
  const std::vector<FileEntry>* m_entries = nullptr;
  std::function<bool(std::size_t)> m_isSelectable;
  std::function<void(std::size_t)> m_onActivate;
};

class FileGridAdapter final : public VirtualGridAdapter {
public:
  FileGridAdapter(float scale, ThumbnailService* thumbnails) : m_scale(scale), m_thumbnails(thumbnails) {}

  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setEntries(const std::vector<FileEntry>* entries) { m_entries = entries; }
  void setSelectableFn(std::function<bool(std::size_t)> fn) { m_isSelectable = std::move(fn); }
  void setOnActivate(std::function<void(std::size_t)> fn) { m_onActivate = std::move(fn); }

  [[nodiscard]] std::size_t itemCount() const override { return m_entries == nullptr ? 0 : m_entries->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<FileEntryTile>(m_scale, m_thumbnails);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_entries == nullptr || index >= m_entries->size()) {
      return;
    }
    auto* file = static_cast<FileEntryTile*>(&tile);
    const bool disabled = m_isSelectable && !m_isSelectable(index);
    // FileEntryTile::bind detects same-thumbnailPath rebinds and skips acquire/release,
    // so per-frame rebinds that VirtualGridView's row-modulo recycling already filters
    // out remain free of thumbnail churn.
    file->bind(
        *m_renderer, (*m_entries)[index], index, file->width(), file->height(), selected, hovered && !selected, disabled
    );
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

private:
  float m_scale = 1.0f;
  ThumbnailService* m_thumbnails = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<FileEntry>* m_entries = nullptr;
  std::function<bool(std::size_t)> m_isSelectable;
  std::function<void(std::size_t)> m_onActivate;
};

FileDialogView::FileDialogView(ThumbnailService* thumbnails) : m_thumbnails(thumbnails) {}

FileDialogView::~FileDialogView() = default;

void FileDialogView::create() {
  const float scale = contentScale();
  m_listRowHeight = std::ceil(32.0f * scale);
  m_gridCellSize = kGridMinCellWidth * scale;

  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceMd * scale,
  });

  auto listFocus = std::make_unique<InputArea>();
  listFocus->setFocusable(true);
  listFocus->setVisible(false);
  listFocus->setParticipatesInLayout(false);
  m_listFocusArea = static_cast<InputArea*>(root->addChild(std::move(listFocus)));

  root->addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
          },
          ui::label({
              .out = &m_titleLabel,
              .fontSize = Style::fontSizeTitle * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
              .fontWeight = FontWeight::Bold,
          }),
          ui::spacer(),
          ui::button({
              .glyph = "close",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Default,
              .minWidth = Style::controlHeightSm * scale,
              .minHeight = Style::controlHeightSm * scale,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this]() { DeferredCall::callLater([this]() { cancelDialog(); }); },
          })
      )
  );

  root->addChild(
      ui::row({
          .out = &m_breadcrumbRow,
          .align = FlexAlign::Center,
          .gap = Style::spaceXs * scale,
          .minHeight = Style::controlHeightSm * scale,
          .fillWidth = true,
          .clipChildren = true,
      })
  );

  root->addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
          },
          ui::input({
              .out = &m_searchInput,
              .placeholder = i18n::tr("ui.dialogs.file.filter-placeholder"),
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceMd * scale,
              .width = 320.0f * scale,
              .flexGrow = 1.0f,
              .onChange =
                  [this](const std::string& text) {
                    m_filterQuery = text;
                    applyFilter(true);
                  },
              .onSubmit = [this](const std::string&) { activateSelection(); },
          }),
          ui::button({
              .out = &m_backButton,
              .glyph = "arrow-big-up",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Default,
              .minWidth = Style::controlHeightSm * scale,
              .minHeight = Style::controlHeightSm * scale,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this]() { DeferredCall::callLater([this]() { navigateUp(); }); },
          }),
          ui::spacer(),
          ui::label({
              .out = &m_sortLabel,
              .fontSize = Style::fontSizeCaption * scale,
          }),
          ui::button({
              .out = &m_hiddenToggle,
              .glyph = "eye",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Tab,
              .minWidth = Style::controlHeightSm * scale,
              .minHeight = Style::controlHeightSm * scale,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this]() { DeferredCall::callLater([this]() { setShowHiddenFiles(!m_showHiddenFiles); }); },
          }),
          ui::button({
              .out = &m_viewToggle,
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Default,
              .minWidth = Style::controlHeightSm * scale,
              .minHeight = Style::controlHeightSm * scale,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this]() {
                DeferredCall::callLater([this]() {
                  setViewMode(m_viewMode == ViewMode::List ? ViewMode::Grid : ViewMode::List);
                });
              },
          })
      )
  );

  auto listContainer = ui::column({
      .out = &m_listContainer,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0f,
  });

  listContainer->addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
          },
          ui::button({
              .out = &m_nameSortButton,
              .text = i18n::tr("ui.dialogs.file.sort.name"),
              .contentAlign = ButtonContentAlign::Start,
              .variant = ButtonVariant::Ghost,
              // File-list header sort style.
              .minHeight = Style::controlHeightSm * scale,
              .paddingV = Style::spaceXs * scale,
              .paddingH = Style::spaceSm * scale,
              .flexGrow = 1.0f,
              .onClick = [this]() { DeferredCall::callLater([this]() { setSort(FileDialogSortField::Name); }); },
          }),
          ui::button({
              .out = &m_sizeSortButton,
              .text = i18n::tr("ui.dialogs.file.sort.size"),
              .contentAlign = ButtonContentAlign::End,
              .variant = ButtonVariant::Ghost,
              // File-list header sort style.
              .minWidth = 96.0f * scale,
              .minHeight = Style::controlHeightSm * scale,
              .paddingV = Style::spaceXs * scale,
              .paddingH = Style::spaceSm * scale,
              .onClick = [this]() { DeferredCall::callLater([this]() { setSort(FileDialogSortField::Size); }); },
          }),
          ui::button({
              .out = &m_dateSortButton,
              .text = i18n::tr("ui.dialogs.file.sort.date"),
              .contentAlign = ButtonContentAlign::End,
              .variant = ButtonVariant::Ghost,
              // File-list header sort style.
              .minWidth = 152.0f * scale,
              .minHeight = Style::controlHeightSm * scale,
              .paddingV = Style::spaceXs * scale,
              .paddingH = Style::spaceSm * scale,
              .onClick = [this]() { DeferredCall::callLater([this]() { setSort(FileDialogSortField::Modified); }); },
          })
      )
  );

  listContainer->addChild(ui::separator());

  m_listAdapter = std::make_unique<FileListAdapter>(scale);
  m_listAdapter->setEntries(&m_visibleEntries);
  m_listAdapter->setSelectableFn([this](std::size_t idx) { return isSelectableIndex(idx); });
  m_listAdapter->setOnActivate([this](std::size_t idx) {
    DeferredCall::callLater([this, idx]() { handleEntryClick(idx); });
  });

  listContainer->addChild(
      ui::virtualGridView({
          .out = &m_listGrid,
          .columns = 1,
          .cellHeight = m_listRowHeight,
          .squareCells = false,
          .columnGap = 0.0f,
          .rowGap = 0.0f,
          .overscanRows = kListRowOverscan,
          .scrollbarVisible = true,
          .scrollCardStyleScale = scale,
          .adapter = m_listAdapter.get(),
          .flexGrow = 1.0f,
          .onSelectionChanged = [this](std::optional<std::size_t>) { syncGridSelection(); },
      })
  );

  listContainer->addChild(
      ui::label({
          .out = &m_listEmptyLabel,
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Label& label) { label.setCaptionStyle(); },
      })
  );

  root->addChild(std::move(listContainer));

  auto gridContainer = ui::column({
      .out = &m_gridContainer,
      .align = FlexAlign::Stretch,
      .flexGrow = 1.0f,
      .visible = false,
  });

  m_gridAdapter = std::make_unique<FileGridAdapter>(scale, m_thumbnails);
  m_gridAdapter->setEntries(&m_visibleEntries);
  m_gridAdapter->setSelectableFn([this](std::size_t idx) { return isSelectableIndex(idx); });
  m_gridAdapter->setOnActivate([this](std::size_t idx) {
    DeferredCall::callLater([this, idx]() { handleEntryClick(idx); });
  });

  gridContainer->addChild(
      ui::virtualGridView({
          .out = &m_gridGrid,
          .columns = 0,
          .minCellWidth = m_gridCellSize,
          .squareCells = true,
          .columnGap = Style::spaceSm * scale,
          .rowGap = Style::spaceSm * scale,
          .overscanRows = kGridRowOverscan,
          .scrollbarVisible = true,
          .scrollCardStyleScale = scale,
          .adapter = m_gridAdapter.get(),
          .flexGrow = 1.0f,
          .onSelectionChanged = [this](std::optional<std::size_t>) { syncGridSelection(); },
      })
  );

  gridContainer->addChild(
      ui::label({
          .out = &m_gridEmptyLabel,
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Label& label) { label.setCaptionStyle(); },
      })
  );

  root->addChild(std::move(gridContainer));

  root->addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
          },
          ui::input({
              .out = &m_filenameInput,
              .placeholder = i18n::tr("ui.dialogs.file.filename-placeholder"),
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceMd * scale,
              .flexGrow = 1.0f,
              .onChange = [this](const std::string&) { updateControls(); },
              .onSubmit = [this](const std::string&) { submitDialog(); },
          }),
          ui::spacer(),
          ui::button({
              .out = &m_cancelButton,
              .text = i18n::tr("common.actions.cancel"),
              .variant = ButtonVariant::Secondary,
              // Dialog footer action style.
              .minWidth = 92.0f * scale,
              .minHeight = Style::controlHeight * scale,
              .paddingV = Style::spaceSm * scale,
              .paddingH = Style::spaceMd * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this]() { DeferredCall::callLater([this]() { cancelDialog(); }); },
          }),
          ui::button({
              .out = &m_okButton,
              .variant = ButtonVariant::Primary,
              // Dialog footer action style.
              .minWidth = 92.0f * scale,
              .minHeight = Style::controlHeight * scale,
              .paddingV = Style::spaceSm * scale,
              .paddingH = Style::spaceMd * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this]() { DeferredCall::callLater([this]() { submitDialog(); }); },
          })
      )
  );
  setRoot(std::move(root));

  if (m_animations != nullptr && this->root() != nullptr) {
    this->root()->setAnimationManager(m_animations);
  }

  if (m_thumbnails != nullptr) {
    m_thumbnailPendingSub = m_thumbnails->subscribePendingUpload([this]() {
      m_thumbnailRefreshPending = true;
      requestUpdateOnly();
    });
  }
}

void FileDialogView::onOpen(std::string_view /*context*/) {
  m_options = FileDialog::currentOptions();
  m_currentDirectory = resolveStartDirectory(m_options.startDirectory);
  m_filterQuery.clear();
  m_viewMode = m_options.defaultViewMode == FileDialogViewMode::Grid ? ViewMode::Grid : ViewMode::List;
  m_sortField = FileDialogSortField::Name;
  m_sortOrder = FileDialogSortOrder::Ascending;
  m_showHiddenFiles = m_options.showHiddenFiles;
  m_selectedIndex = static_cast<std::size_t>(-1);
  m_thumbnailRefreshPending = false;

  if (m_titleLabel != nullptr) {
    m_titleLabel->setText(m_options.title);
  }
  if (m_searchInput != nullptr) {
    m_searchInput->setValue("");
  }
  if (m_filenameInput != nullptr) {
    m_filenameInput->setValue(m_options.defaultFilename);
    const bool showFilename = m_options.mode != FileDialogMode::SelectFolder;
    m_filenameInput->setVisible(showFilename);
    m_filenameInput->inputArea()->setEnabled(m_options.mode == FileDialogMode::Save);
    m_filenameInput->inputArea()->setFocusable(m_options.mode == FileDialogMode::Save);
  }

  setViewMode(m_viewMode);
  refreshDirectory();
}

void FileDialogView::onClose() {
  if (m_listGrid != nullptr) {
    m_listGrid->setAdapter(nullptr);
  }
  if (m_gridGrid != nullptr) {
    m_gridGrid->setAdapter(nullptr);
  }

  m_entries.clear();
  m_visibleEntries.clear();
  m_currentDirectory.clear();
  m_filterQuery.clear();
  m_selectedIndex = static_cast<std::size_t>(-1);
  m_rootLayout = nullptr;
  m_titleLabel = nullptr;
  m_breadcrumbRow = nullptr;
  m_homeButton = nullptr;
  m_backButton = nullptr;
  m_searchInput = nullptr;
  m_sortLabel = nullptr;
  m_hiddenToggle = nullptr;
  m_viewToggle = nullptr;
  m_listContainer = nullptr;
  m_nameSortButton = nullptr;
  m_sizeSortButton = nullptr;
  m_dateSortButton = nullptr;
  m_listGrid = nullptr;
  m_listEmptyLabel = nullptr;
  m_gridContainer = nullptr;
  m_gridGrid = nullptr;
  m_gridEmptyLabel = nullptr;
  m_filenameInput = nullptr;
  m_cancelButton = nullptr;
  m_okButton = nullptr;
  m_listFocusArea = nullptr;
  m_listAdapter.reset();
  m_gridAdapter.reset();
  m_thumbnailPendingSub.disconnect();
  clearReleasedRoot();
}

InputArea* FileDialogView::initialFocusArea() const {
  if (m_options.mode == FileDialogMode::Save && m_filenameInput != nullptr) {
    return m_filenameInput->inputArea();
  }
  return m_searchInput != nullptr ? m_searchInput->inputArea() : m_listFocusArea;
}

void FileDialogView::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }

  if (m_listAdapter != nullptr) {
    m_listAdapter->setRenderer(&renderer);
  }
  if (m_gridAdapter != nullptr) {
    m_gridAdapter->setRenderer(&renderer);
  }

  if (m_thumbnails != nullptr && m_thumbnails->uploadPending(renderer.textureManager())) {
    m_thumbnailRefreshPending = false;
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  if (m_gridGrid != nullptr) {
    const float viewportW = m_gridGrid->scrollView().contentViewportWidth();
    if (viewportW > 0.0f) {
      const float gap = Style::spaceSm * contentScale();
      const std::size_t columns =
          std::max<std::size_t>(1, static_cast<std::size_t>(std::floor((viewportW + gap) / (m_gridCellSize + gap))));
      m_gridColumns = columns;
    }
  }
}

void FileDialogView::doUpdate(Renderer& renderer) {
  if (!m_thumbnailRefreshPending || m_thumbnails == nullptr) {
    return;
  }

  const bool changed = m_thumbnails->uploadPending(renderer.textureManager());
  m_thumbnailRefreshPending = false;
  if (changed && m_viewMode == ViewMode::Grid && m_gridGrid != nullptr) {
    // Force visible tiles to rebind so FileEntryTile::bind re-queries the thumbnail
    // service. Same-path rebinds are a no-op for the thumbnail cache.
    m_gridGrid->notifyDataChanged();
  }
  requestLayout();
  requestRedraw();
}

bool FileDialogView::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }

  if ((modifiers & KeyMod::Ctrl) != 0 && (sym == XKB_KEY_l || sym == XKB_KEY_L)) {
    focusSearch();
    return true;
  }

  if (sym == XKB_KEY_Tab) {
    cycleFocus((modifiers & KeyMod::Shift) != 0);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    cancelDialog();
    return true;
  }

  if (KeySymbol::isBackspace(sym) && !isTextInputFocused()) {
    navigateUp();
    return true;
  }

  if (m_visibleEntries.empty()) {
    if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
      if (m_options.mode == FileDialogMode::SelectFolder) {
        submitDialog();
        return true;
      }
    }
    return false;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    if (!isTextInputFocused() || hostFocusedArea() == m_listFocusArea) {
      activateSelection();
      return true;
    }
    return false;
  }

  InputArea* focused = hostFocusedArea();
  const bool filenameFocused = m_filenameInput != nullptr && focused == m_filenameInput->inputArea();
  if (filenameFocused) {
    return false;
  }

  auto moveSelection = [this](int delta) {
    if (m_visibleEntries.empty()) {
      return;
    }
    if (m_selectedIndex == static_cast<std::size_t>(-1)) {
      selectIndex(firstSelectableIndex());
      return;
    }

    int next = static_cast<int>(m_selectedIndex) + delta;
    while (next >= 0 && next < static_cast<int>(m_visibleEntries.size())) {
      if (isSelectableIndex(static_cast<std::size_t>(next))) {
        selectIndex(static_cast<std::size_t>(next));
        return;
      }
      next += delta > 0 ? 1 : -1;
    }
  };

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    moveSelection(m_viewMode == ViewMode::Grid ? -static_cast<int>(m_gridColumns) : -1);
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    moveSelection(m_viewMode == ViewMode::Grid ? static_cast<int>(m_gridColumns) : 1);
    return true;
  }
  if (m_viewMode == ViewMode::Grid && KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    moveSelection(-1);
    return true;
  }
  if (m_viewMode == ViewMode::Grid && KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    moveSelection(1);
    return true;
  }

  return false;
}

void FileDialogView::refreshDirectory() {
  m_entries = m_scanner.scan(m_currentDirectory, m_options.extensions, m_showHiddenFiles, m_sortField, m_sortOrder);
  rebuildBreadcrumb();
  applyFilter(true);
}

void FileDialogView::applyFilter(bool resetScroll) {
  const std::filesystem::path preserved = selectedPath();
  const std::string query = StringUtils::toLower(m_filterQuery);

  m_visibleEntries.clear();
  for (const auto& entry : m_entries) {
    if (!query.empty() && StringUtils::toLower(entry.name).find(query) == std::string::npos) {
      continue;
    }
    m_visibleEntries.push_back(entry);
  }

  m_selectedIndex = static_cast<std::size_t>(-1);
  if (!preserved.empty()) {
    for (std::size_t i = 0; i < m_visibleEntries.size(); ++i) {
      if (m_visibleEntries[i].absPath == preserved && isSelectableIndex(i)) {
        m_selectedIndex = i;
        break;
      }
    }
  }
  if (m_selectedIndex == static_cast<std::size_t>(-1)) {
    m_selectedIndex = firstSelectableIndex();
  }

  if (resetScroll) {
    if (m_listGrid != nullptr) {
      m_listGrid->scrollView().setScrollOffset(0.0f);
    }
    if (m_gridGrid != nullptr) {
      m_gridGrid->scrollView().setScrollOffset(0.0f);
    }
  }
  if (m_listGrid != nullptr) {
    m_listGrid->notifyDataChanged();
  }
  if (m_gridGrid != nullptr) {
    m_gridGrid->notifyDataChanged();
  }

  syncGridSelection();
  applyEmptyStates();
  updateFilenameFieldFromSelection();
  updateControls();
  if (root() != nullptr) {
    root()->markLayoutDirty();
  }
  requestLayout();
}

void FileDialogView::rebuildBreadcrumb() {
  if (m_breadcrumbRow == nullptr) {
    return;
  }

  while (!m_breadcrumbRow->children().empty()) {
    m_breadcrumbRow->removeChild(m_breadcrumbRow->children().back().get());
  }

  const float scale = contentScale();

  m_breadcrumbRow->addChild(
      ui::button({
          .out = &m_homeButton,
          .glyph = "home",
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .onClick = [this]() { DeferredCall::callLater([this]() { navigateHome(); }); },
      })
  );

  std::filesystem::path partial("/");
  for (const auto& component : m_currentDirectory.relative_path()) {
    m_breadcrumbRow->addChild(
        ui::label({
            .text = "/",
            .fontSize = Style::fontSizeCaption * scale,
        })
    );

    partial /= component;
    const auto target = partial;
    m_breadcrumbRow->addChild(
        ui::button({
            .text = component.string(),
            .variant = ButtonVariant::Ghost,
            .padding = Style::spaceXs * scale,
            .onClick = [this, target]() { DeferredCall::callLater([this, target]() { navigateInto(target); }); },
        })
    );
  }
}

void FileDialogView::applyEmptyStates() {
  const bool empty = m_visibleEntries.empty();
  const std::string emptyText = empty ? i18n::tr("ui.dialogs.file.empty-filtered") : std::string();

  if (m_listEmptyLabel != nullptr) {
    m_listEmptyLabel->setText(emptyText);
    m_listEmptyLabel->setVisible(empty);
    m_listEmptyLabel->setParticipatesInLayout(empty);
  }
  if (m_listGrid != nullptr) {
    m_listGrid->setVisible(!empty);
    m_listGrid->setParticipatesInLayout(!empty);
  }

  if (m_gridEmptyLabel != nullptr) {
    m_gridEmptyLabel->setText(emptyText);
    m_gridEmptyLabel->setVisible(empty);
    m_gridEmptyLabel->setParticipatesInLayout(empty);
  }
  if (m_gridGrid != nullptr) {
    m_gridGrid->setVisible(!empty);
    m_gridGrid->setParticipatesInLayout(!empty);
  }
}

void FileDialogView::updateControls() {
  std::string okText = i18n::tr("ui.dialogs.file.actions.open");
  switch (m_options.mode) {
  case FileDialogMode::Open:
    okText = i18n::tr("ui.dialogs.file.actions.open");
    break;
  case FileDialogMode::Save:
    okText = i18n::tr("ui.dialogs.file.actions.save");
    break;
  case FileDialogMode::SelectFolder:
    okText = i18n::tr("ui.dialogs.file.actions.select-folder");
    break;
  }

  if (m_okButton != nullptr) {
    m_okButton->setText(okText);
    bool enabled = false;
    if (m_options.mode == FileDialogMode::Open) {
      enabled = m_selectedIndex < m_visibleEntries.size() && !m_visibleEntries[m_selectedIndex].isDir;
    } else if (m_options.mode == FileDialogMode::Save) {
      enabled = m_filenameInput != nullptr && !m_filenameInput->value().empty();
    } else {
      enabled = true;
    }
    m_okButton->setEnabled(enabled);
  }

  if (m_backButton != nullptr) {
    m_backButton->setEnabled(
        m_currentDirectory.has_parent_path() && m_currentDirectory != m_currentDirectory.root_path()
    );
  }
  if (m_hiddenToggle != nullptr) {
    m_hiddenToggle->setSelected(m_showHiddenFiles);
  }
  if (m_viewToggle != nullptr) {
    m_viewToggle->setGlyph(m_viewMode == ViewMode::List ? "layout-grid" : "list");
  }

  if (m_sortLabel != nullptr) {
    std::string field = i18n::tr("ui.dialogs.file.sort.name");
    switch (m_sortField) {
    case FileDialogSortField::Name:
      field = i18n::tr("ui.dialogs.file.sort.name");
      break;
    case FileDialogSortField::Size:
      field = i18n::tr("ui.dialogs.file.sort.size");
      break;
    case FileDialogSortField::Modified:
      field = i18n::tr("ui.dialogs.file.sort.date");
      break;
    }
    m_sortLabel->setText(
        i18n::tr(
            "ui.dialogs.file.sort.summary", "field", field, "direction",
            m_sortOrder == FileDialogSortOrder::Ascending ? i18n::tr("ui.dialogs.file.sort.ascending")
                                                          : i18n::tr("ui.dialogs.file.sort.descending")
        )
    );
  }

  if (m_listContainer != nullptr) {
    m_listContainer->setVisible(m_viewMode == ViewMode::List);
  }
  if (m_gridContainer != nullptr) {
    m_gridContainer->setVisible(m_viewMode == ViewMode::Grid);
  }
}

void FileDialogView::setShowHiddenFiles(bool show) {
  if (m_showHiddenFiles == show) {
    return;
  }

  m_showHiddenFiles = show;
  refreshDirectory();
}

void FileDialogView::updateFilenameFieldFromSelection() {
  if (m_filenameInput == nullptr || m_options.mode == FileDialogMode::SelectFolder) {
    return;
  }

  if (m_options.mode == FileDialogMode::Open) {
    if (m_selectedIndex < m_visibleEntries.size()) {
      m_filenameInput->setValue(m_visibleEntries[m_selectedIndex].name);
    } else {
      m_filenameInput->setValue("");
    }
    return;
  }

  if (m_selectedIndex < m_visibleEntries.size() && !m_visibleEntries[m_selectedIndex].isDir) {
    m_filenameInput->setValue(m_visibleEntries[m_selectedIndex].name);
  }
}

void FileDialogView::setViewMode(ViewMode mode) {
  if (m_viewMode == mode) {
    updateControls();
    return;
  }
  m_viewMode = mode;
  updateControls();
  ensureSelectionVisible();
  requestLayout();
}

void FileDialogView::setSort(FileDialogSortField field) {
  if (m_sortField == field) {
    m_sortOrder = m_sortOrder == FileDialogSortOrder::Ascending ? FileDialogSortOrder::Descending
                                                                : FileDialogSortOrder::Ascending;
  } else {
    m_sortField = field;
    m_sortOrder = FileDialogSortOrder::Ascending;
  }
  refreshDirectory();
}

void FileDialogView::navigateInto(const std::filesystem::path& path) {
  std::error_code ec;
  if (path.empty() || !std::filesystem::exists(path, ec) || ec || !std::filesystem::is_directory(path, ec) || ec) {
    return;
  }
  m_currentDirectory = path;
  refreshDirectory();
}

void FileDialogView::navigateUp() {
  if (!m_currentDirectory.has_parent_path()) {
    return;
  }
  const std::filesystem::path parent = m_currentDirectory.parent_path();
  if (parent.empty() || parent == m_currentDirectory) {
    return;
  }
  navigateInto(parent);
}

void FileDialogView::navigateHome() { navigateInto(homeDirectory()); }

void FileDialogView::selectIndex(std::size_t index) {
  if (index >= m_visibleEntries.size() || !isSelectableIndex(index)) {
    return;
  }
  m_selectedIndex = index;
  syncGridSelection();
  updateFilenameFieldFromSelection();
  updateControls();
  focusList();
  ensureSelectionVisible();
  requestRedraw();
}

void FileDialogView::handleEntryClick(std::size_t index) {
  if (index >= m_visibleEntries.size()) {
    syncGridSelection();
    return;
  }

  const FileEntry& entry = m_visibleEntries[index];
  if (entry.isDir) {
    if (m_options.mode == FileDialogMode::SelectFolder) {
      if (m_selectedIndex == index) {
        navigateInto(entry.absPath);
      } else {
        selectIndex(index);
      }
    } else {
      navigateInto(entry.absPath);
    }
    return;
  }

  if (!isSelectableIndex(index)) {
    syncGridSelection();
    return;
  }

  if (m_selectedIndex == index) {
    if (m_options.mode == FileDialogMode::Save) {
      updateFilenameFieldFromSelection();
    }
    submitDialog();
    return;
  }

  selectIndex(index);
}

void FileDialogView::activateSelection() {
  if (m_selectedIndex >= m_visibleEntries.size()) {
    if (m_options.mode == FileDialogMode::SelectFolder) {
      submitDialog();
    }
    return;
  }

  const FileEntry& entry = m_visibleEntries[m_selectedIndex];
  if (entry.isDir) {
    if (m_options.mode == FileDialogMode::SelectFolder) {
      submitDialog();
    } else {
      navigateInto(entry.absPath);
    }
    return;
  }

  if (m_options.mode == FileDialogMode::SelectFolder) {
    return;
  }
  submitDialog();
}

void FileDialogView::submitDialog() {
  if (m_options.mode == FileDialogMode::Open) {
    if (m_selectedIndex >= m_visibleEntries.size() || m_visibleEntries[m_selectedIndex].isDir) {
      return;
    }
    acceptDialog(m_visibleEntries[m_selectedIndex].absPath);
    return;
  }

  if (m_options.mode == FileDialogMode::Save) {
    if (m_filenameInput == nullptr || m_filenameInput->value().empty()) {
      return;
    }
    acceptDialog(m_currentDirectory / m_filenameInput->value());
    return;
  }

  if (m_selectedIndex < m_visibleEntries.size() && m_visibleEntries[m_selectedIndex].isDir) {
    acceptDialog(m_visibleEntries[m_selectedIndex].absPath);
  } else {
    acceptDialog(m_currentDirectory);
  }
}

void FileDialogView::focusSearch() {
  if (m_searchInput != nullptr) {
    focusHostArea(m_searchInput->inputArea());
  }
}

void FileDialogView::focusList() {
  if (m_listFocusArea != nullptr) {
    focusHostArea(m_listFocusArea);
  }
}

void FileDialogView::focusFilename() {
  if (m_options.mode == FileDialogMode::Save && m_filenameInput != nullptr) {
    focusHostArea(m_filenameInput->inputArea());
  }
}

void FileDialogView::cycleFocus(bool reverse) {
  std::vector<InputArea*> order;
  if (m_searchInput != nullptr) {
    order.push_back(m_searchInput->inputArea());
  }
  if (m_listFocusArea != nullptr) {
    order.push_back(m_listFocusArea);
  }
  if (m_options.mode == FileDialogMode::Save && m_filenameInput != nullptr) {
    order.push_back(m_filenameInput->inputArea());
  }
  if (order.empty()) {
    return;
  }

  InputArea* current = hostFocusedArea();
  auto it = std::find(order.begin(), order.end(), current);
  std::size_t index = it == order.end() ? 0 : static_cast<std::size_t>(std::distance(order.begin(), it));
  if (reverse) {
    index = index == 0 ? order.size() - 1 : index - 1;
  } else {
    index = (index + 1) % order.size();
  }
  focusHostArea(order[index]);
}

void FileDialogView::ensureSelectionVisible() {
  if (m_selectedIndex >= m_visibleEntries.size()) {
    return;
  }

  if (m_viewMode == ViewMode::List && m_listGrid != nullptr && m_listRowHeight > 0.0f) {
    ScrollView& scroll = m_listGrid->scrollView();
    const float top = static_cast<float>(m_selectedIndex) * m_listRowHeight;
    const float bottom = top + m_listRowHeight;
    const float viewport = m_listGrid->height();
    const float offset = scroll.scrollOffset();
    if (top < offset) {
      scroll.setScrollOffset(top);
    } else if (bottom > offset + viewport) {
      scroll.setScrollOffset(bottom - viewport);
    }
    requestLayout();
    return;
  }

  if (m_viewMode == ViewMode::Grid && m_gridGrid != nullptr && m_gridColumns > 0) {
    ScrollView& scroll = m_gridGrid->scrollView();
    const float gap = Style::spaceSm * contentScale();
    const float viewportW = m_gridGrid->scrollView().contentViewportWidth();
    const float columnsF = static_cast<float>(m_gridColumns);
    const float cellW = std::max(0.0f, (viewportW - (columnsF - 1.0f) * gap) / std::max(columnsF, 1.0f));
    const float cellH = cellW; // squareCells
    const float pitch = cellH + gap;
    const std::size_t row = m_selectedIndex / m_gridColumns;
    const float top = static_cast<float>(row) * pitch;
    const float bottom = top + cellH;
    const float viewport = m_gridGrid->height();
    const float offset = scroll.scrollOffset();
    if (top < offset) {
      scroll.setScrollOffset(top);
    } else if (bottom > offset + viewport) {
      scroll.setScrollOffset(bottom - viewport);
    }
    requestLayout();
  }
}

void FileDialogView::syncGridSelection() {
  const std::optional<std::size_t> selection =
      m_selectedIndex < m_visibleEntries.size() ? std::optional{m_selectedIndex} : std::nullopt;
  if (m_listGrid != nullptr) {
    m_listGrid->setSelectedIndex(selection);
  }
  if (m_gridGrid != nullptr) {
    m_gridGrid->setSelectedIndex(selection);
  }
}

std::size_t FileDialogView::firstSelectableIndex() const {
  for (std::size_t i = 0; i < m_visibleEntries.size(); ++i) {
    if (isSelectableIndex(i)) {
      return i;
    }
  }
  return static_cast<std::size_t>(-1);
}

bool FileDialogView::isSelectableIndex(std::size_t index) const {
  if (index >= m_visibleEntries.size()) {
    return false;
  }
  if (m_options.mode == FileDialogMode::SelectFolder) {
    return m_visibleEntries[index].isDir;
  }
  return true;
}

bool FileDialogView::isTextInputFocused() const {
  InputArea* focused = hostFocusedArea();
  if (m_searchInput != nullptr && focused == m_searchInput->inputArea()) {
    return true;
  }
  if (m_filenameInput != nullptr && focused == m_filenameInput->inputArea()) {
    return true;
  }
  return false;
}

std::filesystem::path FileDialogView::selectedPath() const {
  if (m_selectedIndex < m_visibleEntries.size()) {
    return m_visibleEntries[m_selectedIndex].absPath;
  }
  return {};
}

std::filesystem::path FileDialogView::homeDirectory() const {
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home);
  }
  return std::filesystem::current_path();
}

std::filesystem::path FileDialogView::resolveStartDirectory(const std::filesystem::path& preferred) const {
  std::error_code ec;
  if (!preferred.empty() && std::filesystem::exists(preferred, ec) && !ec &&
      std::filesystem::is_directory(preferred, ec) && !ec) {
    return preferred;
  }

  const std::filesystem::path home = homeDirectory();
  if (std::filesystem::exists(home, ec) && !ec && std::filesystem::is_directory(home, ec) && !ec) {
    return home;
  }
  return std::filesystem::current_path();
}

void FileDialogView::requestUpdateOnly() {
  if (m_host != nullptr) {
    m_host->requestUpdateOnly();
  }
}

void FileDialogView::requestLayout() {
  if (m_host != nullptr) {
    m_host->requestLayout();
  }
}

void FileDialogView::requestRedraw() {
  if (m_host != nullptr) {
    m_host->requestRedraw();
  }
}

void FileDialogView::focusHostArea(InputArea* area) {
  if (m_host != nullptr) {
    m_host->focusArea(area);
  }
}

InputArea* FileDialogView::hostFocusedArea() const { return m_host != nullptr ? m_host->focusedArea() : nullptr; }

void FileDialogView::acceptDialog(std::optional<std::filesystem::path> result) {
  if (m_host != nullptr) {
    m_host->accept(std::move(result));
  }
}

void FileDialogView::cancelDialog() {
  if (m_host != nullptr) {
    m_host->cancel();
  }
}
