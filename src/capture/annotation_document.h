#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace capture {

  enum class AnnotationTool : std::uint8_t {
    Move,
    Brush,
    Highlighter,
    Line,
    Arrow,
    Rectangle,
    Circle,
    Text,
    Numbering,
    Blur,
    Eraser,
    Crop,
  };

  inline constexpr std::size_t kAnnotationToolCount = 12;

  [[nodiscard]] std::string_view annotationToolName(AnnotationTool tool) noexcept;
  [[nodiscard]] std::optional<AnnotationTool> annotationToolFromName(std::string_view name) noexcept;
  // Shapes are two-point drags: {start, end}.
  [[nodiscard]] bool annotationToolIsShape(AnnotationTool tool) noexcept;
  // Tools that never produce an annotation of their own.
  [[nodiscard]] bool annotationToolIsNonDrawing(AnnotationTool tool) noexcept;
  [[nodiscard]] double annotationToolDefaultWidth(AnnotationTool tool) noexcept;

  struct AnnotationPoint {
    double x = 0.0;
    double y = 0.0;
  };

  struct AnnotationColor {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;

    bool operator==(const AnnotationColor&) const = default;
  };

  struct Annotation {
    AnnotationTool tool = AnnotationTool::Brush;
    double width = 6.0; // logical px; Text: font px; Numbering: circle diameter
    AnnotationColor stroke{};
    AnnotationColor fill{.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F}; // Rectangle/Circle; a == 0 means no fill
    std::vector<AnnotationPoint> points;
    std::string text; // Text: content; Numbering: label
  };

  struct AnnotationRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
  };

  // Freehand/shape editing model with one undo step per pointer gesture.
  class AnnotationDocument {
  public:
    static constexpr std::size_t kHistoryLimit = 50;

    [[nodiscard]] const std::vector<Annotation>& annotations() const noexcept { return m_annotations; }
    [[nodiscard]] std::optional<AnnotationRect> crop() const noexcept { return m_crop; }
    // Crop is an export parameter, not document content, so it stays out of undo history.
    void setCrop(std::optional<AnnotationRect> crop);

    void beginInteraction();
    void discardInteraction();
    [[nodiscard]] bool interactionOpen() const noexcept { return m_interactionOpen; }
    void endInteraction() noexcept { m_interactionOpen = false; }

    Annotation& push(Annotation annotation);
    [[nodiscard]] Annotation* active();
    void erase(std::size_t index);
    // Removes every annotation the segment touches, topmost first. Returns the number removed.
    std::size_t eraseAlong(AnnotationPoint from, AnnotationPoint to, double radius);
    [[nodiscard]] std::optional<std::size_t> hitTest(AnnotationPoint point, double tolerance) const;
    void offset(std::size_t index, double dx, double dy);

    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    [[nodiscard]] bool canUndo() const noexcept { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !m_redo.empty(); }

    // Relabels Numbering annotations 1..n in list order.
    void renumber();
    void clear();

    // Bumps on every mutation; the overlay compares it to decide on a full re-raster.
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
    void touch() noexcept { ++m_generation; }

  private:
    std::vector<Annotation> m_annotations;
    std::deque<std::vector<Annotation>> m_undo;
    std::deque<std::vector<Annotation>> m_redo;
    std::optional<AnnotationRect> m_crop;
    std::uint64_t m_generation = 0;
    bool m_interactionOpen = false;
  };

  // Bounds include the stroke width and, for Arrow, the head. Text measures through
  // capture::measureText (annotation_raster.h).
  [[nodiscard]] AnnotationRect annotationBounds(const Annotation& annotation);
  [[nodiscard]] bool
  annotationIntersectsSegment(const Annotation& annotation, AnnotationPoint a, AnnotationPoint b, double radius);
  [[nodiscard]] AnnotationPoint snapAngle45(AnnotationPoint start, AnnotationPoint end);
  [[nodiscard]] AnnotationPoint snapSquare(AnnotationPoint start, AnnotationPoint end);
  // True when `candidate` deviates less than `tolerance` from the anchor..next segment and
  // does not reverse direction, so it can be replaced by `next` instead of appended.
  [[nodiscard]] bool
  canSimplifyPoint(AnnotationPoint anchor, AnnotationPoint candidate, AnnotationPoint next, double tolerance);
  [[nodiscard]] AnnotationRect unionRect(const AnnotationRect& a, const AnnotationRect& b);

} // namespace capture
