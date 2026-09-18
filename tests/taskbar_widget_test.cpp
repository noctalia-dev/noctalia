#include "shell/bar/widgets/taskbar_widget.h"
#include "tests/test_check.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class TaskbarWidgetTestAccess {
public:
  static std::pair<bool, bool>
  compare(std::uintptr_t previousHandle, std::uintptr_t nextHandle, std::string previousTitle, std::string nextTitle) {
    const TaskbarWidget::TaskModel previous{
        .handleKey = previousHandle,
        .title = std::move(previousTitle),
    };
    const TaskbarWidget::TaskModel next{
        .handleKey = nextHandle,
        .title = std::move(nextTitle),
    };
    const auto comparison = TaskbarWidget::compareModels(false, {previous}, {}, {next}, {});
    return {comparison.layoutEqual, comparison.titlesChanged};
  }
  static std::pair<bool, bool> activeFlip(bool groupByWorkspace) {
    const TaskbarWidget::TaskModel previous{
        .handleKey = 11,
        .active = false,
    };
    const TaskbarWidget::TaskModel next{
        .handleKey = 11,
        .active = true,
    };
    const auto comparison = TaskbarWidget::compareModels(groupByWorkspace, {previous}, {}, {next}, {});
    return {comparison.layoutEqual, comparison.activesChanged};
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

  static TaskbarWidget::TaskModel untitledTask(std::uintptr_t handleKey, std::string appId, std::string displayName) {
    return {
        .handleKey = handleKey,
        .appId = std::move(appId),
        .displayName = std::move(displayName),
    };
  }

  static std::string displayTitle(std::string title, std::string displayName, std::string appId) {
    const TaskbarWidget::TaskModel task{
        .appId = std::move(appId),
        .title = std::move(title),
        .displayName = std::move(displayName),
    };
    return TaskbarWidget::displayTitle(task);
  }

  // Reads a retained tile's text the way the tooltip provider and title refresh do.
  static std::optional<std::string> resolvedDisplayTitle(
      const std::vector<TaskbarWidget::TaskModel>& tasks, std::size_t index, std::uint64_t referenceGeneration,
      std::uint64_t currentGeneration
  ) {
    const auto* current =
        TaskbarWidget::resolveTask(tasks, {.index = index, .generation = referenceGeneration}, currentGeneration);
    return current != nullptr ? std::optional<std::string>(TaskbarWidget::displayTitle(*current)) : std::nullopt;
  }

  static std::pair<bool, bool> compareDisplayNames(std::string previousName, std::string nextName) {
    const TaskbarWidget::TaskModel previous{
        .handleKey = 11,
        .displayName = std::move(previousName),
    };
    const TaskbarWidget::TaskModel next{
        .handleKey = 11,
        .displayName = std::move(nextName),
    };
    const auto comparison = TaskbarWidget::compareModels(false, {previous}, {}, {next}, {});
    return {comparison.layoutEqual, comparison.titlesChanged};
  }

  static std::string bindingWindowId(std::string workspaceWindowId, std::string exactId) {
    return std::string(
        TaskbarWidget::workspaceBindingWindowId(
            TaskbarWidget::TaskModel{
                .workspaceWindowId = std::move(workspaceWindowId),
                .exactWindowId = std::move(exactId),
            }
        )
    );
  }

  static std::pair<std::string, std::string> rebindWorkspaceWindow(std::string exactId, std::string assignedWindowId) {
    TaskbarWidget::TaskModel task{
        .workspaceWindowId = exactId,
        .exactWindowId = std::move(exactId),
    };
    // Simulate reconciliation overwriting workspaceWindowId.
    task.workspaceWindowId = std::move(assignedWindowId);
    return {task.workspaceWindowId, task.exactWindowId};
  }

  static bool exactWindowIdChangeKeepsLayout(bool groupByWorkspace, std::string previousExact, std::string nextExact) {
    const TaskbarWidget::TaskModel previous{
        .handleKey = 11,
        .workspaceWindowId = "41",
        .exactWindowId = std::move(previousExact),
    };
    const TaskbarWidget::TaskModel next{
        .handleKey = 11,
        .workspaceWindowId = "41",
        .exactWindowId = std::move(nextExact),
    };
    return TaskbarWidget::compareModels(groupByWorkspace, {previous}, {}, {next}, {}).layoutEqual;
  }
};

int main() {
  // Title changes are patched into retained labels and tooltips, never treated as structural.
  TEST_CHECK(TaskbarWidgetTestAccess::compare(11, 11, "old", "new") == std::pair(true, true));
  TEST_CHECK(TaskbarWidgetTestAccess::compare(11, 11, "same", "same") == std::pair(true, false));
  // Task identity changes rebuild regardless of the title.
  TEST_CHECK(!TaskbarWidgetTestAccess::compare(11, 12, "old", "new").first);
  // Flat tiles patch active visuals in place; grouped summaries still rebuild.
  TEST_CHECK(TaskbarWidgetTestAccess::activeFlip(false) == std::pair(true, true));
  TEST_CHECK(!TaskbarWidgetTestAccess::activeFlip(true).first);

  auto tasks = std::vector{
      TaskbarWidgetTestAccess::task(11, "first"),
      TaskbarWidgetTestAccess::task(12, "second"),
  };
  TEST_CHECK(TaskbarWidgetTestAccess::resolvedTitle(tasks, 0, 7, 7) == std::optional<std::string>("first"));
  // Retained references read the current model, not the model they were built from.
  tasks[0].title = "retitled";
  TEST_CHECK(TaskbarWidgetTestAccess::resolvedTitle(tasks, 0, 7, 7) == std::optional<std::string>("retitled"));
  TEST_CHECK(!TaskbarWidgetTestAccess::resolvedTitle(tasks, 2, 7, 7).has_value());
  TEST_CHECK(!TaskbarWidgetTestAccess::resolvedTitle(tasks, 0, 7, 8).has_value());

  // A window with a blank title shows its desktop entry name, or the app id when no entry matches.
  TEST_CHECK(TaskbarWidgetTestAccess::displayTitle("Document", "Editor", "org.example.editor") == "Document");
  TEST_CHECK(TaskbarWidgetTestAccess::displayTitle("", "Editor", "org.example.editor") == "Editor");
  TEST_CHECK(TaskbarWidgetTestAccess::displayTitle(" ", "Editor", "org.example.editor") == "Editor");
  TEST_CHECK(TaskbarWidgetTestAccess::displayTitle("", "", "org.example.editor") == "org.example.editor");
  TEST_CHECK(TaskbarWidgetTestAccess::displayTitle("", "", "").empty());
  // A display name change patches the retained label like a title change.
  TEST_CHECK(TaskbarWidgetTestAccess::compareDisplayNames("Editor", "Editor (Beta)") == std::pair(true, true));
  TEST_CHECK(TaskbarWidgetTestAccess::compareDisplayNames("Editor", "Editor") == std::pair(true, false));

  // Retained references to an untitled window read the fallback until a title arrives; the model
  // title itself stays empty for matching.
  tasks.push_back(TaskbarWidgetTestAccess::untitledTask(13, "org.example.editor", "Editor"));
  TEST_CHECK(TaskbarWidgetTestAccess::resolvedTitle(tasks, 2, 7, 7) == std::optional<std::string>(""));
  TEST_CHECK(TaskbarWidgetTestAccess::resolvedDisplayTitle(tasks, 2, 7, 7) == std::optional<std::string>("Editor"));
  tasks[2].title = "Document";
  TEST_CHECK(TaskbarWidgetTestAccess::resolvedDisplayTitle(tasks, 2, 7, 7) == std::optional<std::string>("Document"));

  // Workspace placement can be rebound while the authoritative exact identity
  // remains intact for focus/close actions.
  TEST_CHECK(
      TaskbarWidgetTestAccess::rebindWorkspaceWindow("41", "42") == std::pair(std::string("42"), std::string("41"))
  );
  // workspaceBindingWindowId prefers the authoritative exact identity when set.
  TEST_CHECK(TaskbarWidgetTestAccess::bindingWindowId("42", "41") == "41");
  TEST_CHECK(TaskbarWidgetTestAccess::bindingWindowId("42", "") == "42");
  // Binding changes are event-time data in flat mode and structure in grouped mode.
  TEST_CHECK(TaskbarWidgetTestAccess::exactWindowIdChangeKeepsLayout(false, "", "41"));
  TEST_CHECK(!TaskbarWidgetTestAccess::exactWindowIdChangeKeepsLayout(true, "", "41"));
  TEST_CHECK(TaskbarWidgetTestAccess::exactWindowIdChangeKeepsLayout(true, "41", "41"));

  return 0;
}
