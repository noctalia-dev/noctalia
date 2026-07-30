#include "shell/bar/widgets/taskbar_widget.h"

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class TaskbarWidgetTestAccess {
public:
  static std::pair<bool, bool> compare(
      bool showWindowTitle, std::uintptr_t previousHandle, std::uintptr_t nextHandle, std::string previousTitle,
      std::string nextTitle
  ) {
    const TaskbarWidget::TaskModel previous{
        .handleKey = previousHandle,
        .title = std::move(previousTitle),
    };
    const TaskbarWidget::TaskModel next{
        .handleKey = nextHandle,
        .title = std::move(nextTitle),
    };
    const auto comparison = TaskbarWidget::compareModels(showWindowTitle, {previous}, {}, {next}, {});
    return {comparison.layoutEqual, comparison.titlesChanged};
  }

  // Resolves a retained tile's task reference the way the live tile callbacks do.
  static std::optional<std::string> resolvedTitle(
      const std::vector<TaskbarWidget::TaskModel>& tasks, std::size_t index, std::uint64_t referenceGeneration,
      std::uint64_t currentGeneration
  ) {
    const auto* current =
        TaskbarWidget::resolveTask(tasks, {.index = index, .generation = referenceGeneration}, currentGeneration);
    return current != nullptr ? std::optional<std::string>(current->title) : std::nullopt;
  }

  static TaskbarWidget::TaskModel task(std::uintptr_t handleKey, std::string title) {
    return {
        .handleKey = handleKey,
        .title = std::move(title),
    };
  }
};

int main() {
  // A hidden title change keeps the layout and reports the title change for tooltip refreshes.
  assert(TaskbarWidgetTestAccess::compare(false, 11, 11, "old", "new") == std::pair(true, true));
  // A displayed title is layout, so it still forces a rebuild.
  assert(!TaskbarWidgetTestAccess::compare(true, 11, 11, "old", "new").first);
  assert(TaskbarWidgetTestAccess::compare(true, 11, 11, "same", "same") == std::pair(true, false));
  // Task identity changes rebuild regardless of the title.
  assert(!TaskbarWidgetTestAccess::compare(false, 11, 12, "old", "new").first);

  auto tasks = std::vector{
      TaskbarWidgetTestAccess::task(11, "first"),
      TaskbarWidgetTestAccess::task(12, "second"),
  };
  assert(TaskbarWidgetTestAccess::resolvedTitle(tasks, 0, 7, 7) == std::optional<std::string>("first"));
  // Retained references read the current model, not the model they were built from.
  tasks[0].title = "retitled";
  assert(TaskbarWidgetTestAccess::resolvedTitle(tasks, 0, 7, 7) == std::optional<std::string>("retitled"));
  assert(!TaskbarWidgetTestAccess::resolvedTitle(tasks, 2, 7, 7).has_value());
  assert(!TaskbarWidgetTestAccess::resolvedTitle(tasks, 0, 7, 8).has_value());

  return 0;
}
