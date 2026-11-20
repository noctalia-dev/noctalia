#include "filemanager/file_list_header.h"

#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/file_manager_settings.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/separator.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/file_view_metrics.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>

namespace filemanager {

  namespace {
    constexpr std::array kColumnSortFields{FileDialogSortField::Name,     FileDialogSortField::Size,
                                           FileDialogSortField::Modified, FileDialogSortField::Type,
                                           FileDialogSortField::Created,  FileDialogSortField::Accessed};
  } // namespace

  FileListHeaderController::FileListHeaderController(
      FileManagerSettings& settings, WaylandConnection& wayland, InputDispatcher& dispatcher, Callbacks callbacks
  )
      : m_settings(settings), m_wayland(wayland), m_dispatcher(dispatcher), m_callbacks(std::move(callbacks)) {}

  void FileListHeaderController::loadInitialState() {
    const auto viewSettings = m_settings.loadFileView();
    m_gridMode = viewSettings.gridMode;
    m_sortField = viewSettings.sortField;
    m_sortOrder = viewSettings.sortOrder;
    m_settings.loadListColumnWidths(m_preferredColumns);
    m_settings.loadListColumnLayout(m_columnLayout);
    m_listColumns = m_preferredColumns;
  }

  std::unique_ptr<Flex> FileListHeaderController::buildHeader() {
    auto fileHeader = std::make_unique<Flex>();
    m_header = fileHeader.get();
    fileHeader->setAlign(FlexAlign::Center);
    fileHeader->setGap(Style::spaceSm);
    fileHeader->setPadding(5.0F, Style::spaceSm);
    const auto styleHeader = [](Button& button, ButtonContentAlign alignment) {
      auto headerPalette = Button::defaultPalette(ButtonVariant::Ghost);
      headerPalette.normal = {clearColorSpec(), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurfaceVariant)};
      headerPalette.hover = {
          colorSpecFromRole(ColorRole::Hover, 0.35F), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface)
      };
      headerPalette.pressed = headerPalette.hover;
      headerPalette.selected =
          Button::ButtonStateColors{clearColorSpec(), clearColorSpec(), colorSpecFromRole(ColorRole::Primary)};
      button.setCustomPalette(std::move(headerPalette));
      button.setFontSize(Style::fontSizeCaption);
      button.setControlHeight(26.0F);
      button.setPadding(0.0F);
      button.setContentAlign(alignment);
    };
    for (auto column : kFileColumns) {
      const auto index = columnIndex(column);
      auto header = std::make_unique<Button>();
      header->setText(kFileColumnLabels[index]);
      header->setVariant(ButtonVariant::Ghost);
      header->setOnClick([this, index] { setSortField(kColumnSortFields[index]); });
      m_headers[index] = header.get();
      styleHeader(
          *header,
          column == FileColumn::Name || column == FileColumn::Type ? ButtonContentAlign::Start : ButtonContentAlign::End
      );
      header->setMinWidth(kMinimumFileColumns[column]);
      header->setMaxWidth(std::max(kMinimumFileColumns[column], m_listColumns[column]));
      fileHeader->addChild(std::move(header));
    }
    const auto makeColumnMarker = [](ColorSpec color) {
      auto node = std::make_unique<Separator>();
      node->setParticipatesInLayout(false);
      node->setHitTestVisible(false);
      node->setZIndex(1);
      node->setOrientation(SeparatorOrientation::VerticalRule);
      node->setThickness(2.0F);
      node->setGradientEdges(true);
      node->setColor(color);
      node->setSize(2.0F, 20.0F);
      return node;
    };
    for (auto& marker : m_markers) {
      auto node = makeColumnMarker(colorSpecFromRole(ColorRole::Primary, 0.58F));
      marker = node.get();
      fileHeader->addChild(std::move(node));
    }
    auto dropMarker = makeColumnMarker(colorSpecFromRole(ColorRole::Primary));
    m_dropMarker = dropMarker.get();
    dropMarker->setZIndex(2);
    dropMarker->setVisible(false);
    fileHeader->addChild(std::move(dropMarker));
    return fileHeader;
  }

  void FileListHeaderController::attachGrid(VirtualGridView* grid) {
    m_grid = grid;
  }

  void FileListHeaderController::detachUi() {
    m_resizingColumn.reset();
    m_hoverBoundary.reset();
    m_columnDrag.reset();
    m_header = nullptr;
    m_grid = nullptr;
    m_headers.fill(nullptr);
    m_markers.fill(nullptr);
    m_dropMarker = nullptr;
    m_leftGutter = -1.0F;
    m_rightGutter = -1.0F;
  }

  void FileListHeaderController::ensureViewScaleLoaded() {
    if (!m_viewScaleLoaded) {
      m_viewScaleLoaded = true;
      m_viewScale = m_settings.loadViewScale(m_viewScale);
    }
  }

  void FileListHeaderController::setSortField(FileDialogSortField field) {
    if (m_sortField == field) {
      toggleSortOrder();
      return;
    }
    m_sortField = field;
    if (m_callbacks.onViewStateChanged)
      m_callbacks.onViewStateChanged();
    if (m_callbacks.onSortChanged)
      m_callbacks.onSortChanged();
    syncSortControls();
  }

  void FileListHeaderController::toggleSortOrder() {
    m_sortOrder =
        m_sortOrder == FileDialogSortOrder::Ascending ? FileDialogSortOrder::Descending : FileDialogSortOrder::Ascending;
    if (m_callbacks.onViewStateChanged)
      m_callbacks.onViewStateChanged();
    if (m_callbacks.onSortChanged)
      m_callbacks.onSortChanged();
    syncSortControls();
  }

  void FileListHeaderController::syncSortControls() {
    for (auto column : kFileColumns) {
      const auto index = columnIndex(column);
      if (auto* header = m_headers[index]) {
        const bool selected = m_sortField == kColumnSortFields[index];
        auto text = std::string(kFileColumnLabels[index]);
        if (selected)
          text += m_sortOrder == FileDialogSortOrder::Ascending ? "  ↑" : "  ↓";
        header->setText(text);
        header->setSelected(selected);
      }
    }
  }

  void FileListHeaderController::setGridMode(bool gridMode) {
    if (m_gridMode == gridMode)
      return;
    cancelGesture();
    m_gridMode = gridMode;
    if (m_callbacks.onViewStateChanged)
      m_callbacks.onViewStateChanged();
    updateColumnHeaders();
    if (m_callbacks.onViewModeOrScaleChanged)
      m_callbacks.onViewModeOrScaleChanged();
  }

  void FileListHeaderController::adjustViewScale(float value) {
    if (!m_grid || gestureActive() || (m_callbacks.canAdjustScale && !m_callbacks.canAdjustScale()))
      return;
    const float scale = clampFileViewScale(value);
    if (scale == m_viewScale)
      return;
    m_viewScale = scale;
    if (m_callbacks.onViewModeOrScaleChanged)
      m_callbacks.onViewModeOrScaleChanged();
    if (!m_settings.saveViewScale(m_viewScale) && m_callbacks.onStatus)
      m_callbacks.onStatus("Unable to save file view zoom");
  }

  void FileListHeaderController::updateColumnHeaders() {
    if (!m_header)
      return;
    cancelGesture();
    m_visibleColumns = m_gridMode
        ? std::vector<FileColumn>(kDefaultFileColumns.begin(), kDefaultFileColumns.end())
        : m_columnLayout.displayed();
    for (auto column : kFileColumns) {
      const auto index = columnIndex(column);
      const bool visible = std::ranges::contains(m_visibleColumns, column);
      m_headers[index]->setVisible(visible);
      m_headers[index]->setParticipatesInLayout(visible);
      m_markers[index]->setVisible(visible);
    }
    for (auto column : m_visibleColumns)
      m_header->addChild(m_header->removeChild(m_headers[columnIndex(column)]));

    applyColumnWidths();
    if (!m_gridMode && m_grid)
      m_grid->notifyDataChanged();
    if (m_callbacks.onRequestLayout)
      m_callbacks.onRequestLayout();
  }

  void FileListHeaderController::persistColumnLayout() {
    if (!m_settings.saveListColumnLayout(m_columnLayout) && m_callbacks.onStatus)
      m_callbacks.onStatus("Unable to save file column layout");
  }

  bool FileListHeaderController::applyColumnWidths() {
    if (!m_header || m_visibleColumns.empty())
      return false;
    float leftGutter = 0.0F;
    float rightGutter = 0.0F;
    if (!m_gridMode && m_grid && m_grid->focusArea()->width() > 0.0F) {
      float headerX = 0.0F, headerY = 0.0F, rowX = 0.0F, rowY = 0.0F;
      Node::absolutePosition(m_header, headerX, headerY);
      Node::absolutePosition(m_grid->focusArea(), rowX, rowY);
      leftGutter = std::max(0.0F, rowX - headerX);
      rightGutter = std::max(0.0F, m_header->width() - m_grid->focusArea()->width() - leftGutter);
    }
    const bool paddingChanged =
        std::abs(leftGutter - m_leftGutter) > 0.5F || std::abs(rightGutter - m_rightGutter) > 0.5F;
    if (paddingChanged) {
      m_leftGutter = leftGutter;
      m_rightGutter = rightGutter;
      m_header->setPadding(5.0F, Style::spaceSm + rightGutter, 5.0F, Style::spaceSm + leftGutter);
    }
    const auto fitted =
        fitFileColumns(m_resizingColumn ? m_listColumns : m_preferredColumns, availableWidth(), m_visibleColumns);
    bool changed = false;
    bool textInsetChanged = false;
    for (auto column : m_visibleColumns) {
      auto* header = m_headers[columnIndex(column)];
      changed |= std::abs(fitted[column] - m_listColumns[column]) > 0.5F;
      header->setMinWidth(fitted[column]);
      header->setMaxWidth(fitted[column]);
      const bool name = column == FileColumn::Name;
      const float inset = name ? fileViewMetrics(m_viewScale).listIcon + Style::spaceSm
                               : fileColumnTextInset(column == m_visibleColumns.back(), Style::spaceSm);
      const bool onLeft = name != Style::rtl();
      const float left = onLeft ? inset : 0.0F;
      const float right = onLeft ? 0.0F : inset;
      if (header->paddingLeft() != left || header->paddingRight() != right) {
        header->setPadding(0.0F, right, 0.0F, left);
        textInsetChanged = true;
      }
    }
    m_listColumns = fitted;
    if (changed && m_grid && !m_gridMode)
      m_grid->notifyDataChanged();
    return changed || paddingChanged || textInsetChanged;
  }

  void FileListHeaderController::arrangeAfterWidthChange(Renderer& renderer) {
    if (!m_header)
      return;
    m_header->arrange(
        renderer,
        LayoutRect{
            .x = m_header->x(), .y = m_header->y(), .width = m_header->width(), .height = m_header->height()
        }
    );
  }

  void FileListHeaderController::positionColumnMarkers() {
    if (!m_header)
      return;
    const bool rtl = Style::rtl();
    for (auto column : m_visibleColumns) {
      auto* header = m_headers[columnIndex(column)];
      const bool trailing = column == m_visibleColumns.back();
      const float halfGap = trailing ? 0.0F : Style::spaceSm * 0.5F;
      const float edge = rtl ? header->x() - halfGap : header->x() + header->width() + halfGap;
      m_markers[columnIndex(column)]->setPosition(edge - 1.0F, (m_header->height() - 20.0F) * 0.5F);
    }
  }

  void FileListHeaderController::layoutVisibleMarkers(Renderer& renderer) {
    // Markers are positioned overlays: keep them out of Flex sizing, but still
    // lay out each Separator's internal gradient rectangles explicitly.
    for (auto* marker : m_markers)
      if (marker && marker->visible())
        marker->layout(renderer);
    if (m_dropMarker && m_dropMarker->visible())
      m_dropMarker->layout(renderer);
  }

  void FileListHeaderController::cancelGesture() {
    const auto resize = m_resizingColumn;
    m_resizingColumn.reset();
    if (resize && m_markers[columnIndex(*resize)])
      m_markers[columnIndex(*resize)]->setColor(colorSpecFromRole(ColorRole::Primary, 0.58F));
    if (m_hoverBoundary && m_markers[columnIndex(*m_hoverBoundary)])
      m_markers[columnIndex(*m_hoverBoundary)]->setColor(colorSpecFromRole(ColorRole::Primary, 0.58F));
    m_hoverBoundary.reset();
    if (m_columnDrag) {
      if (m_headers[columnIndex(m_columnDrag->column)])
        m_headers[columnIndex(m_columnDrag->column)]->setBorder(clearColorSpec(), 0.0F);
      if (m_dropMarker)
        m_dropMarker->setVisible(false);
      m_columnDrag.reset();
    }
    if (resize)
      applyColumnWidths();
    if (m_callbacks.onRequestLayout)
      m_callbacks.onRequestLayout();
  }

  bool FileListHeaderController::handlePointer(const PointerEvent& event, float x, float y) {
    if (m_gridMode
        || !m_header
        || m_visibleColumns.empty()
        || (m_callbacks.canHandlePointer && !m_callbacks.canHandlePointer()))
      return false;

    const auto pointerSerial = m_callbacks.pointerSerial ? m_callbacks.pointerSerial() : 0U;
    const bool leftButton = event.type == PointerEvent::Type::Button && event.button == BTN_LEFT;
    const bool motion = event.type == PointerEvent::Type::Motion;
    const bool leave = event.type == PointerEvent::Type::Leave;
    const bool rtl = Style::rtl();
    float hx = 0.0F, hy = 0.0F;
    Node::absolutePosition(m_header, hx, hy);
    const bool inHeader = !leave && y >= hy - 2.0F && y <= hy + m_header->height() + 2.0F;
    const float available = availableWidth();
    if (m_resizingColumn) {
      const auto column = *m_resizingColumn;
      if (leave) {
        cancelGesture();
        return true;
      }
      if (pointerSerial)
        m_wayland.setCursorShape(pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
      const bool flexibleName = m_preferredColumns[FileColumn::Name] <= 0.0F;
      if (motion) {
        const float delta = rtl ? m_dragX - x : x - m_dragX;
        m_listColumns =
            resizeFileColumnBoundary(m_columnStart, column, delta, available, m_visibleColumns, flexibleName);
        applyColumnWidths();
        if (m_grid)
          m_grid->notifyDataChanged();
        if (m_callbacks.onRequestLayout)
          m_callbacks.onRequestLayout();
        return true;
      }
      if (leftButton && !event.pressed) {
        if (available >= fileColumnTotal(kMinimumFileColumns, m_visibleColumns)
            && m_listColumns[column] != m_columnStart[column]) {
          const bool keepNameFlexible =
              flexibleName && column != FileColumn::Name && hasOptionalFileColumns(m_visibleColumns);
          for (auto visible : m_visibleColumns)
            if (!keepNameFlexible || visible != FileColumn::Name)
              m_preferredColumns[visible] = m_listColumns[visible];
          if (!m_settings.saveListColumnWidths(m_preferredColumns) && m_callbacks.onStatus)
            m_callbacks.onStatus("Unable to save file column widths");
        }
        cancelGesture();
        m_dispatcher.cancelPointerCapture();
        return true;
      }
      return false;
    }

    if (m_columnDrag) {
      auto& drag = *m_columnDrag;
      if (leave) {
        m_dispatcher.cancelPointerCapture();
        cancelGesture();
        return false;
      }
      if (leftButton && !event.pressed) {
        const auto column = drag.column;
        const auto destination = drag.destination;
        cancelGesture();
        if (!destination)
          return false; // A normal header click still goes through the button's sort action.
        if (inHeader && m_columnLayout.move(column, *destination)) {
          updateColumnHeaders();
          persistColumnLayout();
        }
        return true;
      }
      if (motion && (drag.destination || std::hypot(x - drag.x, y - drag.y) > 5.0F)) {
        if (!drag.destination) {
          m_dispatcher.cancelPointerCapture();
          m_dispatcher.pointerLeave();
          m_headers[columnIndex(drag.column)]->setBorder(colorSpecFromRole(ColorRole::Primary, 0.7F), 1.0F);
        }

        float edge = x - hx;
        std::size_t destination = 0;
        for (auto column : m_visibleColumns) {
          if (column == drag.column)
            continue;
          const auto* header = m_headers[columnIndex(column)];
          const float center = hx + header->x() + header->width() * 0.5F;
          if (rtl ? x > center : x < center) {
            edge = rtl ? header->x() + header->width() + Style::spaceSm * 0.5F : header->x() - Style::spaceSm * 0.5F;
            break;
          }
          edge = m_markers[columnIndex(column)]->x() + 1.0F;
          ++destination;
        }
        drag.destination = destination;
        m_dropMarker->setVisible(inHeader);
        m_dropMarker->setPosition(edge - 1.0F, (m_header->height() - 20.0F) * 0.5F);
        if (pointerSerial)
          m_wayland.setCursorShape(pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING);
        if (m_callbacks.onRequestLayout)
          m_callbacks.onRequestLayout();
        return true;
      }
      return false;
    }

    std::optional<FileColumn> boundary;
    if (inHeader && available >= fileColumnTotal(kMinimumFileColumns, m_visibleColumns))
      for (auto column : m_visibleColumns)
        if (std::abs(x - (hx + m_markers[columnIndex(column)]->x() + 1.0F)) <= 6.0F) {
          boundary = column;
          break;
        }
    if ((motion || leave || event.type == PointerEvent::Type::Enter) && m_hoverBoundary != boundary) {
      if (m_hoverBoundary)
        m_markers[columnIndex(*m_hoverBoundary)]->setColor(colorSpecFromRole(ColorRole::Primary, 0.58F));
      m_hoverBoundary = boundary;
      if (boundary)
        m_markers[columnIndex(*boundary)]->setColor(colorSpecFromRole(ColorRole::Primary, 0.82F));
      if (m_callbacks.onRequestRedraw)
        m_callbacks.onRequestRedraw();
    }
    if (boundary && leftButton && event.pressed) {
      m_dispatcher.cancelPointerCapture();
      m_resizingColumn = boundary;
      m_dragX = x;
      m_columnStart = m_listColumns;
      m_markers[columnIndex(*boundary)]->setColor(colorSpecFromRole(ColorRole::Primary));
      return true;
    }
    if (boundary && (motion || event.type == PointerEvent::Type::Enter)) {
      m_dispatcher.pointerLeave();
      if (pointerSerial)
        m_wayland.setCursorShape(pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
      return true;
    }
    if (leftButton && event.pressed && inHeader && m_visibleColumns.size() > 1)
      for (auto column : m_visibleColumns)
        if (m_headers[columnIndex(column)]->containsScenePoint(x, y)) {
          m_columnDrag = ColumnDrag{column, x, y, std::nullopt};
          break;
        }
    return false;
  }

  float FileListHeaderController::availableWidth() const {
    if (!m_header || m_visibleColumns.empty())
      return 0.0F;
    // The list's focus plane shares the resolved row width, including the scrollbar gutter.
    const float rowWidth = m_grid && !m_gridMode && m_grid->focusArea()->width() > 0.0F ? m_grid->focusArea()->width()
                                                                                        : m_header->width();
    const float spacing = static_cast<float>(m_visibleColumns.size() + 1) * Style::spaceSm;
    return std::max(0.0F, rowWidth - spacing);
  }

} // namespace filemanager
