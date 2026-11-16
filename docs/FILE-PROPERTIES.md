# Files Properties

Properties is a single native window with a stable item context. Alt+Enter and the item context menu open the same window; multiple-item Properties is unsupported. General, Permissions and Details retain the approved layout. Metadata values use frame-free read-only native Input controls for mouse/keyboard selection and Ctrl+C. Long Details values retain More/Less for an expanded, scrollable selection area. Ordinary values have no tooltips.

General owns the item icon and editable filename; tabs and the compact close control sit at the top without a separate title row. Apply confirms all staged changes, running rename first and continuing with retained metadata edits only after a successful identity-checked refresh. Enter confirms only the filename edit through the existing asynchronous rename manager; Escape restores the original draft. Failed rename stops the sequence and preserves the entered name and other staged edits. Validation is shared with inline rename. Checked renames verify the source device/inode before execution, reject collisions without replacement, retain failed input and update the Properties context and browser selection on success. Like the existing path-based rename API, these checks cannot make concurrent namespace edits atomic with the rename syscall.

General uses GIO content-type descriptions, while Details keeps the technical MIME identifier and literal Filename suffix. Drive-wide metadata and capacity appear only for an exact enclosing-mount-root match, never merely because the item belongs to a filesystem. Read-only filesystem capability checks remain separate from visible drive details. Selection copying uses Files' existing desktop clipboard writer. Only long paths and symlink targets retain hover-only copy actions, which copy the complete value. Transparent edge/corner input regions provide resize cursors and resizing without a visible grip. Files' root breadcrumb is `/`; navigation paths and Places names are unchanged.

Owner/group controls use native selects, with the current account and normal users prioritised using local login policy, and eligible groups before other groups. Ownership changes remain staged. Read/Write/Execute (Traverse for folders) columns replace per-row summaries. Dropdown dismissal and keyboard navigation use the existing native popup implementation.

Unmounted volume Properties reuses the same window and tabs with discovered name, state and available filesystem/source/UUID/device metadata. It does not query an invented path, scan contents or enable item editing. Mounted locations and bookmarks use the ordinary item path and exact mount-root detection.

## Staged changes

Owner and Group Change actions list local accounts from the system account files, with reentrant NSS validation before execution. Eligible process groups are marked; other groups and owner changes require administrator authorisation. R/W/X changes, owner/group selections and MIME default application selections remain staged until Apply. Cancel/close requests with pending edits require the explicit Discard & Close action; Escape dismisses that confirmation. Metadata is queried again without following symlinks after execution, including after partial failure. Failed unapplied root changes remain staged against refreshed metadata.

General lists compatible installed applications through GIO. Changing Open with saves the user's default for the entire MIME type, including all folders for `inode/directory`; it does not set a per-file association. GIO reports persistence errors in Properties. This preference never uses administrator privileges.

## Privilege boundary

The graphical file manager stays unprivileged. Owned-file mode edits and eligible group changes run without elevation. Other ownership/mode edits use `/usr/bin/pkexec --disable-internal-agent` with the installed `nythera-properties-helper`. Nythera's existing Polkit agent (or another registered desktop agent) collects authentication; Files never collects passwords. The client refuses user-writable installation paths, missing policies and actions not registered with the expected executable annotation. There is no generic action or command fallback.

The dedicated `dev.nythera.Files.modify-properties` action requires active-session administrator authentication without retained authorisation. The helper accepts one fixed request: encoded absolute path, device/inode identity, original/edited modes, optional validated account names and a recursive flag. It accepts no executable or command string. Both sides validate the request; numeric IDs cannot substitute for account names. Owner and group fields not selected for change use the kernel's unchanged sentinel. The helper is an ordinary executable, **not setuid**.

The engine pins the selected inode with `openat2`/`O_PATH`, rejects symbolic links and substituted identities, and modifies only the pinned object. There is no permissive fallback on kernels without `openat2`. Permission masks preserve unedited bits and current special bits. Ownership changes may cause Linux to clear setuid/setgid or file capabilities; these effects are reported rather than silently restoring security metadata.

## Recursive permissions

The recursive checkbox applies only to directories and cannot be combined with ownership changes. A cancellable read-only preflight checks ownership and traversal access; an owned, accessible tree runs without elevation. When the preflight finds protected entries, one authorisation covers the whole operation rather than one prompt per child. Namespace changes after preflight can still cause ordinary partial failures. It applies only the explicitly edited permission bits. Execute edits affect directories; regular files retain their existing execute bits. This prevents a traversal edit from making ordinary data files executable.

Traversal uses root-relative `openat2` resolution with `RESOLVE_BENEATH`, `RESOLVE_NO_SYMLINKS`, `RESOLVE_NO_MAGICLINKS` and `RESOLVE_NO_XDEV`. Child mounts, including bind mounts on the same filesystem, are excluded. Symlinks, special entries and hard-linked child files are skipped; modifying a hard-linked child could affect a path outside the tree. Directly selecting a hard-linked file still addresses that inode, as normal POSIX ownership/permission changes do. Directories are changed after their contents and re-resolved from the selected root before applying changes. Traversal is limited to depth 128 and one million entries, with bounded directory handles and path memory. Hitting a limit is a partial failure, not success.

File-operation jobs provide progress counts, cancellation and final succeeded/failed/skipped counts. Duplicate active operations on the same device/inode are rejected. Administrator helpers also take an advisory inode lock against duplicate helper processes. Cancellation is checked between entries and before mutation; completed changes are not rolled back. EOF on the private stdin pipe stops the helper if the client cancels or exits. A cancellation during authentication prevents subsequent mutations but may leave the desktop authentication prompt visible until the agent resolves it. A filesystem syscall already in progress must return before cancellation can finish.

Open descriptors prevent path substitution from redirecting a mutation. They cannot freeze a concurrently edited filesystem namespace: an object can be renamed after being resolved. A mutation remains attached to the inode resolved within the authorised scope. Concurrent filesystem activity may produce skipped entries or partial failures; no atomic tree snapshot or rollback is promised.

## Installation and validation

Build with at most two jobs:

```fish
ninja -C build-debug -j2 nythera-file-manager nythera-properties-helper property_operations_test property_permissions_test file_operation_manager_test
```

An administrator can install **only** the helper and its policy after reviewing the change:

```fish
meson install -C build-debug --no-rebuild --tags properties-helper
```

The helper path follows Meson's prefix/libexecdir. The policy is installed into Polkit's pkg-config `policydir`, which can differ from Meson's prefix. With the current development configuration these are `/usr/local/libexec/nythera-properties-helper` and `/usr/share/polkit-1/actions/dev.nythera.Files.modify-properties.policy`. Both files and their ancestors must be root-owned and not group/other-writable. Polkit must register the action and a desktop authentication agent must be available. No helper or policy is installed by a build.

Automated tests use temporary fixtures and injected authorisation outcomes; they never authenticate or run the helper as root. Existing mount boundaries are inspected without mounting or modifying them. Real authentication/denial, cancellation during authentication, root ownership changes, capability clearing, bind-mount integration, filesystem-specific behaviour and GUI acceptance require an authorised integration environment. Tests and compilation do not establish visual correctness.

## Implementation files

- Folder calculation: `src/filemanager/folder_size.cpp` / `folder_size.h`, tested in `tests/folder_size_test.cpp`.
- Selectable metadata uses the existing `src/ui/controls/input.cpp` / `input.h`, with read-only regression coverage in `tests/input_read_only_test.cpp`.
- Window and entry points: `src/filemanager/properties_window.cpp`, `properties_window.h`, `file_manager.cpp`, `file_manager.h`; shared name validation and root label: `src/filemanager/file_interaction.h`, `file_breadcrumbs.h`.
- Shared pinned-inode permission primitive: `src/filemanager/property_permissions.h`; item metadata helpers: `src/filemanager/property_metadata.h`.
- Request validation, account resolution and bounded traversal: `src/filemanager/property_operations.cpp`, `property_operations.h`.
- Unprivileged authorisation client and installed helper entry point: `src/filemanager/property_operation_client.cpp`, `property_helper_main.cpp`.
- Existing job integration: `src/filemanager/file_operation_manager.cpp`, `file_operation_manager.h`.
- Build and policy: `meson.build`, `data/polkit/dev.nythera.Files.modify-properties.policy.in`.
- Focused coverage: `tests/property_permissions_test.cpp`, `property_operations_test.cpp`, `property_metadata_test.cpp`, `file_operation_manager_test.cpp`, `file_interaction_test.cpp`.
- Behaviour and installation record: this document and `docs/NYTHERA-CUSTOMISATIONS.md`.

## Interaction validation

Read-only Input mode blocks printable keys, editing shortcuts, clear actions and IME edits while retaining native mouse selection, Shift+arrow navigation, Ctrl+A and Ctrl+C. Metadata uses a per-control copy callback into the existing desktop clipboard writer rather than another clipboard backend. Enter in the filename field is handled by Properties directly, independent of the standalone application's global validation matcher.

Only unclear action controls retain tooltips. Properties supplies pointer coordinates relative to its own surface for the existing Files fresh-hover policy; previously the shared pointer position could still refer to the main browser surface. Properties clears owned tooltips on scrolling, leaving, rebuilding and closing, before destroying its parent surface. No shared tooltip-manager code is changed. Popup positioning and mouse selection still require manual acceptance; automated tests use keyboard events and isolated filesystem fixtures.

## Folder size and contextual feedback

Opening a real directory starts one GTask worker; symlink Properties never scan the target. Size shows Calculating with a compact native spinner, then human-readable logical bytes (and exact bytes where useful), regular-file, folder and link counts. Tabs reuse the active scan or cached result. Metadata refresh of the same path/device/inode reuses that result; a successful rename cancels it and starts a calculation for the new path. Closing cancels the worker and invalidates its revision. Completion is guarded by window lifetime, generation, request identity/path and scan revision, so an older completion cannot replace a new target's data.

The definition is the sum of `st_size` for regular-file and symlink directory entries, counting hard-linked files once per name. Directory inode sizes and allocated disk usage are excluded. Subdirectory counts exclude the selected root; links and special entries have separate counts. No file contents are read. The selected root may itself be on a network filesystem. Root-relative opening reuses the no-follow, beneath-root, no-XDEV resolver; child mounts (including same-device bind mounts) are excluded and reported. Active-ancestor identities detect cycles, and an entry limit of one million, depth limit of 128 and path limit of 4096 bytes bound directory handles and path memory. There is no per-directory worker and no unbounded visited-entry set.

Unreadable directories, missing entries, cycles, limits, overflow and excluded child mounts mark the result Partial. Root opening, identity or enumeration failures are explicitly failed calculations. Cancellation retains the observed totals. Size uses concise category summaries rather than raw path/error dumps. A failure count measures failed operations, not the number of descendants omitted: a single unreadable directory can exclude an entire subtree. A bounded sample of 16 relative paths, operations and errno values is retained in the result and emitted through GLib debug logging (`G_MESSAGES_DEBUG=all`); permission errors, missing/stale entries, other I/O errors and unreadable subtrees have separate counts. This distinguishes likely causes without treating another file manager's totals as authoritative. Hidden entries are included regardless of file-view visibility. Compact native Refresh and Stop icon controls reuse the Size section. Refresh is disabled while a calculation is active and invalidates the idle cached result before starting a replacement. Stop requests cancellation and leaves the completion guard intact, preserving the cancelled totals; another scan cannot start until that worker completes. Counts and exact byte totals use the environment locale's grouping. Complete results have no Calculation/Status row. Folder calculation policy and safeguards are available in Details. Only start/completion update the UI, avoiding per-entry update queues. Cancellation is checked between entries and after opening them. A blocked filesystem syscall may delay worker termination, but never blocks the UI thread. Totals are best-effort observations rather than an atomic filesystem snapshot, and are cached until Refresh is requested, the context changes or Properties is reopened; they are not a live filesystem monitor.

Properties has no shared operation-result strip. Successful edits refresh their state without a confirmation label. Rename errors appear immediately below Name, association errors below Open with, and ownership/permission errors in Permissions; each has a Dismiss action and persists across tab switches until resolved or dismissed. A partial permission result, excluded entries, cancellation or ownership-related security-bit effects retain the meaningful summary in Permissions. A combined Apply's prior successful steps are mentioned only when needed to explain a later failure. Folder-size notes remain confined to Size. Existing file-operation jobs continue to handle long-running permission progress; neither their status-bar behaviour nor notifications are changed.
