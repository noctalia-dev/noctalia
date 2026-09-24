#include "capture/annotation_document.h"

#include <cmath>
#include <cstdio>
#include <print>
#include <string>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "annotation_document_test: {}", message);
      return false;
    }
    return true;
  }

  capture::Annotation brush(double x, double y) {
    return capture::Annotation{
        .tool = capture::AnnotationTool::Brush,
        .width = 6.0,
        .stroke = {.r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
        .fill = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F},
        .points = {{.x = x, .y = y}},
        .text = {},
    };
  }

  capture::Annotation shape(capture::AnnotationTool tool, double x0, double y0, double x1, double y1) {
    return capture::Annotation{
        .tool = tool,
        .width = 6.0,
        .stroke = {.r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
        .fill = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F},
        .points = {{.x = x0, .y = y0}, {.x = x1, .y = y1}},
        .text = {},
    };
  }

  capture::Annotation numbering(double x, double y, std::string label) {
    return capture::Annotation{
        .tool = capture::AnnotationTool::Numbering,
        .width = 32.0,
        .stroke = {.r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
        .fill = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F},
        .points = {{.x = x, .y = y}},
        .text = std::move(label),
    };
  }

} // namespace

int main() {
  bool ok = true;

  {
    // One gesture is one undo step: undo restores the pre-gesture list, redo re-applies it.
    capture::AnnotationDocument doc;
    doc.beginInteraction();
    doc.push(brush(10.0, 10.0));
    doc.endInteraction();
    doc.beginInteraction();
    doc.push(brush(20.0, 20.0));
    doc.endInteraction();

    ok = expect(doc.annotations().size() == 2, "two gestures leave two annotations") && ok;
    ok = expect(doc.undo(), "undo reports a step was applied") && ok;
    ok = expect(doc.annotations().size() == 1, "undo restores the pre-gesture list") && ok;
    ok = expect(doc.redo(), "redo reports a step was applied") && ok;
    ok = expect(doc.annotations().size() == 2, "redo re-applies the undone gesture") && ok;

    ok = expect(doc.undo(), "second undo applies") && ok;
    ok = expect(doc.canRedo(), "redo is available right after an undo") && ok;
    doc.beginInteraction();
    doc.push(brush(30.0, 30.0));
    doc.endInteraction();
    ok = expect(!doc.canRedo(), "a new gesture after undo drops the redo branch") && ok;
  }

  {
    // A cancelled gesture leaves no trace and no undo step.
    capture::AnnotationDocument doc;
    doc.beginInteraction();
    doc.push(brush(5.0, 5.0));
    doc.discardInteraction();
    ok = expect(doc.annotations().empty(), "discardInteraction removes the pushed annotation") && ok;
    ok = expect(!doc.canUndo(), "a discarded gesture leaves no undo step") && ok;
  }

  {
    // The eraser removes what its swept segment touches and nothing else.
    capture::AnnotationDocument doc;
    doc.push(shape(capture::AnnotationTool::Arrow, 0.0, 0.0, 100.0, 100.0));
    doc.push(shape(capture::AnnotationTool::Rectangle, 400.0, 400.0, 450.0, 450.0));

    const std::size_t removed = doc.eraseAlong(
        capture::AnnotationPoint{.x = 48.0, .y = 48.0}, capture::AnnotationPoint{.x = 52.0, .y = 52.0}, 4.0
    );
    ok = expect(removed == 1, "eraseAlong removes the annotation under the swept segment") && ok;
    ok = expect(
             doc.annotations().size() == 1 && doc.annotations().front().tool == capture::AnnotationTool::Rectangle,
             "the untouched rectangle survives the erase"
         )
        && ok;

    // A rectangle is an outline, so its edge erases and its interior does not.
    ok = expect(
             doc.eraseAlong(
                 capture::AnnotationPoint{.x = 425.0, .y = 425.0}, capture::AnnotationPoint{.x = 426.0, .y = 425.0}, 4.0
             ) == 0,
             "sweeping the rectangle's interior erases nothing"
         )
        && ok;
    ok = expect(
             doc.eraseAlong(
                 capture::AnnotationPoint{.x = 400.0, .y = 425.0}, capture::AnnotationPoint{.x = 401.0, .y = 425.0}, 4.0
             ) == 1,
             "sweeping the rectangle's edge erases it"
         )
        && ok;
    ok = expect(doc.annotations().empty(), "the document is empty once both are erased") && ok;
  }

  {
    // Numbering labels stay 1..n after one is erased.
    capture::AnnotationDocument doc;
    doc.push(numbering(10.0, 10.0, "1"));
    doc.push(numbering(200.0, 200.0, "2"));
    doc.push(numbering(400.0, 400.0, "3"));
    doc.erase(0);
    doc.renumber();
    ok = expect(
             doc.annotations().size() == 2 && doc.annotations()[0].text == "1" && doc.annotations()[1].text == "2",
             "renumber relabels the remaining markers 1..n"
         )
        && ok;
  }

  {
    // Hit testing picks the annotation drawn last where two overlap.
    capture::AnnotationDocument doc;
    doc.push(shape(capture::AnnotationTool::Rectangle, 0.0, 0.0, 100.0, 100.0));
    doc.push(shape(capture::AnnotationTool::Rectangle, 0.0, 0.0, 100.0, 100.0));
    const auto hit = doc.hitTest(capture::AnnotationPoint{.x = 0.0, .y = 50.0}, 4.0);
    ok = expect(hit.has_value() && *hit == 1, "hitTest returns the topmost of two overlapping rectangles") && ok;

    const auto miss = doc.hitTest(capture::AnnotationPoint{.x = 50.0, .y = 50.0}, 4.0);
    ok = expect(!miss.has_value(), "a rectangle's interior is not part of its outline hitbox") && ok;
  }

  {
    // Shift-snapping locks a line to a 45 degree axis without changing its length.
    const capture::AnnotationPoint start{.x = 0.0, .y = 0.0};
    const capture::AnnotationPoint raw{.x = 10.0, .y = 9.0};
    const capture::AnnotationPoint snapped = capture::snapAngle45(start, raw);
    ok = expect(std::abs(snapped.x - snapped.y) < 1e-6, "snapAngle45 lands on the diagonal") && ok;
    ok = expect(
             std::abs(std::hypot(snapped.x, snapped.y) - std::hypot(raw.x, raw.y)) < 1e-6,
             "snapAngle45 preserves the drag length"
         )
        && ok;

    const capture::AnnotationPoint square = capture::snapSquare(start, capture::AnnotationPoint{.x = -20.0, .y = 5.0});
    ok = expect(
             std::abs(square.x + 20.0) < 1e-6 && std::abs(square.y - 20.0) < 1e-6,
             "snapSquare takes the longer side and keeps each sign"
         )
        && ok;
  }

  {
    // History is bounded, so the oldest steps fall off instead of growing without limit.
    capture::AnnotationDocument doc;
    for (std::size_t i = 0; i < capture::AnnotationDocument::kHistoryLimit + 10; ++i) {
      doc.beginInteraction();
      doc.push(brush(static_cast<double>(i), 0.0));
      doc.endInteraction();
    }
    std::size_t steps = 0;
    while (doc.undo()) {
      ++steps;
    }
    ok = expect(steps == capture::AnnotationDocument::kHistoryLimit, "undo history trims to its limit") && ok;
    ok = expect(doc.annotations().size() == 10, "the trimmed-away gestures stay applied") && ok;
  }

  {
    // Move offsets only the addressed annotation.
    capture::AnnotationDocument doc;
    doc.push(brush(10.0, 10.0));
    doc.push(brush(50.0, 50.0));
    doc.offset(0, 5.0, -3.0);
    ok = expect(
             doc.annotations()[0].points.front().x == 15.0
                 && doc.annotations()[0].points.front().y == 7.0
                 && doc.annotations()[1].points.front().x == 50.0,
             "offset moves one annotation and leaves the other"
         )
        && ok;
  }

  {
    // The crop rect is an export parameter, so it never appears in undo history.
    capture::AnnotationDocument doc;
    doc.setCrop(capture::AnnotationRect{.x = 1.0, .y = 2.0, .width = 3.0, .height = 4.0});
    ok = expect(doc.crop().has_value(), "setCrop stores the rect") && ok;
    ok = expect(!doc.canUndo(), "setCrop records no undo step") && ok;
  }

  return ok ? 0 : 1;
}
