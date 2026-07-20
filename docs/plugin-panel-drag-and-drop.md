# Plugin panel drag and drop

Noctalia plugin API 5 adds native pointer drag and drop to declarative plugin panels. It is intended for reorderable
lists, kanban-style groups, category moves, launchers, keybind editors, and similar interfaces. The host performs pointer
tracking, hit testing, animations, cursor changes, and drag-preview rendering. Luau receives one callback when a valid
drop completes and remains responsible for changing and persisting the plugin's data.

Drag and drop is currently available in plugin panels. The controls are rejected outside that surface.

## Requirements

Set `plugin_api = 5` in the plugin manifest and build the panel with `ui.dragSource` and `ui.dropZone`.

```toml
plugin_api = 5
```

Both controls are flex containers. They accept children and the common row/column layout and visual properties:
`width`, `height`, `flexGrow`, `opacity`, `visible`, `gap`, `padding`, `paddingH`, `paddingV`, `align`, `justify`, `fill`,
`radius`, `border`, `borderWidth`, `minWidth`, and `minHeight`.

## Minimal example

```luau
local items = {
  { id = "first", title = "First task" },
  { id = "second", title = "Second task" },
}

function onTaskDropped(taskId, targetList)
  -- Update the local model first so the retained UI lands without a flash.
  moveTask(taskId, targetList)
  renderPanel()

  -- Persist asynchronously if required by the plugin.
  saveTasks()
end

local function taskRow(task)
  return ui.row({ key = "task-" .. task.id, gap = 8, align = "center" }, {
    ui.dragSource({
      key = "task-handle-" .. task.id,
      width = 28,
      height = 28,
      dragType = "todo-task",
      payload = task.id,
      previewAncestor = 1,
      liftFromLayout = true,
      tooltip = noctalia.tr("panel.move_task"),
    }, {
      ui.glyph({ name = "menu", size = 16 }),
    }),
    ui.label({ text = task.title, flexGrow = 1 }),
  })
end

function renderPanel()
  local rows = {}
  for _, task in ipairs(items) do
    rows[#rows + 1] = taskRow(task)
  end

  panel.render(ui.dropZone({
    key = "todo-list-inbox",
    accepts = { "todo-task" },
    value = "inbox",
    onDrop = "onTaskDropped",
    padding = 8,
    gap = 4,
    radius = 12,
  }, rows))
end

function onOpen(_context)
  renderPanel()
end
```

Dragging the handle renders the whole parent row as a native ghost because `previewAncestor = 1`. Dropping calls
`onTaskDropped(payload, value)`, which in this example becomes `onTaskDropped(task.id, "inbox")`.

## `ui.dragSource`

Required properties:

| Property | Type | Meaning |
| --- | --- | --- |
| `dragType` | string | Application-defined type used to match compatible drop zones. |
| `payload` | string | Opaque value delivered as the first argument to the drop callback. |

Optional properties:

| Property | Type | Default | Meaning |
| --- | --- | --- | --- |
| `enabled` | boolean | `true` | Disables arming and dragging when false. |
| `tooltip` | string | empty | Tooltip shown over the draggable area. |
| `previewAncestor` | integer `0..8` | `0` | Number of parent levels to use for the ghost. `0` previews the source itself; `1` commonly previews its row. |
| `liftFromLayout` | boolean | `false` | Temporarily removes the previewed node from layout while dragging. Use with expanding insertion zones for sortable lists. |

The whole bounds of `ui.dragSource` act as the pointer handle. A press becomes a drag only after the native movement
threshold is crossed, so a small accidental movement does not immediately start dragging. The cursor changes from grab
to grabbing while active.

For a full-row handle, wrap the complete row in `ui.dragSource`. For a small dedicated handle with a full-row ghost,
place `ui.dragSource` inside the row and set `previewAncestor = 1`.

If the requested level would reach the panel branch that owns the native drag overlay, Noctalia clamps the preview to
the highest safe ancestor on the plugin-content branch. This prevents a proxy from recursively rendering itself.

The ghost is a render proxy in the panel overlay. It is not a second Luau tree, does not participate in hit testing,
and is not clipped by a surrounding `ui.scroll` viewport.

## `ui.dropZone`

Required properties:

| Property | Type | Meaning |
| --- | --- | --- |
| `accepts` | array of strings | Drag types accepted by this zone. An empty array accepts nothing. |
| `value` | string | Opaque target value delivered as the second callback argument. |
| `onDrop` | string | Name of a global Luau function receiving `(payload, value)`. |

Optional properties:

| Property | Type | Default | Meaning |
| --- | --- | --- | --- |
| `direction` | `"column"` or `"row"` | `"column"` | Flex direction for the zone's children. |
| `enabled` | boolean | `true` | Excludes the zone from target selection when false. |
| `expandOnDrag` | boolean | `false` | Animates a fixed-height zone to the dragged preview's height while it is targeted. |
| `hitSlop` | number | `0` | Adds a drag-only proximity area without changing layout or intercepting normal clicks. Values are clamped to the supported range. |

An active compatible zone receives Noctalia's primary translucent fill and focus-ring border. Its `radius` follows the
global Noctalia corner-roundness setting. The declared fill and border return when the pointer leaves or the drag ends.

Nested zones are supported. Normal hit testing chooses the deepest compatible zone. Proximity zones with `hitSlop`
are considered first; when several overlap, the closest zone wins, with the deeper zone breaking an exact tie. A zone
outside a clipped or hidden ancestor cannot be selected.

## Reorderable lists with continuous gaps

A sortable list normally places a thin insertion zone before every item and one after the last item. The thin zone
occupies very little idle space. `hitSlop` makes the nearest insertion point active, and `expandOnDrag` grows it to the
dragged row height. With `liftFromLayout = true`, the source row leaves its old position as the destination gap opens,
so there is always one visible insertion point rather than an inert band between rows.

```luau
local DND_TYPE = "todo-task"

local function insertionZone(anchorId, placement)
  return ui.dropZone({
    key = "insert-" .. placement .. "-" .. anchorId,
    accepts = { DND_TYPE },
    value = placement .. "|" .. anchorId,
    onDrop = "onTaskReordered",
    height = 3,
    radius = 8,
    expandOnDrag = true,
    hitSlop = 64,
  })
end

local function sortableRows(items)
  local children = {}
  for _, item in ipairs(items) do
    children[#children + 1] = insertionZone(item.id, "before")
    children[#children + 1] = taskRow(item)
  end
  if #items > 0 then
    children[#children + 1] = insertionZone(items[#items].id, "after")
  end
  return children
end

function onTaskReordered(taskId, target)
  local placement, anchorId = string.match(target, "^([^|]+)|(.+)$")
  if (placement ~= "before" and placement ~= "after") or anchorId == nil or taskId == anchorId then return end

  local previous = cloneItems(items)
  if reorderTask(items, taskId, anchorId, placement) then
    renderPanel() -- optimistic landing
    persistTasks(function(ok)
      if not ok then
        items = previous
        renderPanel()
      end
    end)
  end
end
```

The parsing above is deliberately simple; plugins may encode `value` and `payload` in any stable string format. JSON is
also suitable when structured data is needed, as long as the size limits below are respected.

When a list can be empty, keep a category-level `ui.dropZone` around its content so the empty category remains a valid
destination. Nested insertion zones will take precedence over that catch-all target while the pointer is near a row.

## Cross-container moves

Use the same `dragType` in every compatible container and give each outer zone a different `value`:

```luau
local function taskColumn(column)
  return ui.dropZone({
    key = "column-" .. column.id,
    accepts = { "todo-task" },
    value = column.id,
    onDrop = "onTaskMoved",
    radius = 12,
    padding = 8,
  }, sortableRows(column.items))
end

function onTaskMoved(taskId, destinationColumnId)
  moveTaskBetweenColumns(taskId, destinationColumnId)
  renderPanel()
  saveBoard()
end
```

This pattern works for ToDo categories, keybind groups, playlist sections, launcher folders, and other plugin-owned
collections. Noctalia does not interpret the payload or mutate the model automatically.

## Rendering and persistence guidance

- Give sources, rows, zones, and repeated children stable `key` values. Retained reconciliation can then reuse native
  nodes instead of rebuilding them.
- Update the in-memory model and call the plugin's render helper, which must pass the new tree to `panel.render(...)`,
  before starting slower file or IPC persistence. This makes the dropped row land immediately instead of flashing back
  to its previous position.
- Keep a previous snapshot when persistence can fail. Restore it and render again on failure.
- Avoid publishing a full-screen loading state for background persistence. Keep the last ready data visible until the
  refreshed snapshot arrives.
- The callback is asynchronous with respect to the native pointer release. Do not rely on native nodes remaining in the
  drag state after the callback is queued.
- `enabled = false` is useful while a save or modal editor is active.

## Validation and limits

Invalid required properties disable that control and write a diagnostic to the Noctalia log. They do not reuse stale
values from an earlier render.

- `dragType`, `value`, and `onDrop` must be non-empty strings of at most 256 bytes.
- `payload` must be a non-empty string of at most 16 KiB.
- `accepts` must contain no more than 16 non-empty strings, each at most 256 bytes.
- `previewAncestor` must be an integer from 0 through 8.
- Only a left-button drag is currently armed.
- Hiding, disabling, removing, or changing an active source or target cancels the current drag safely.
- Releasing outside a compatible target produces no callback.

Treat `payload` and `value` as untrusted input when they are used to select files, construct commands, or address an
external service. Prefer opaque IDs looked up in the plugin's current model over executable text.

## Troubleshooting

If dragging does not start, verify that the panel manifest uses plugin API 5, the source has non-empty `dragType` and
`payload`, and `enabled` is not false. If no target highlights, confirm that its `accepts` array contains the exact same
case-sensitive drag type and that all required target properties are present.

If a sortable list has dead bands, add thin insertion zones around every item, enable `expandOnDrag`, and increase
`hitSlop`. If a dropped item flashes back, update the local model before asynchronous persistence and keep ready data on
screen while the backing service refreshes.
