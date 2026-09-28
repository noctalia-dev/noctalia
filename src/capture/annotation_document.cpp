#include "capture/annotation_document.h"

#include "capture/annotation_raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>

namespace capture {
  namespace {

    struct ToolName {
      AnnotationTool tool;
      std::string_view name;
    };

    constexpr std::array<ToolName, kAnnotationToolCount> kToolNames = {{
        {AnnotationTool::Move, "move"},
        {AnnotationTool::Brush, "brush"},
        {AnnotationTool::Highlighter, "highlighter"},
        {AnnotationTool::Line, "line"},
        {AnnotationTool::Arrow, "arrow"},
        {AnnotationTool::Rectangle, "rectangle"},
        {AnnotationTool::Circle, "circle"},
        {AnnotationTool::Text, "text"},
        {AnnotationTool::Numbering, "numbering"},
        {AnnotationTool::Blur, "blur"},
        {AnnotationTool::Eraser, "eraser"},
        {AnnotationTool::Crop, "crop"},
    }};

    [[nodiscard]] double distanceToSegment(double px, double py, double x0, double y0, double x1, double y1) {
      const double dx = x1 - x0;
      const double dy = y1 - y0;
      const double lengthSquared = (dx * dx) + (dy * dy);
      if (lengthSquared <= 0.0001) {
        return std::hypot(px - x0, py - y0);
      }
      const double t = std::clamp((((px - x0) * dx) + ((py - y0) * dy)) / lengthSquared, 0.0, 1.0);
      return std::hypot(px - (x0 + (t * dx)), py - (y0 + (t * dy)));
    }

    // Liang-Barsky segment/rect overlap.
    [[nodiscard]] bool segmentIntersectsRect(
        double x0, double y0, double x1, double y1, double left, double top, double right, double bottom
    ) {
      if ((x0 >= left && x0 <= right && y0 >= top && y0 <= bottom)
          || (x1 >= left && x1 <= right && y1 >= top && y1 <= bottom)) {
        return true;
      }

      const double dx = x1 - x0;
      const double dy = y1 - y0;
      double t0 = 0.0;
      double t1 = 1.0;

      if (std::abs(dx) < 0.0001) {
        if (x0 < left || x0 > right) {
          return false;
        }
      } else {
        double txMin = (left - x0) / dx;
        double txMax = (right - x0) / dx;
        if (txMin > txMax) {
          std::swap(txMin, txMax);
        }
        t0 = std::max(t0, txMin);
        t1 = std::min(t1, txMax);
        if (t0 > t1) {
          return false;
        }
      }

      if (std::abs(dy) < 0.0001) {
        if (y0 < top || y0 > bottom) {
          return false;
        }
      } else {
        double tyMin = (top - y0) / dy;
        double tyMax = (bottom - y0) / dy;
        if (tyMin > tyMax) {
          std::swap(tyMin, tyMax);
        }
        t0 = std::max(t0, tyMin);
        t1 = std::min(t1, tyMax);
        if (t0 > t1) {
          return false;
        }
      }

      return true;
    }

    [[nodiscard]] AnnotationRect shapeRect(const Annotation& annotation) {
      const AnnotationPoint start = annotation.points.front();
      const AnnotationPoint end = annotation.points.back();
      return AnnotationRect{
          .x = std::min(start.x, end.x),
          .y = std::min(start.y, end.y),
          .width = std::abs(end.x - start.x),
          .height = std::abs(end.y - start.y),
      };
    }

  } // namespace

  std::string_view annotationToolName(AnnotationTool tool) noexcept {
    for (const auto& entry : kToolNames) {
      if (entry.tool == tool) {
        return entry.name;
      }
    }
    return "brush";
  }

  std::optional<AnnotationTool> annotationToolFromName(std::string_view name) noexcept {
    for (const auto& entry : kToolNames) {
      if (entry.name == name) {
        return entry.tool;
      }
    }
    return std::nullopt;
  }

  bool annotationToolIsShape(AnnotationTool tool) noexcept {
    return tool == AnnotationTool::Line
        || tool == AnnotationTool::Arrow
        || tool == AnnotationTool::Rectangle
        || tool == AnnotationTool::Circle
        || tool == AnnotationTool::Blur;
  }

  bool annotationToolIsNonDrawing(AnnotationTool tool) noexcept {
    return tool == AnnotationTool::Move || tool == AnnotationTool::Eraser || tool == AnnotationTool::Crop;
  }

  double annotationToolDefaultWidth(AnnotationTool tool) noexcept {
    switch (tool) {
    case AnnotationTool::Highlighter:
      return 24.0;
    case AnnotationTool::Eraser:
      return 28.0;
    case AnnotationTool::Blur:
      return 32.0;
    case AnnotationTool::Text:
      return 24.0;
    case AnnotationTool::Numbering:
      return 32.0;
    case AnnotationTool::Move:
    case AnnotationTool::Crop:
    case AnnotationTool::Brush:
    case AnnotationTool::Line:
    case AnnotationTool::Arrow:
    case AnnotationTool::Rectangle:
    case AnnotationTool::Circle:
      break;
    }
    return 6.0;
  }

  void AnnotationDocument::setCrop(std::optional<AnnotationRect> crop) {
    m_crop = crop;
    ++m_generation;
  }

  void AnnotationDocument::beginInteraction() {
    m_undo.push_back(m_annotations);
    while (m_undo.size() > kHistoryLimit) {
      m_undo.pop_front();
    }
    m_redo.clear();
    m_interactionOpen = true;
  }

  void AnnotationDocument::discardInteraction() {
    m_interactionOpen = false;
    if (m_undo.empty()) {
      return;
    }
    m_annotations = std::move(m_undo.back());
    m_undo.pop_back();
    ++m_generation;
  }

  Annotation& AnnotationDocument::push(Annotation annotation) {
    m_annotations.push_back(std::move(annotation));
    ++m_generation;
    return m_annotations.back();
  }

  Annotation* AnnotationDocument::active() {
    if (!m_interactionOpen || m_annotations.empty()) {
      return nullptr;
    }
    return &m_annotations.back();
  }

  void AnnotationDocument::erase(std::size_t index) {
    if (index >= m_annotations.size()) {
      return;
    }
    m_annotations.erase(m_annotations.begin() + static_cast<std::ptrdiff_t>(index));
    ++m_generation;
  }

  std::size_t AnnotationDocument::eraseAlong(AnnotationPoint from, AnnotationPoint to, double radius) {
    std::size_t removed = 0;
    for (std::size_t i = m_annotations.size(); i > 0; --i) {
      if (annotationIntersectsSegment(m_annotations[i - 1], from, to, radius)) {
        m_annotations.erase(m_annotations.begin() + static_cast<std::ptrdiff_t>(i - 1));
        ++removed;
      }
    }
    if (removed > 0) {
      renumber();
      ++m_generation;
    }
    return removed;
  }

  std::optional<std::size_t> AnnotationDocument::hitTest(AnnotationPoint point, double tolerance) const {
    for (std::size_t i = m_annotations.size(); i > 0; --i) {
      if (annotationIntersectsSegment(m_annotations[i - 1], point, point, tolerance)) {
        return i - 1;
      }
    }
    return std::nullopt;
  }

  void AnnotationDocument::offset(std::size_t index, double dx, double dy) {
    if (index >= m_annotations.size()) {
      return;
    }
    for (auto& point : m_annotations[index].points) {
      point.x += dx;
      point.y += dy;
    }
    ++m_generation;
  }

  bool AnnotationDocument::undo() {
    if (m_undo.empty()) {
      return false;
    }
    m_redo.push_back(m_annotations);
    while (m_redo.size() > kHistoryLimit) {
      m_redo.pop_front();
    }
    m_annotations = std::move(m_undo.back());
    m_undo.pop_back();
    renumber();
    ++m_generation;
    return true;
  }

  bool AnnotationDocument::redo() {
    if (m_redo.empty()) {
      return false;
    }
    m_undo.push_back(m_annotations);
    while (m_undo.size() > kHistoryLimit) {
      m_undo.pop_front();
    }
    m_annotations = std::move(m_redo.back());
    m_redo.pop_back();
    renumber();
    ++m_generation;
    return true;
  }

  void AnnotationDocument::renumber() {
    int count = 0;
    for (auto& annotation : m_annotations) {
      if (annotation.tool == AnnotationTool::Numbering) {
        ++count;
        annotation.text = std::to_string(count);
      }
    }
  }

  void AnnotationDocument::clear() {
    m_annotations.clear();
    m_undo.clear();
    m_redo.clear();
    m_crop.reset();
    m_interactionOpen = false;
    ++m_generation;
  }

  AnnotationRect annotationBounds(const Annotation& annotation) {
    if (annotation.points.empty()) {
      return AnnotationRect{};
    }

    const double halfWidth = annotation.width / 2.0;

    if (annotation.tool == AnnotationTool::Numbering) {
      const AnnotationPoint anchor = annotation.points.front();
      return AnnotationRect{
          .x = anchor.x - halfWidth,
          .y = anchor.y - halfWidth,
          .width = annotation.width,
          .height = annotation.width,
      };
    }

    if (annotation.tool == AnnotationTool::Text) {
      return measureText(annotation);
    }

    if (annotation.tool == AnnotationTool::Blur && annotation.points.size() >= 2) {
      return shapeRect(annotation);
    }

    double minX = annotation.points.front().x;
    double minY = annotation.points.front().y;
    double maxX = minX;
    double maxY = minY;
    for (const auto& point : annotation.points) {
      minX = std::min(minX, point.x);
      minY = std::min(minY, point.y);
      maxX = std::max(maxX, point.x);
      maxY = std::max(maxY, point.y);
    }

    // The arrow head extends past the end point, so grow the box by its reach.
    if (annotation.tool == AnnotationTool::Arrow && annotation.points.size() >= 2) {
      const double head = std::max(12.0, annotation.width * 3.0);
      minX -= head;
      minY -= head;
      maxX += head;
      maxY += head;
    }

    return AnnotationRect{
        .x = minX - halfWidth,
        .y = minY - halfWidth,
        .width = (maxX - minX) + annotation.width,
        .height = (maxY - minY) + annotation.width,
    };
  }

  bool annotationIntersectsSegment(const Annotation& annotation, AnnotationPoint a, AnnotationPoint b, double radius) {
    if (annotation.points.empty()) {
      return false;
    }

    const double reach = radius + (annotation.width / 2.0);

    if (annotation.tool == AnnotationTool::Text) {
      if (annotation.text.empty()) {
        return false;
      }
      const AnnotationRect box = measureText(annotation);
      return segmentIntersectsRect(
          a.x, a.y, b.x, b.y, box.x - radius, box.y - radius, box.x + box.width + radius, box.y + box.height + radius
      );
    }

    if (annotation.tool == AnnotationTool::Numbering) {
      const AnnotationPoint center = annotation.points.front();
      return distanceToSegment(center.x, center.y, a.x, a.y, b.x, b.y) <= reach;
    }

    if (annotationToolIsShape(annotation.tool) && annotation.points.size() >= 2) {
      const AnnotationPoint start = annotation.points.front();
      const AnnotationPoint end = annotation.points.back();
      const double left = std::min(start.x, end.x);
      const double right = std::max(start.x, end.x);
      const double top = std::min(start.y, end.y);
      const double bottom = std::max(start.y, end.y);

      switch (annotation.tool) {
      case AnnotationTool::Line:
      case AnnotationTool::Arrow:
        return distanceToSegment(start.x, start.y, a.x, a.y, b.x, b.y) <= reach
            || distanceToSegment(end.x, end.y, a.x, a.y, b.x, b.y) <= reach
            || distanceToSegment(a.x, a.y, start.x, start.y, end.x, end.y) <= reach
            || distanceToSegment(b.x, b.y, start.x, start.y, end.x, end.y) <= reach;
      case AnnotationTool::Blur:
        return segmentIntersectsRect(a.x, a.y, b.x, b.y, left, top, right, bottom);
      case AnnotationTool::Rectangle: {
        const std::array<AnnotationPoint, 2> probes{a, b};
        for (const auto& probe : probes) {
          if (distanceToSegment(probe.x, probe.y, left, top, right, top) <= reach
              || distanceToSegment(probe.x, probe.y, right, top, right, bottom) <= reach
              || distanceToSegment(probe.x, probe.y, right, bottom, left, bottom) <= reach
              || distanceToSegment(probe.x, probe.y, left, bottom, left, top) <= reach) {
            return true;
          }
        }
        return false;
      }
      case AnnotationTool::Circle: {
        const double centerX = (start.x + end.x) / 2.0;
        const double centerY = (start.y + end.y) / 2.0;
        const double radiusX = std::max(std::abs(end.x - start.x) / 2.0, 0.0001);
        const double radiusY = std::max(std::abs(end.y - start.y) / 2.0, 0.0001);
        const std::array<AnnotationPoint, 2> probes{a, b};
        for (const auto& probe : probes) {
          const double nx = (probe.x - centerX) / radiusX;
          const double ny = (probe.y - centerY) / radiusY;
          const double distance = std::abs(std::hypot(nx, ny) - 1.0) * std::min(radiusX, radiusY);
          if (distance <= reach) {
            return true;
          }
        }
        return false;
      }
      default:
        break;
      }
    }

    for (const auto& point : annotation.points) {
      if (distanceToSegment(point.x, point.y, a.x, a.y, b.x, b.y) <= reach) {
        return true;
      }
    }
    return false;
  }

  AnnotationPoint snapAngle45(AnnotationPoint start, AnnotationPoint end) {
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    if (std::abs(dx) < 0.0001 && std::abs(dy) < 0.0001) {
      return end;
    }
    constexpr double kStep = std::numbers::pi / 4.0;
    const double distance = std::hypot(dx, dy);
    const double angle = std::round(std::atan2(dy, dx) / kStep) * kStep;
    return AnnotationPoint{
        .x = start.x + (std::cos(angle) * distance),
        .y = start.y + (std::sin(angle) * distance),
    };
  }

  AnnotationPoint snapSquare(AnnotationPoint start, AnnotationPoint end) {
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    if (std::abs(dx) < 0.0001 && std::abs(dy) < 0.0001) {
      return end;
    }
    const double side = std::max(std::abs(dx), std::abs(dy));
    return AnnotationPoint{
        .x = start.x + (dx < 0.0 ? -side : side),
        .y = start.y + (dy < 0.0 ? -side : side),
    };
  }

  bool canSimplifyPoint(AnnotationPoint anchor, AnnotationPoint candidate, AnnotationPoint next, double tolerance) {
    const double segmentX = next.x - anchor.x;
    const double segmentY = next.y - anchor.y;
    const double segmentLength = std::hypot(segmentX, segmentY);
    const double incomingX = candidate.x - anchor.x;
    const double incomingY = candidate.y - anchor.y;
    const double outgoingX = next.x - candidate.x;
    const double outgoingY = next.y - candidate.y;

    if (segmentLength <= 0.0001 || ((incomingX * outgoingX) + (incomingY * outgoingY)) < 0.0) {
      return false;
    }

    const double deviation =
        std::abs((segmentX * (anchor.y - candidate.y)) - ((anchor.x - candidate.x) * segmentY)) / segmentLength;
    return deviation <= tolerance;
  }

  AnnotationRect unionRect(const AnnotationRect& a, const AnnotationRect& b) {
    if (a.width <= 0.0 || a.height <= 0.0) {
      return b;
    }
    if (b.width <= 0.0 || b.height <= 0.0) {
      return a;
    }
    const double left = std::min(a.x, b.x);
    const double top = std::min(a.y, b.y);
    const double right = std::max(a.x + a.width, b.x + b.width);
    const double bottom = std::max(a.y + a.height, b.y + b.height);
    return AnnotationRect{.x = left, .y = top, .width = right - left, .height = bottom - top};
  }

} // namespace capture
