#include "shell/clipboard/clipboard_panel.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/keybind_matcher.h"
#include "core/log.h"
#include "core/process.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/async_texture_cache.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

  constexpr float kSidebarWidth = 272.0f;
  constexpr float kRowHeight = 46.0f;
  constexpr float kPreviewImageHeight = 280.0f;
  constexpr float kListGlyphSize = 24.0f;
  constexpr float kListThumbSize = 40.0f;
  constexpr std::size_t kListOverscanRows = 3;
  constexpr auto kPreviewPayloadDebounceInterval = std::chrono::milliseconds(75);
  constexpr auto kFilterDebounceInterval = std::chrono::milliseconds(120);
  constexpr Logger kLog("clipboard");

  void replaceAll(std::string& text, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) {
      return;
    }

    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
      text.replace(pos, needle.size(), replacement);
      pos += replacement.size();
    }
  }

  std::string buildImageActionCommand(std::string command, std::string_view imagePath) {
    const bool hasPathPlaceholder = command.find("{path}") != std::string::npos;
    const bool hasStdinPlaceholder = command.find("{stdin}") != std::string::npos;
    const std::string quotedPath = StringUtils::shellQuote(imagePath);

    if (hasPathPlaceholder) {
      replaceAll(command, "{path}", quotedPath);
    }
    if (hasStdinPlaceholder) {
      replaceAll(command, "{stdin}", "-");
    }

    if (!hasPathPlaceholder || hasStdinPlaceholder) {
      return "cat -- " + quotedPath + " | " + command;
    }
    return command;
  }

  std::string collapseWhitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    bool lastWasSpace = true;
    for (char ch : text) {
      const bool isWhitespace = (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r');
      if (isWhitespace) {
        if (!lastWasSpace) {
          out.push_back(' ');
        }
        lastWasSpace = true;
        continue;
      }
      out.push_back(ch);
      lastWasSpace = false;
    }

    if (!out.empty() && out.back() == ' ') {
      out.pop_back();
    }
    return out;
  }

  std::string formatBytes(std::size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    std::size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1 < std::size(units)) {
      value /= 1024.0;
      ++unitIndex;
    }

    char buffer[32];
    if (unitIndex == 0) {
      std::snprintf(buffer, sizeof(buffer), "%zu %s", bytes, units[unitIndex]);
    } else {
      std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unitIndex]);
    }
    return buffer;
  }

  std::string entryTitle(const ClipboardEntry& entry) {
    if (!entry.textPreview.empty()) {
      return entry.textPreview;
    }
    if (entry.isImage()) {
      return i18n::tr("clipboard.entry.image");
    }
    return entry.dataMimeType.empty() ? i18n::tr("clipboard.entry.title") : entry.dataMimeType;
  }

  std::string previewTitle(const ClipboardEntry& entry) {
    if (entry.isImage()) {
      return i18n::tr("clipboard.preview.image-title");
    }
    return i18n::tr("clipboard.preview.text-title");
  }

  std::unique_ptr<Button> makeCompactIconButton(
      Button** out, std::string glyph, ButtonVariant variant, float scale, std::function<void()> onClick,
      bool visible = true, bool participatesInLayout = true
  ) {
    return ui::button({
        .out = out,
        .glyph = std::move(glyph),
        .glyphSize = Style::fontSizeBody * scale,
        .variant = variant,
        // Compact entry action style.
        .minWidth = Style::controlHeightSm * scale,
        .minHeight = Style::controlHeightSm * scale,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusMd(scale),
        .visible = visible,
        .participatesInLayout = participatesInLayout,
        .onClick = std::move(onClick),
    });
  }

  class ClipboardListRow final : public InputArea {
  public:
    ClipboardListRow(float scale, ThumbnailService* thumbnails) : m_scale(scale), m_thumbnails(thumbnails) {
      setVisible(false);

      addChild(
          ui::box({
              .out = &m_background,
              .radius = Style::scaledRadiusMd(scale),
          })
      );

      auto row = ui::row(
          {.out = &m_row, .align = FlexAlign::Center, .gap = Style::spaceMd * scale, .configure = [scale](Flex& flex) {
             flex.setPadding(Style::spaceXs * scale, Style::spaceSm * scale);
           }}
      );
      addChild(std::move(row));

      m_row->addChild(
          ui::row({
              .out = &m_lead,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Center,
          })
      );

      m_lead->addChild(
          ui::image({
              .out = &m_image,
              .fit = ImageFit::Cover,
              .radius = Style::scaledRadiusSm(scale),
              .visible = false,
          })
      );

      m_lead->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = kListGlyphSize * scale,
          })
      );

      m_row->addChild(
          ui::column(
              {
                  .out = &m_textColumn,
                  .align = FlexAlign::Start,
                  .gap = Style::spaceXs * scale,
                  .flexGrow = 1.0f,
              },
              ui::label({
                  .out = &m_title,
                  .fontSize = Style::fontSizeBody * scale,
                  .maxLines = 1,
                  .fontWeight = FontWeight::Bold,
                  .configure = [](Label& label) { label.setHitTestVisible(false); },
              }),
              ui::label({
                  .out = &m_meta,
                  .fontSize = Style::fontSizeCaption * scale,
                  .maxLines = 1,
                  .configure = [](Label& label) {
                    label.setCaptionStyle();
                    label.setHitTestVisible(false);
                  },
              })
          )
      );
    }

    ~ClipboardListRow() override { releaseThumbnail(); }

    void setThumbnailService(ThumbnailService* thumbnails) {
      if (m_thumbnails == thumbnails) {
        return;
      }
      releaseThumbnail();
      m_thumbnails = thumbnails;
    }

    void bind(
        Renderer& renderer, const ClipboardEntry& entry, std::size_t historyIndex, float width, bool selected,
        bool hovered
    ) {
      m_historyIndex = historyIndex;
      m_selected = selected;
      m_hovered = hovered;
      m_isImage = entry.isImage();
      setVisible(true);
      setEnabled(true);
      setSize(width, kRowHeight * m_scale);

      const std::string nextThumbPath = m_isImage ? entry.payloadPath : std::string();
      if (m_thumbnailPath != nextThumbPath) {
        if (m_image != nullptr) {
          m_image->clear(renderer);
          m_image->setVisible(false);
        }
        releaseThumbnail();
        m_thumbnailPath = nextThumbPath;
        if (!m_thumbnailPath.empty() && m_thumbnails != nullptr) {
          (void)m_thumbnails->acquire(m_thumbnailPath);
        }
      }

      const std::string rawTitle = entryTitle(entry);
      m_title->setText(m_isImage ? rawTitle : collapseWhitespace(rawTitle));
      m_meta->setText(formatTimeAgo(entry.capturedAt) + "  •  " + formatBytes(entry.byteSize));

      if (m_glyph != nullptr) {
        m_glyph->setGlyph(m_isImage ? "photo" : "file-text");
      }
      refreshThumbnail(renderer);
      applyVisualState();
      layout(renderer);
    }

    void refreshThumbnail(Renderer& renderer) {
      if (m_image == nullptr || m_glyph == nullptr) {
        return;
      }
      if (!m_isImage || m_thumbnailPath.empty() || m_thumbnails == nullptr) {
        m_image->clear(renderer);
        m_image->setVisible(false);
        m_glyph->setVisible(true);
        return;
      }

      const TextureHandle handle = m_thumbnails->peek(m_thumbnailPath);
      if (handle.id == 0) {
        m_image->clear(renderer);
        m_image->setVisible(false);
        m_glyph->setVisible(true);
        return;
      }

      m_image->setExternalTexture(renderer, handle);
      m_image->setVisible(true);
      m_glyph->setVisible(false);
    }

  private:
    void doLayout(Renderer& renderer) override {
      const float thumbPx = kListThumbSize * m_scale;
      const float rowW = width();
      const float rowH = height();
      if (m_background != nullptr) {
        m_background->setPosition(0.0f, 0.0f);
        m_background->setSize(rowW, rowH);
      }
      if (m_row != nullptr) {
        m_row->setPosition(0.0f, 0.0f);
        m_row->setSize(rowW, rowH);
      }
      if (m_lead != nullptr) {
        m_lead->setSize(thumbPx, thumbPx);
        m_lead->setMinWidth(thumbPx);
        m_lead->setMinHeight(thumbPx);
      }
      if (m_image != nullptr) {
        m_image->setSize(thumbPx, thumbPx);
      }
      if (m_title != nullptr && m_meta != nullptr) {
        const float textWidth =
            std::max(0.0f, rowW - thumbPx - Style::spaceMd * m_scale - Style::spaceSm * m_scale * 2.0f);
        m_title->setMaxWidth(textWidth);
        m_meta->setMaxWidth(textWidth);
      }

      InputArea::doLayout(renderer);
    }

    void releaseThumbnail() {
      if (!m_thumbnailPath.empty() && m_thumbnails != nullptr) {
        m_thumbnails->release(m_thumbnailPath);
      }
      m_thumbnailPath.clear();
    }

    void applyVisualState() {
      if (m_background == nullptr || m_glyph == nullptr || m_title == nullptr || m_meta == nullptr) {
        return;
      }

      const Color bg = m_selected  ? colorForRole(ColorRole::SurfaceVariant)
                       : m_hovered ? colorForRole(ColorRole::SurfaceVariant, 0.45f)
                                   : clearColor();
      m_background->setFill(bg);
      m_glyph->setColor(colorSpecFromRole(m_isImage ? ColorRole::Secondary : ColorRole::Primary));
      m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_meta->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    }

    float m_scale = 1.0f;
    ThumbnailService* m_thumbnails = nullptr;
    Box* m_background = nullptr;
    Flex* m_row = nullptr;
    Flex* m_lead = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Flex* m_textColumn = nullptr;
    Label* m_title = nullptr;
    Label* m_meta = nullptr;
    std::size_t m_historyIndex = static_cast<std::size_t>(-1);
    bool m_selected = false;
    bool m_hovered = false;
    bool m_isImage = false;
    std::string m_thumbnailPath;
  };

} // namespace

class ClipboardListAdapter final : public VirtualGridAdapter {
public:
  ClipboardListAdapter(float scale, ClipboardService* clipboard, ThumbnailService* thumbnails)
      : m_scale(scale), m_clipboard(clipboard), m_thumbnails(thumbnails) {}

  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setFilteredIndices(const std::vector<std::size_t>* indices) { m_filteredIndices = indices; }
  void setThumbnailService(ThumbnailService* thumbnails) {
    m_thumbnails = thumbnails;
    for (ClipboardListRow* row : m_pool) {
      if (row != nullptr) {
        row->setThumbnailService(thumbnails);
      }
    }
  }
  void setOnActivate(std::function<void(std::size_t)> callback) { m_onActivate = std::move(callback); }

  void refreshVisibleThumbnails(Renderer& renderer) {
    for (ClipboardListRow* row : m_pool) {
      if (row != nullptr && row->visible()) {
        row->refreshThumbnail(renderer);
      }
    }
  }

  [[nodiscard]] std::size_t itemCount() const override {
    return m_filteredIndices == nullptr ? 0 : m_filteredIndices->size();
  }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    auto row = std::make_unique<ClipboardListRow>(m_scale, m_thumbnails);
    m_pool.push_back(row.get());
    return row;
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_clipboard == nullptr || m_filteredIndices == nullptr ||
        index >= m_filteredIndices->size()) {
      return;
    }
    const std::size_t historyIndex = (*m_filteredIndices)[index];
    const auto& history = m_clipboard->history();
    if (historyIndex >= history.size()) {
      return;
    }
    auto* row = static_cast<ClipboardListRow*>(&tile);
    row->bind(*m_renderer, history[historyIndex], historyIndex, row->width(), selected, hovered && !selected);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

private:
  float m_scale = 1.0f;
  ClipboardService* m_clipboard = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<std::size_t>* m_filteredIndices = nullptr;
  std::vector<ClipboardListRow*> m_pool;
  std::function<void(std::size_t)> m_onActivate;
};

ClipboardPanel::ClipboardPanel(
    ClipboardService* clipboard, ConfigService* config, ThumbnailService* thumbnails, AsyncTextureCache* asyncTextures
)
    : m_clipboard(clipboard), m_config(config), m_thumbnails(thumbnails), m_asyncTextures(asyncTextures) {}

ClipboardPanel::~ClipboardPanel() = default;

PanelPlacement ClipboardPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.clipboardPlacement : PanelPlacement::Centered;
}

void ClipboardPanel::setActivateCallback(std::function<void(const ClipboardEntry&)> callback) {
  m_activateCallback = std::move(callback);
}

void ClipboardPanel::create() {
  const float scale = contentScale();
  auto rootLayout = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  auto focusArea = std::make_unique<InputArea>();
  focusArea->setFocusable(true);
  focusArea->setVisible(false);
  focusArea->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (key.pressed) {
      handleKeyEvent(key.sym, key.modifiers);
    }
  });
  m_focusArea = static_cast<InputArea*>(rootLayout->addChild(std::move(focusArea)));

  auto sidebar = ui::column({
      .out = &m_sidebar,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceSm * scale,
  });

  auto sidebarHeader = ui::row(
      {
          .out = &m_sidebarHeaderRow,
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .gap = Style::spaceSm * scale,
      },
      ui::label({
          .out = &m_sidebarTitle,
          .text = i18n::tr("clipboard.title"),
          .fontSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
          .fontWeight = FontWeight::Bold,
      }),
      makeCompactIconButton(&m_clearHistoryButton, "trash", ButtonVariant::Destructive, scale, [this]() {
        if (m_clipboard != nullptr) {
          m_clipboard->clearHistory();
        }
      })
  );
  sidebar->addChild(std::move(sidebarHeader));

  sidebar->addChild(
      ui::input({
          .out = &m_filterInput,
          .placeholder = i18n::tr("clipboard.filter-placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceMd * scale,
          .clearButtonEnabled = true,
          .onChange = [this](const std::string& text) { onFilterChanged(text); },
          .onSubmit = [this](const std::string& /*text*/) { activateSelected(); },
          .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
      })
  );

  m_listAdapter = std::make_unique<ClipboardListAdapter>(scale, m_clipboard, m_thumbnails);
  m_listAdapter->setFilteredIndices(&m_filteredIndices);
  m_listAdapter->setOnActivate([this](std::size_t index) {
    if (m_selectedIndex == index) {
      activateSelected();
      return;
    }
    selectIndex(index);
  });

  sidebar->addChild(
      ui::virtualGridView({
          .out = &m_listGrid,
          .columns = 1,
          .cellHeight = kRowHeight * scale,
          .squareCells = false,
          .columnGap = 0.0f,
          .rowGap = Style::spaceXs * scale,
          .overscanRows = kListOverscanRows,
          .scrollbarVisible = true,
          .adapter = m_listAdapter.get(),
          .flexGrow = 1.0f,
      })
  );

  sidebar->addChild(
      ui::label({
          .out = &m_listEmptyLabel,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Label& label) { label.setCaptionStyle(); },
      })
  );

  rootLayout->addChild(std::move(sidebar));

  auto preview = ui::column({
      .out = &m_previewCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceSm * scale,
      .flexGrow = 1.0f,
  });

  auto previewActions = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      makeCompactIconButton(&m_copyButton, "copy", ButtonVariant::Default, scale, [this]() { activateSelected(); }),
      makeCompactIconButton(
          &m_imageActionButton, "photo-edit", ButtonVariant::Default, scale, [this]() { runImageAction(); }, false,
          false
      ),
      makeCompactIconButton(&m_pinButton, "pin", ButtonVariant::Default, scale, [this]() { togglePinSelected(); }),
      makeCompactIconButton(
          &m_deleteEntryButton, "trash", ButtonVariant::Destructive, scale, [this]() { deleteSelectedEntry(); }
      ),
      ui::button({
          .out = &m_closeButton,
          .glyph = "close",
          .glyphSize = Style::fontSizeBody * scale,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = []() { PanelManager::instance().close(); },
          // Preview header icon style.
          .configure = [scale, opacity = panelCardOpacity()](
                           Button& button
                       ) { panel_button_style::applyHeaderButtonStyle(button, opacity); },
      })
  );

  auto previewHeader = ui::row(
      {
          .out = &m_previewHeaderRow,
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .gap = Style::spaceSm * scale,
      },
      ui::label({
          .out = &m_previewTitle,
          .text = i18n::tr("clipboard.entry.title"),
          .fontSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
          .fontWeight = FontWeight::Bold,
          .flexGrow = 1.0f,
      }),
      std::move(previewActions)
  );
  preview->addChild(std::move(previewHeader));

  preview->addChild(
      ui::label({
          .out = &m_previewMeta,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .configure = [](Label& label) { label.setCaptionStyle(); },
      })
  );

  auto previewScroll = ui::scrollView({
      .out = &m_previewScrollView,
      .scrollbarVisible = true,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](ScrollView& scrollView) {
        scrollView.setCardStyle(scale, opacity, borders);
      },
  });
  m_previewContent = previewScroll->content();
  m_previewContent->setDirection(FlexDirection::Vertical);
  m_previewContent->setAlign(FlexAlign::Start);
  m_previewContent->setGap(Style::spaceSm * scale);
  preview->addChild(std::move(previewScroll));

  rootLayout->addChild(std::move(preview));

  setRoot(std::move(rootLayout));
  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  if (m_thumbnails != nullptr) {
    m_thumbnailPendingSub = m_thumbnails->subscribePendingUpload([this]() {
      m_thumbnailRefreshPending = true;
      PanelManager::instance().requestUpdateOnly();
    });
  }

  schedulePreviewPayloadRefresh(false);
}

void ClipboardPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr || m_sidebar == nullptr || m_previewCard == nullptr || m_listGrid == nullptr ||
      m_previewScrollView == nullptr) {
    return;
  }

  m_lastWidth = width;
  m_lastHeight = height;

  const float sidebarWidth = std::min(kSidebarWidth, std::max(220.0f, width * 0.34f));
  m_sidebar->setSize(sidebarWidth, 0.0f);

  m_focusArea->setPosition(0.0f, 0.0f);
  m_focusArea->setSize(1.0f, 1.0f);

  if (m_listAdapter != nullptr) {
    m_listAdapter->setRenderer(&renderer);
  }

  // Flex layout handles all sizing: sidebar title is measured automatically,
  // listGrid fills remaining sidebar height (flexGrow), preview fills
  // remaining root width (flexGrow), previewScroll fills remaining preview
  // height (flexGrow). Stretch alignment propagates cross-axis sizes.
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  bool relayoutNeeded = false;
  const float previewScrollH = m_previewScrollView->height();
  if (m_lastPreviewWidth != m_previewScrollView->contentViewportWidth() || m_lastPreviewHeight != previewScrollH) {
    rebuildPreview(renderer, m_previewScrollView->contentViewportWidth(), previewScrollH);
    relayoutNeeded = true;
  }

  if (relayoutNeeded) {
    m_rootLayout->layout(renderer);
  }

  if (m_thumbnails != nullptr) {
    const bool changed = m_thumbnails->uploadPending(renderer.textureManager());
    if (changed) {
      m_thumbnailRefreshPending = false;
      if (m_listAdapter != nullptr) {
        m_listAdapter->setRenderer(&renderer);
        m_listAdapter->refreshVisibleThumbnails(renderer);
      }
    }
  }

  if (m_pendingScrollToSelected) {
    scrollToSelected();
    m_pendingScrollToSelected = false;
  }
}

void ClipboardPanel::doUpdate(Renderer& renderer) {
  if (m_thumbnailRefreshPending && m_thumbnails != nullptr) {
    const bool changed = m_thumbnails->uploadPending(renderer.textureManager());
    m_thumbnailRefreshPending = false;
    if (changed && m_listAdapter != nullptr) {
      m_listAdapter->setRenderer(&renderer);
      m_listAdapter->refreshVisibleThumbnails(renderer);
    }
  }

  updatePreviewActions();

  if (m_clipboard == nullptr || m_lastWidth <= 0.0f) {
    return;
  }

  if (m_lastChangeSerial != m_clipboard->changeSerial()) {
    applyFilter();
    if (m_filteredIndices.empty()) {
      m_selectedIndex = 0;
    } else if (m_selectedIndex >= m_filteredIndices.size()) {
      m_selectedIndex = m_filteredIndices.size() - 1;
    }

    m_lastChangeSerial = m_clipboard->changeSerial();
    updateListState();
    if (m_listGrid != nullptr) {
      m_listGrid->notifyDataChanged();
      m_listGrid->setSelectedIndex(
          m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
      );
    }

    schedulePreviewPayloadRefresh(false);
    const float previewWidth =
        m_previewScrollView != nullptr ? m_previewScrollView->contentViewportWidth() : m_lastWidth;
    const float previewHeight = m_previewScrollView != nullptr ? m_previewScrollView->height() : m_lastHeight;
    rebuildPreview(renderer, previewWidth, previewHeight);
  }
}

void ClipboardPanel::onOpen(std::string_view /*context*/) {
  m_selectedIndex = 0;
  m_previewPayloadIndex = static_cast<std::size_t>(-1);
  m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
  m_previewPayloadDebounceTimer.stop();
  m_lastPreviewWidth = -1.0f;
  m_lastPreviewHeight = -1.0f;
  m_pendingScrollToSelected = false;
  m_filterQuery.clear();
  m_pendingFilterQuery.clear();
  m_filterDebounceTimer.stop();
  if (m_filterInput != nullptr) {
    m_filterInput->setValue("");
  }
  applyFilter();
  updateListState();
  if (m_listGrid != nullptr) {
    m_listGrid->notifyDataChanged();
    m_listGrid->setSelectedIndex(
        m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
    );
    m_listGrid->scrollView().setScrollOffset(0.0f);
  }
  m_lastChangeSerial = m_clipboard != nullptr ? m_clipboard->changeSerial() : 0;
  schedulePreviewPayloadRefresh(false);
}

void ClipboardPanel::onClose() {
  m_thumbnailPendingSub.disconnect();
  if (m_listGrid != nullptr) {
    m_listGrid->setAdapter(nullptr);
  }
  m_listAdapter.reset();
  m_rootLayout = nullptr;
  m_focusArea = nullptr;
  m_sidebar = nullptr;
  m_sidebarHeaderRow = nullptr;
  m_sidebarTitle = nullptr;
  m_clearHistoryButton = nullptr;
  m_closeButton = nullptr;
  m_filterInput = nullptr;
  m_listGrid = nullptr;
  m_listEmptyLabel = nullptr;
  m_filteredIndices.clear();
  m_previewCard = nullptr;
  m_previewHeaderRow = nullptr;
  m_previewTitle = nullptr;
  m_previewMeta = nullptr;
  m_imageActionButton = nullptr;
  m_pinButton = nullptr;
  m_copyButton = nullptr;
  m_deleteEntryButton = nullptr;
  m_previewScrollView = nullptr;
  m_previewContent = nullptr;
  m_previewImage = nullptr;
  m_previewPayloadDebounceTimer.stop();
  m_filterDebounceTimer.stop();
  m_pendingFilterQuery.clear();
  m_filterQuery.clear();
  clearReleasedRoot();
  m_lastWidth = 0.0f;
  m_lastHeight = 0.0f;
  m_pendingScrollToSelected = false;
  m_thumbnailRefreshPending = false;

  if (m_clipboard != nullptr) {
    m_clipboard->evictAllPayloads();
  }
  if (m_asyncTextures != nullptr) {
    DeferredCall::callLater([asyncTextures = m_asyncTextures]() { asyncTextures->trimUnused(0); });
  }
}

InputArea* ClipboardPanel::initialFocusArea() const {
  return m_filterInput != nullptr ? m_filterInput->inputArea() : m_focusArea;
}

void ClipboardPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_closeButton != nullptr) {
    panel_button_style::applyHeaderButtonStyle(*m_closeButton, opacity);
  }
  if (m_previewScrollView != nullptr) {
    m_previewScrollView->setCardStyle(contentScale(), opacity, panelBordersEnabled());
  }
}

void ClipboardPanel::schedulePreviewPayloadRefresh(bool debounced) {
  const std::size_t historyIndex = selectedHistoryIndex();
  if (m_clipboard == nullptr || historyIndex == static_cast<std::size_t>(-1)) {
    m_previewPayloadDebounceTimer.stop();
    m_previewPayloadIndex = static_cast<std::size_t>(-1);
    m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
    m_lastPreviewWidth = -1.0f;
    m_lastPreviewHeight = -1.0f;
    return;
  }

  if (!debounced || historyIndex == m_previewPayloadIndex) {
    m_previewPayloadDebounceTimer.stop();
    m_previewPayloadIndex = historyIndex;
    m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
    m_lastPreviewWidth = -1.0f;
    m_lastPreviewHeight = -1.0f;
    return;
  }

  m_pendingPreviewPayloadIndex = historyIndex;
  m_lastPreviewWidth = -1.0f;
  m_lastPreviewHeight = -1.0f;
  m_previewPayloadDebounceTimer.start(kPreviewPayloadDebounceInterval, [this]() {
    if (m_pendingPreviewPayloadIndex == static_cast<std::size_t>(-1)) {
      return;
    }
    m_previewPayloadIndex = m_pendingPreviewPayloadIndex;
    m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
    m_lastPreviewWidth = -1.0f;
    m_lastPreviewHeight = -1.0f;
    PanelManager::instance().refresh();
  });
}

void ClipboardPanel::updateListState() {
  const auto& history = m_clipboard != nullptr ? m_clipboard->history() : std::deque<ClipboardEntry>{};
  const bool empty = history.empty() || m_filteredIndices.empty();

  if (m_listEmptyLabel != nullptr) {
    m_listEmptyLabel->setText(
        history.empty()         ? i18n::tr("clipboard.empty.history-title")
        : m_filterQuery.empty() ? i18n::tr("clipboard.empty.history-title")
                                : i18n::tr("clipboard.empty.no-matches-title")
    );
    m_listEmptyLabel->setVisible(empty);
    m_listEmptyLabel->setParticipatesInLayout(empty);
  }
  if (m_listGrid != nullptr) {
    m_listGrid->setVisible(!empty);
    m_listGrid->setParticipatesInLayout(!empty);
  }
}

void ClipboardPanel::updatePreviewActions() {
  if (m_imageActionButton == nullptr) {
    return;
  }

  bool showImageAction = false;
  if (m_clipboard != nullptr && m_config != nullptr &&
      !StringUtils::trim(m_config->config().shell.clipboardImageActionCommand).empty()) {
    const std::size_t historyIndex = selectedHistoryIndex();
    const auto& history = m_clipboard->history();
    showImageAction = historyIndex != static_cast<std::size_t>(-1) && historyIndex < history.size() &&
                      history[historyIndex].isImage();
  }

  m_imageActionButton->setVisible(showImageAction);
  m_imageActionButton->setParticipatesInLayout(showImageAction);
  m_imageActionButton->setEnabled(showImageAction);

  updatePinButton();
}

void ClipboardPanel::updatePinButton() {
  if (m_pinButton == nullptr) {
    return;
  }

  bool hasSelection = false;
  bool pinned = false;
  if (m_clipboard != nullptr) {
    const std::size_t historyIndex = selectedHistoryIndex();
    const auto& history = m_clipboard->history();
    if (historyIndex != static_cast<std::size_t>(-1) && historyIndex < history.size()) {
      hasSelection = true;
      pinned = history[historyIndex].pinned;
    }
  }

  m_pinButton->setVisible(hasSelection);
  m_pinButton->setParticipatesInLayout(hasSelection);
  m_pinButton->setEnabled(hasSelection);
  m_pinButton->setGlyph(pinned ? "unpin" : "pin");
  m_pinButton->setVariant(pinned ? ButtonVariant::Primary : ButtonVariant::Default);
}

void ClipboardPanel::rebuildPreview(Renderer& renderer, float width, float height) {
  uiAssertNotRendering("ClipboardPanel::rebuildPreview");
  if (m_previewContent == nullptr || m_previewTitle == nullptr || m_previewMeta == nullptr) {
    return;
  }

  updatePreviewActions();

  while (!m_previewContent->children().empty()) {
    m_previewContent->removeChild(m_previewContent->children().front().get());
  }
  m_previewImage = nullptr;

  const auto& history = m_clipboard != nullptr ? m_clipboard->history() : std::deque<ClipboardEntry>{};
  const std::size_t historyIndex = selectedHistoryIndex();
  if (history.empty() || historyIndex == static_cast<std::size_t>(-1)) {
    m_previewTitle->setText(i18n::tr("clipboard.entry.title"));
    m_previewMeta->setText("");

    m_previewContent->addChild(
        ui::label({
            .text = history.empty() ? i18n::tr("clipboard.empty.history-message")
                                    : i18n::tr("clipboard.empty.no-matches-message"),
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxWidth = width,
        })
    );
    m_lastPreviewWidth = width;
    m_lastPreviewHeight = height;
    return;
  }

  const auto& entry = history[historyIndex];
  m_previewTitle->setText(previewTitle(entry));
  m_previewTitle->setMaxWidth(width);
  m_previewMeta->setText(formatTimeAgo(entry.capturedAt) + "  •  " + formatBytes(entry.byteSize));
  m_previewMeta->setMaxWidth(width);

  if (m_previewPayloadIndex != historyIndex) {
    m_previewContent->addChild(
        ui::label({
            .text = i18n::tr("clipboard.preview.loading"),
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxWidth = width,
        })
    );
    m_lastPreviewWidth = width;
    m_lastPreviewHeight = height;
    return;
  }

  if (m_clipboard != nullptr && m_previewPayloadIndex != static_cast<std::size_t>(-1) &&
      m_previewPayloadIndex != historyIndex) {
    m_clipboard->evictEntryPayload(m_previewPayloadIndex);
  }

  if (entry.isImage()) {
    const float imageHeight = std::min(kPreviewImageHeight, std::max(180.0f, height - Style::spaceMd));
    auto image = ui::image({
        .fit = ImageFit::Contain,
        .width = width,
        .height = imageHeight,
    });
    const int previewTargetSize = static_cast<int>(std::ceil(std::max(width, imageHeight)));
    image->setAsyncReadyCallback([]() { PanelManager::instance().refresh(); });
    if (m_asyncTextures != nullptr && !entry.payloadPath.empty()) {
      (void)image->setSourceFileAsync(renderer, *m_asyncTextures, entry.payloadPath, previewTargetSize);
    }
    m_previewImage = image.get();
    m_previewContent->addChild(std::move(image));
  } else {
    if (m_clipboard != nullptr) {
      (void)m_clipboard->ensureEntryLoaded(historyIndex);
    }
    const auto& loadedEntry = m_clipboard != nullptr ? m_clipboard->history()[historyIndex] : entry;
    constexpr std::size_t kMaxPreviewChars = 8000;
    constexpr int kMaxPreviewLines = 200;

    std::string text(loadedEntry.data.begin(), loadedEntry.data.end());
    const bool truncated = text.size() > kMaxPreviewChars;
    if (truncated) {
      text.resize(kMaxPreviewChars);
    }

    // Expand tabs to 4 spaces once up front; Pango's natural wrapping then
    // handles everything else — newlines become paragraph breaks, each
    // paragraph's leading whitespace stays on its first line, continuations
    // have no indent, and the whole layout ellipsizes at kMaxPreviewLines.
    std::string expanded;
    expanded.reserve(text.size());
    for (char ch : text) {
      if (ch == '\t') {
        expanded.append("    ");
      } else {
        expanded.push_back(ch);
      }
    }

    if (expanded.empty()) {
      m_previewContent->addChild(
          ui::label({
              .text = i18n::tr("clipboard.preview.empty-text-payload"),
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    } else {
      m_previewContent->addChild(
          ui::label({
              .text = expanded,
              .fontSize = Style::fontSizeBody,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxWidth = width,
              .maxLines = kMaxPreviewLines,
          })
      );
      if (truncated) {
        m_previewContent->addChild(
            ui::label({
                .text = i18n::tr("clipboard.preview.truncated"),
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .configure = [](Label& label) { label.setCaptionStyle(); },
            })
        );
      }
    }
  }

  m_previewContent->layout(renderer);
  m_lastPreviewWidth = width;
  m_lastPreviewHeight = height;
}

std::size_t ClipboardPanel::selectedHistoryIndex() const {
  if (m_selectedIndex >= m_filteredIndices.size()) {
    return static_cast<std::size_t>(-1);
  }
  return m_filteredIndices[m_selectedIndex];
}

void ClipboardPanel::applyFilter() {
  m_filteredIndices.clear();
  if (m_clipboard == nullptr) {
    return;
  }
  const auto& history = m_clipboard->history();

  // Case-insensitive substring match on the entry title.
  std::string needle;
  needle.reserve(m_filterQuery.size());
  for (char ch : m_filterQuery) {
    needle.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  m_filteredIndices.reserve(history.size());
  for (std::size_t i = 0; i < history.size(); ++i) {
    if (needle.empty()) {
      m_filteredIndices.push_back(i);
      continue;
    }
    std::string haystack = entryTitle(history[i]);
    for (char& ch : haystack) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (haystack.find(needle) != std::string::npos) {
      m_filteredIndices.push_back(i);
    }
  }
}

void ClipboardPanel::onFilterChanged(const std::string& text) {
  if (text == m_pendingFilterQuery && text == m_filterQuery) {
    return;
  }
  m_pendingFilterQuery = text;

  auto commit = [this]() {
    if (m_pendingFilterQuery == m_filterQuery) {
      return;
    }
    m_filterQuery = m_pendingFilterQuery;
    applyFilter();
    m_selectedIndex = 0;
    updateListState();
    if (m_listGrid != nullptr) {
      m_listGrid->notifyDataChanged();
      m_listGrid->setSelectedIndex(
          m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
      );
    }
    schedulePreviewPayloadRefresh(true);
    m_pendingScrollToSelected = true;
    PanelManager::instance().refresh();
  };

  m_filterDebounceTimer.start(kFilterDebounceInterval, commit);
}

void ClipboardPanel::selectIndex(std::size_t index) {
  if (m_clipboard == nullptr || index >= m_filteredIndices.size()) {
    return;
  }
  if (m_selectedIndex == index) {
    return;
  }
  m_selectedIndex = index;
  if (m_listGrid != nullptr) {
    m_listGrid->setSelectedIndex(index);
  }
  schedulePreviewPayloadRefresh(true);
  m_pendingScrollToSelected = true;
  PanelManager::instance().refresh();
}

void ClipboardPanel::deleteSelectedEntry() {
  if (m_clipboard == nullptr) {
    return;
  }
  const std::size_t historyIndex = selectedHistoryIndex();
  if (historyIndex == static_cast<std::size_t>(-1)) {
    return;
  }
  const std::size_t filterPos = m_selectedIndex;
  if (!m_clipboard->removeHistoryEntry(historyIndex)) {
    return;
  }
  applyFilter();
  if (m_filteredIndices.empty()) {
    m_selectedIndex = 0;
  } else {
    m_selectedIndex = std::min(filterPos, m_filteredIndices.size() - 1);
  }
  updateListState();
  if (m_listGrid != nullptr) {
    m_listGrid->notifyDataChanged();
    m_listGrid->setSelectedIndex(
        m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
    );
  }
  schedulePreviewPayloadRefresh(false);
  m_pendingScrollToSelected = true;
  PanelManager::instance().refresh();
}

void ClipboardPanel::togglePinSelected() {
  if (m_clipboard == nullptr) {
    return;
  }
  const std::size_t historyIndex = selectedHistoryIndex();
  if (historyIndex == static_cast<std::size_t>(-1)) {
    return;
  }
  const auto& history = m_clipboard->history();
  if (historyIndex >= history.size()) {
    return;
  }

  const std::string storageId = history[historyIndex].storageId;
  const bool nextPinned = !history[historyIndex].pinned;
  if (!m_clipboard->setEntryPinned(historyIndex, nextPinned)) {
    return;
  }

  applyFilter();

  // The toggled entry moved within the deque; keep it selected by locating it
  // again via its stable storage id.
  std::size_t newSelected = 0;
  const auto& updated = m_clipboard->history();
  for (std::size_t pos = 0; pos < m_filteredIndices.size(); ++pos) {
    const std::size_t idx = m_filteredIndices[pos];
    if (idx < updated.size() && updated[idx].storageId == storageId) {
      newSelected = pos;
      break;
    }
  }
  m_selectedIndex = m_filteredIndices.empty() ? 0 : newSelected;

  updateListState();
  if (m_listGrid != nullptr) {
    m_listGrid->notifyDataChanged();
    m_listGrid->setSelectedIndex(
        m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
    );
  }
  schedulePreviewPayloadRefresh(false);
  m_pendingScrollToSelected = true;
  PanelManager::instance().refresh();
}

void ClipboardPanel::runImageAction() {
  if (m_clipboard == nullptr || m_config == nullptr) {
    return;
  }

  const std::string configuredCommand = StringUtils::trim(m_config->config().shell.clipboardImageActionCommand);
  if (configuredCommand.empty()) {
    return;
  }

  const std::size_t historyIndex = selectedHistoryIndex();
  if (historyIndex == static_cast<std::size_t>(-1)) {
    return;
  }

  const auto& history = m_clipboard->history();
  if (historyIndex >= history.size() || !history[historyIndex].isImage()) {
    return;
  }

  const std::optional<std::string> exportedPath = m_clipboard->exportEntryForExternalTool(historyIndex);
  if (!exportedPath.has_value()) {
    kLog.warn("clipboard image action failed: selected image could not be exported");
    return;
  }

  const std::string command = buildImageActionCommand(configuredCommand, *exportedPath);
  if (!process::runAsync(command)) {
    kLog.warn("clipboard image action failed to launch: {}", configuredCommand);
    return;
  }
  PanelManager::instance().close();
}

void ClipboardPanel::activateSelected() {
  if (m_clipboard == nullptr) {
    return;
  }
  const std::size_t historyIndex = selectedHistoryIndex();
  if (historyIndex == static_cast<std::size_t>(-1)) {
    return;
  }
  if (!m_clipboard->ensureEntryLoaded(historyIndex)) {
    return;
  }
  const ClipboardEntry entry = m_clipboard->history()[historyIndex];
  // Pinned entries already sit at the top; don't reorder them or jump the
  // selection back to the front when they are actioned — just copy.
  const bool wasPinned = entry.pinned;
  const bool promoted = wasPinned ? false : m_clipboard->promoteEntry(historyIndex);
  const bool copied = m_clipboard->copyEntry(entry);
  if (copied || promoted) {
    if (m_activateCallback) {
      m_activateCallback(entry);
      return;
    }
    if (!wasPinned) {
      m_selectedIndex = 0;
      applyFilter();
      updateListState();
      if (m_listGrid != nullptr) {
        m_listGrid->notifyDataChanged();
        m_listGrid->setSelectedIndex(
            m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
        );
      }
      schedulePreviewPayloadRefresh(false);
    }
    PanelManager::instance().refresh();
  }
}

bool ClipboardPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  if (m_clipboard == nullptr || m_filteredIndices.empty()) {
    return false;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    if (m_selectedIndex > 0) {
      selectIndex(m_selectedIndex - 1);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    if (m_selectedIndex + 1 < m_filteredIndices.size()) {
      selectIndex(m_selectedIndex + 1);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelected();
    return true;
  }

  return false;
}

void ClipboardPanel::scrollToSelected() {
  if (m_listGrid == nullptr || m_selectedIndex >= m_filteredIndices.size()) {
    return;
  }
  m_listGrid->scrollToIndex(m_selectedIndex);
}
