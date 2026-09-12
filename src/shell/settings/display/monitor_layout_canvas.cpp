#include "shell/settings/display/monitor_layout_canvas.h"

#include "render/scene/input_area.h"
#include "shell/settings/display/display_layout.h"
#include "shell/settings/display/display_service.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace settings::display {

  namespace {

    constexpr float kPadX = 60.0F;
    constexpr float kPadY = 40.0F;
    constexpr float kMaxPreviewScale = 0.3F;
    constexpr float kSnapThreshold = 5.0F;

  } // namespace

  MonitorLayoutCanvas::MonitorLayoutCanvas(
      DisplayService& display, std::string selectedOutput, std::function<void(std::string)> onSelect, float scale
  )
      : m_display(display), m_selectedOutput(std::move(selectedOutput)), m_onSelect(std::move(onSelect)),
        m_scale(scale) {
    const auto sizes = predictedSizes(m_display.outputs(), m_display.target());
    for (const auto& [name, cfg] : m_display.target()) {
      if (!cfg.enabled) {
        continue;
      }
      const auto sizeIt = sizes.find(name);
      m_rects.push_back(
          OutputRect{
              name,
              static_cast<float>(cfg.x),
              static_cast<float>(cfg.y),
              static_cast<float>(sizeIt != sizes.end() ? sizeIt->second.w : 0),
              static_cast<float>(sizeIt != sizes.end() ? sizeIt->second.h : 0),
          }
      );
    }

    for (const auto& rect : m_rects) {
      const bool selected = m_selectedOutput == rect.name;
      auto box = ui::column({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = 2.0F * m_scale,
          .padding = Style::spaceXs * m_scale,
          .configure = [this, selected](Flex& flex) {
            flex.setRadius(Style::radiusMd * m_scale);
            flex.setFill(
                selected ? colorSpecFromRole(ColorRole::Primary) : colorSpecFromRole(ColorRole::SurfaceVariant)
            );
            flex.setBorder(
                selected ? colorSpecFromRole(ColorRole::Primary) : colorSpecFromRole(ColorRole::Outline),
                selected ? 2.0F : 1.0F
            );
          },
      });
      box->setParticipatesInLayout(false);
      box->addChild(
          ui::label({
              .text = rect.name,
              .fontSize = Style::fontSizeCaption * m_scale,
              .fontWeight = FontWeight::Bold,
              .color = selected ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
          })
      );
      if (rect.width > 0.0F && rect.height > 0.0F) {
        box->addChild(
            ui::label({
                .text =
                    std::to_string(static_cast<int>(rect.width)) + "×" + std::to_string(static_cast<int>(rect.height)),
                .fontSize = Style::fontSizeCaption * m_scale,
                .color =
                    selected ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
            })
        );
      }
      m_boxes.emplace_back(rect.name, box.get());
      addChild(std::move(box));
    }

    auto guideX = ui::box({
        .fill = colorSpecFromRole(ColorRole::Secondary, 0.8F),
    });
    guideX->setParticipatesInLayout(false);
    m_guideX = guideX.get();
    addChild(std::move(guideX));
    auto guideY = ui::box({
        .fill = colorSpecFromRole(ColorRole::Secondary, 0.8F),
    });
    guideY->setParticipatesInLayout(false);
    m_guideY = guideY.get();
    addChild(std::move(guideY));

    auto input = ui::inputArea({
        .cursorShape = m_rects.size() > 1 ? 0U : 0U,
    });
    input->setOnPress([this](const InputArea::PointerData& data) {
      if (!data.pressed) {
        if (!m_dragging) {
          return;
        }
        m_dragging = false;
        const float dx = std::abs(data.localX - m_dragStartPressX);
        const float dy = std::abs(data.localY - m_dragStartPressY);
        if (dx < 3.0F && dy < 3.0F) {
          if (m_onSelect) {
            m_onSelect(m_dragName);
          }
          return;
        }
        commitDrag();
        return;
      }
      if (m_rects.size() <= 1) {
        return;
      }
      for (const auto& rect : m_previewRects) {
        if (data.localX < rect.x
            || data.localX > rect.x + rect.width
            || data.localY < rect.y
            || data.localY > rect.y + rect.height) {
          continue;
        }
        m_dragging = true;
        m_dragName = rect.name;
        m_dragStartLocalX = data.localX;
        m_dragStartLocalY = data.localY;
        m_dragStartRectX = rect.x;
        m_dragStartRectY = rect.y;
        m_dragStartPressX = data.localX;
        m_dragStartPressY = data.localY;
        break;
      }
    });
    input->setOnMotion([this](const InputArea::PointerData& data) {
      if (!m_dragging) {
        return;
      }
      auto rectIt =
          std::ranges::find_if(m_previewRects, [this](const OutputRect& rect) { return rect.name == m_dragName; });
      if (rectIt == m_previewRects.end()) {
        return;
      }
      float newX = m_dragStartRectX + (data.localX - m_dragStartLocalX);
      float newY = m_dragStartRectY + (data.localY - m_dragStartLocalY);
      float guideX = -1.0F;
      float guideY = -1.0F;
      computeSnap(m_dragName, newX, newY, rectIt->width, rectIt->height, newX, newY, guideX, guideY);
      rectIt->x = newX;
      rectIt->y = newY;
      if (m_guideX != nullptr) {
        m_guideX->setPosition(guideX, 0.0F);
      }
      if (m_guideY != nullptr) {
        m_guideY->setPosition(0.0F, guideY);
      }
      for (const auto& [name, box] : m_boxes) {
        const auto it =
            std::ranges::find_if(m_previewRects, [&name](const OutputRect& rect) { return rect.name == name; });
        if (it != m_previewRects.end()) {
          box->setPosition(it->x, it->y);
        }
      }
      markPaintDirty();
    });
    input->setOnClick([this](const InputArea::PointerData& data) {
      if (m_rects.size() <= 1) {
        for (const auto& rect : m_previewRects) {
          if (data.localX >= rect.x
              && data.localX <= rect.x + rect.width
              && data.localY >= rect.y
              && data.localY <= rect.y + rect.height
              && m_onSelect) {
            m_onSelect(rect.name);
          }
        }
      }
    });
    input->setOnCancel([this]() {
      m_dragging = false;
      markPaintDirty();
    });
    m_input = input.get();
    addChild(std::move(input));
  }

  LayoutSize MonitorLayoutCanvas::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
    const float width = std::min(constraints.maxWidth, 980.0F * m_scale);
    return {.width = width, .height = 220.0F * m_scale};
  }

  void MonitorLayoutCanvas::doArrange(Renderer& renderer, const LayoutRect& rect) {
    m_canvasWidth = rect.width;
    m_canvasHeight = rect.height;
    const auto sizes = predictedSizes(m_display.outputs(), m_display.target());
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = -std::numeric_limits<float>::max();
    float maxY = -std::numeric_limits<float>::max();
    bool any = false;
    for (const auto& rectState : m_rects) {
      any = true;
      minX = std::min(minX, rectState.x);
      minY = std::min(minY, rectState.y);
      maxX = std::max(maxX, rectState.x + rectState.width);
      maxY = std::max(maxY, rectState.y + rectState.height);
    }
    if (!any || minX > maxX || minY > maxY) {
      minX = minY = 0.0F;
      maxX = maxY = 1.0F;
    }
    const float totalW = std::max(1.0F, maxX - minX);
    const float totalH = std::max(1.0F, maxY - minY);
    const float padX = kPadX * m_scale;
    const float padY = kPadY * m_scale;
    const float availW = std::max(1.0F, rect.width - padX * 2.0F);
    const float availH = std::max(1.0F, rect.height - padY * 2.0F);
    const float previewScale = std::min({availW / totalW, availH / totalH, kMaxPreviewScale});
    const float offsetX = (rect.width - totalW * previewScale) / 2.0F;
    const float offsetY = (rect.height - totalH * previewScale) / 2.0F;

    m_previewRects = m_rects;
    for (auto& preview : m_previewRects) {
      preview.x = offsetX + (preview.x - minX) * previewScale;
      preview.y = offsetY + (preview.y - minY) * previewScale;
      preview.width *= previewScale;
      preview.height *= previewScale;
    }
    for (const auto& [name, box] : m_boxes) {
      const auto it =
          std::ranges::find_if(m_previewRects, [&name](const OutputRect& rect) { return rect.name == name; });
      if (it == m_previewRects.end()) {
        continue;
      }
      box->setPosition(it->x, it->y);
      box->setSize(it->width, it->height);
      box->arrange(renderer, {.x = it->x, .y = it->y, .width = it->width, .height = it->height});
    }
    if (m_input != nullptr) {
      m_input->setPosition(0.0F, 0.0F);
      m_input->setSize(rect.width, rect.height);
    }
    if (m_guideX != nullptr) {
      m_guideX->setPosition(-2.0F, 0.0F);
      m_guideX->setSize(1.0F, rect.height);
    }
    if (m_guideY != nullptr) {
      m_guideY->setPosition(0.0F, -2.0F);
      m_guideY->setSize(rect.width, 1.0F);
    }
  }

  void MonitorLayoutCanvas::computeSnap(
      const std::string& draggedName, float newX, float newY, float previewW, float previewH, float& snappedX,
      float& snappedY, float& guideX, float& guideY
  ) const {
    float bestSnapX = -1.0F;
    float bestSnapY = -1.0F;
    float bestDX = kSnapThreshold;
    float bestDY = kSnapThreshold;
    guideX = -1.0F;
    guideY = -1.0F;

    const float dragLeft = newX;
    const float dragRight = newX + previewW;
    const float dragCenterX = newX + previewW / 2.0F;
    const float dragTop = newY;
    const float dragBottom = newY + previewH;
    const float dragCenterY = newY + previewH / 2.0F;

    for (const auto& rect : m_previewRects) {
      if (rect.name == draggedName) {
        continue;
      }
      const float otherLeft = rect.x;
      const float otherRight = rect.x + rect.width;
      const float otherCenterX = rect.x + rect.width / 2.0F;
      const float otherTop = rect.y;
      const float otherBottom = rect.y + rect.height;
      const float otherCenterY = rect.y + rect.height / 2.0F;

      struct Candidate {
        float distance;
        float snap;
        float guide;
      };
      const Candidate xCandidates[] = {
          {std::abs(dragLeft - otherLeft), otherLeft, otherLeft},
          {std::abs(dragRight - otherRight), otherRight - previewW, otherRight},
          {std::abs(dragLeft - otherRight), otherRight, otherRight},
          {std::abs(dragRight - otherLeft), otherLeft - previewW, otherLeft},
          {std::abs(dragCenterX - otherCenterX), otherCenterX - previewW / 2.0F, otherCenterX},
      };
      for (const auto& candidate : xCandidates) {
        if (candidate.distance < bestDX) {
          bestDX = candidate.distance;
          bestSnapX = candidate.snap;
          guideX = candidate.guide;
        }
      }
      const Candidate yCandidates[] = {
          {std::abs(dragTop - otherTop), otherTop, otherTop},
          {std::abs(dragBottom - otherBottom), otherBottom - previewH, otherBottom},
          {std::abs(dragTop - otherBottom), otherBottom, otherBottom},
          {std::abs(dragBottom - otherTop), otherTop - previewH, otherTop},
          {std::abs(dragCenterY - otherCenterY), otherCenterY - previewH / 2.0F, otherCenterY},
      };
      for (const auto& candidate : yCandidates) {
        if (candidate.distance < bestDY) {
          bestDY = candidate.distance;
          bestSnapY = candidate.snap;
          guideY = candidate.guide;
        }
      }
    }

    if (bestSnapX >= 0.0F) {
      snappedX = bestSnapX;
    }
    if (bestSnapY >= 0.0F) {
      snappedY = bestSnapY;
    }
  }

  void MonitorLayoutCanvas::commitDrag() {
    const auto sizes = predictedSizes(m_display.outputs(), m_display.target());
    auto draggedIt =
        std::ranges::find_if(m_previewRects, [this](const OutputRect& rect) { return rect.name == m_dragName; });
    if (draggedIt == m_previewRects.end()) {
      return;
    }
    // Recompute the preview transform from the current logical layout (unchanged
    // during the drag) and convert the released preview position back.
    const auto& target = m_display.target();
    float minX = 0.0F;
    float minY = 0.0F;
    float maxX = 0.0F;
    float maxY = 0.0F;
    bool first = true;
    for (const auto& [name, cfg] : target) {
      if (!cfg.enabled) {
        continue;
      }
      const auto sizeIt = sizes.find(name);
      const float w = static_cast<float>(sizeIt != sizes.end() ? sizeIt->second.w : 0);
      const float h = static_cast<float>(sizeIt != sizes.end() ? sizeIt->second.h : 0);
      if (first) {
        minX = static_cast<float>(cfg.x);
        minY = static_cast<float>(cfg.y);
        maxX = static_cast<float>(cfg.x) + w;
        maxY = static_cast<float>(cfg.y) + h;
        first = false;
        continue;
      }
      minX = std::min(minX, static_cast<float>(cfg.x));
      minY = std::min(minY, static_cast<float>(cfg.y));
      maxX = std::max(maxX, static_cast<float>(cfg.x) + w);
      maxY = std::max(maxY, static_cast<float>(cfg.y) + h);
    }
    const float totalW = std::max(1.0F, maxX - minX);
    const float totalH = std::max(1.0F, maxY - minY);
    const float padX = kPadX * m_scale;
    const float padY = kPadY * m_scale;
    const float availW = std::max(1.0F, m_canvasWidth - padX * 2.0F);
    const float availH = std::max(1.0F, m_canvasHeight - padY * 2.0F);
    const float previewScale = std::min({availW / totalW, availH / totalH, kMaxPreviewScale});
    const float offsetX = (m_canvasWidth - totalW * previewScale) / 2.0F;
    const float offsetY = (m_canvasHeight - totalH * previewScale) / 2.0F;

    const int logicalX = static_cast<int>(std::lround(minX + (draggedIt->x - offsetX) / previewScale));
    const int logicalY = static_cast<int>(std::lround(minY + (draggedIt->y - offsetY) / previewScale));
    m_display.setPosition(m_dragName, logicalX, logicalY);
    if (m_onSelect) {
      m_onSelect(m_dragName);
    }
  }

} // namespace settings::display
