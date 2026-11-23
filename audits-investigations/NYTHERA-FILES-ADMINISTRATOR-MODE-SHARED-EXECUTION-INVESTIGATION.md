# Nythera Files — Administrator Mode Shared Execution Migration Investigation

**Repository:** `Nythera-Dev/Nythera` (`dev` at `7778e9117`)\
**Mode:** Read-only investigation (no edits, builds, commits, or restarts performed)

---

## 1. Executive Verdict

**Feasible and low-risk via direct migration — no new daemon, D-Bus service, or filesystem framework is needed.**

The smallest practical path to a Dolphin-style shared execution model is:

1. **Extract the pure local filesystem execution core** out of [file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp) (Copy, Duplicate, Move, Rename/checked-rename, CreateFile/CreateFolder/CreateSymlink, Delete, conflict state machine, staging, and recovery) into a small shared translation unit (e.g. `file_operation_engine.{h,cpp}`) alongside existing [fs_compat.cpp](../src/filemanager/fs_compat.cpp).
2. **Link that shared engine directly into both `nythera_core_lib` and the existing `nythera-admin-helper`** (upgrading `nythera-admin-helper`'s Meson dependency from `glib_dep` to `gio_dep`, matching [nythera-properties-helper](../meson.build#L1275-L1278)), while forcing `GIO_USE_VFS=local` and `GVFS_DISABLE_FUSE=1` in [applyAdminHelperProcessHardening()](../src/filemanager/admin_helper_session.cpp#L475-L497).
3. **Run the shared engine inside the existing per-tab `pkexec` session helper** (`nythera-admin-helper`) on a single operation thread, multiplexed over the existing `stdin`/`stdout` line protocol (`RUN_OP` / `OP_SOURCE` / `CONFLICT` / `CONFLICT_REPLY` / `PROG` / `OP_RESULT`), with [FileOperationManager::worker()](../src/filemanager/file_operation_manager.cpp#L1892-L3414) acting as the `GTask` bridge on the GUI side.
4. **Route all Administrator Mode mutations through `FileOperationManager`** and leave the existing custom fd-relative mutation handlers (`CREATE_FILE`, `CREATE_DIR`, `RENAME_ENTRY`, `COPY_FILE`, `COPY_ITEMS`) intact in the repository as dormant legacy code.

This eliminates the dual filesystem engine immediately, delivers full Copy/Paste, Cut/Move, Duplicate, Rename, Create, Permanent Delete, conflict resolution, pause/resume, progress, cancellation, recovery, and normal-mode NFS compatibility in Administrator Mode, and preserves every existing Polkit, per-tab lifecycle, timeout, Finish, visual, and test-isolation invariant.

---

## 2. Existing Code Reuse

### A. Directly Reusable Filesystem Execution Logic (Zero Duplication)

Inspection of [file_operation_manager.h](../src/filemanager/file_operation_manager.h), [file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp), [fs_compat.cpp](../src/filemanager/fs_compat.cpp), and [file_interaction.h](../src/filemanager/file_interaction.h) confirms that the local filesystem engine has **zero Wayland, Render, UI, GTK, or Polkit dependencies**. The following code can execute identically in both normal mode and inside `nythera-admin-helper`:

| Component | Source Location | What Is Shared |
| --- | --- | --- |
| **Atomic publish, NFS fallback & backup recovery** | [fs_compat.h](../src/filemanager/fs_compat.h), [fs_compat.cpp](../src/filemanager/fs_compat.cpp) | `probeCapabilities()`, `preflightTransferDestination()`, `moveOrPublishEntryNoReplace()` (`RENAME_NOREPLACE` -> `linkat` / `symlinkat` / Option A directory rename), `isAmbiguousNetworkErrno()` recovery, `allocateReplacementBackup()`, `restoreOrReportBackup()`, `removeOrReportBackup()`, `writeBufferToNewFileExclusive()`, `createDirectoryExclusive()`. |
| **Transfer & mutation core** | [file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp#L102-L140), [#L334-L448](../src/filemanager/file_operation_manager.cpp#L334-L448), [#L564-L801](../src/filemanager/file_operation_manager.cpp#L564-L801), [#L858-L945](../src/filemanager/file_operation_manager.cpp#L858-L945), [#L2262-L2409](../src/filemanager/file_operation_manager.cpp#L2262-L2409), [#L2634-L3414](../src/filemanager/file_operation_manager.cpp#L2634-L3414) | `measureTree()`, `copyTree()`, `removeTree()`, `cleanupStagedTree()`, `ensureWritableDirectoriesForCleanup()`, `isSameOrDescendant()`, `createItem()`, `PrivateStaging`, `FileTransferPauseState`, and the entire execution loop for `Copy`, `Duplicate`, `Move`, `Rename` / `renameChecked`, `CreateFolder`, `CreateFile` (including template copy), `CreateSymlink`, and `Delete` — including conflict type-matching (`applyConflictToAll`), directory merge expansion, merged source-dir cleanup, cross-fs move staging & split-state reporting, and `completedEntries`/`movedSources` assembly. |
| **Filename validation** | [file_interaction.h](../src/filemanager/file_interaction.h#L37-L43) | `validFileName()`. |

### B. Process-Specific Dependencies (Must Stay in the Unprivileged GUI Process)

These live inside [file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp) today and **must remain in `FileOperationManager`**, never compiled into `nythera-admin-helper`:

- **Session D-Bus JobView integration:** `g_bus_get_sync(G_BUS_TYPE_SESSION, ...)`, `org.kde.JobViewServer` / `JobViewV3` (`registerJob`, `publishJob`, `finishJob`, [file_operation_manager.cpp:1199-1204, 1718-1890](../src/filemanager/file_operation_manager.cpp#L1199-L1204)).
- **GLib main-loop UI dispatch & timers:** `g_idle_add` progress/pause callbacks, `450ms` progress reveal timer, `100ms` `TransferRates` timer, `GTask` thread pool, `m_workerDrain`, `m_activePaths` / `usesMount()`.
- **Undo/Redo history stacks:** `m_undo`, `m_redo`, `replay()`, `ReplayParent` (disabled for Admin Mode).
- **Wastebin / GVfs `trash://`:** [wastebin_fallback.{h,cpp}](../src/filemanager/wastebin_fallback.h), `Kind::Trash`, `Kind::TrashToHome`, `Kind::Restore`, `Kind::EmptyWastebin` (disabled for Admin Mode).
- **Properties Polkit client:** `runPropertyRequest()` -> [property_operation_client.cpp](../src/filemanager/property_operation_client.cpp) -> `polkit-gobject-1` (`Kind::Properties`).
- **`BrowserTab` / `AdminSessionClient` coupling:** `#include "filemanager/browser_tab.h"` (currently pulled in only for `copyAsAdministrator()`).

### C. UI-Specific Code (Stays Unprivileged in GUI)

- [ModalDialogHost](../src/filemanager/dialogs/modal_dialog_host.cpp) (conflict dialog rendering, permanent delete prompt, quit confirmation)
- [ProgressWindow](../src/filemanager/dialogs/progress_window.cpp)
- [FileTransferController](../src/filemanager/file_transfer_controller.cpp), [FileMenuController](../src/filemanager/file_menu_controller.cpp), [FileManager](../src/filemanager/file_manager.cpp), [TabManager](../src/filemanager/tab_manager.cpp), [BrowserTab](../src/filemanager/browser_tab.cpp), [DirectorySession](../src/filemanager/directory_session.cpp)

**Answer to 3.A:** 100% of the local operation engine (Copy, Move, Duplicate, Rename, Create, Delete, conflict loop, staging, NFS fallbacks, backup/recovery) can be shared with **zero duplicated filesystem logic**, once separated from `FileOperationManager`'s D-Bus/Wastebin/Properties/Undo/GTask orchestration shell.

---

## 3. Migration Map

| File | Disposition | Specific Functions / Changes |
| --- | --- | --- |
| **New:** `src/filemanager/file_operation_engine.{h,cpp}` | **Add (extracted)** | Pure local execution engine extracted from [file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp) (`executeLocalOperation()`): accepts `LocalOperationRequest` (kind, sources, destination, `renameIdentity`, `autoNumber`, `keepBoth`, `recordHistory`), `GCancellable*`, `FileTransferPauseState*`, `ProgressSink`, and `ConflictCallback` (which receives engine-populated `ConflictMetadata`). Returns `LocalOperationResult`. |
| [src/filemanager/file_operation_manager.h](../src/filemanager/file_operation_manager.h) & [file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp) | **Change** | Delegate local `worker()` ops (`Copy`, `Duplicate`, `Move`, `Rename`, `CreateFolder`, `CreateFile`, `CreateSymlink`, `Delete`) to `executeLocalOperation()`. Replace `copyAsAdministrator()` / `m_adminCopyControls` / `AdminCopyOrigin` with a general optional `BrowserTab* adminTab` parameter on mutating entrypoints (`copy`, `duplicate`, `move`, `renameChecked`, `createFolder`, `createFile`, `createFileFromTemplate`, `createSymlink`, `removePermanently`) and `std::optional<AdminOperationOrigin> adminOrigin` on `Operation`. In `worker()`, when `adminTab` is set, dispatch via `AdminSessionClient::runOperation(...)` instead of executing locally. Update `ConflictResolver` signature to receive optional pre-queried `ConflictMetadata`. |
| [src/filemanager/dialogs/modal_dialog_host.h](../src/filemanager/dialogs/modal_dialog_host.h) & [modal_dialog_host.cpp](../src/filemanager/dialogs/modal_dialog_host.cpp) | **Change** | In [makeConflictResolver()](../src/filemanager/dialogs/modal_dialog_host.cpp#L186-L434), build `ConflictDialogData` from engine-provided `ConflictMetadata` when present (avoiding unprivileged `g_file_query_info` / `g_file_query_file_type` on `0700 root:root` paths). In [closeDialog()](../src/filemanager/dialogs/modal_dialog_host.cpp#L599-L611), skip unprivileged `std::filesystem::exists()` for privileged conflicts (validate lexical `validFileName(name)` only; let the engine authoritatively reject collisions). |
| [src/filemanager/admin_protocol.h](../src/filemanager/admin_protocol.h) & [admin_protocol.cpp](../src/filemanager/admin_protocol.cpp) | **Extend (keep legacy dormant)** | Bump `kAdminProtocolVersion` from `5` to `6`. Add shared-op frames: `OpBegin` / `OpSource` / `OpExec` (or `RunOp` + chunked `OpSource`), `OpPause`, `OpResume`, extended `Conflict` (with incoming/existing type, size, mtime, content-type, `sameItem`, `hasRemaining`), `ConflictReply` (already reserved at [admin_protocol.cpp:390](../src/filemanager/admin_protocol.cpp#L390), add `applyToAll` flag), `OpCompletedEntry`, `OpMovedSource`, `OpResult`. **Retain** `CreateFile`, `CreateDir`, `RenameEntry`, `CopyFile`, `CopyItems`, `MutationResult`, `CopyItem` codecs unmodified. |
| [src/filemanager/admin_helper_session.h](../src/filemanager/admin_helper_session.h) & [admin_helper_session.cpp](../src/filemanager/admin_helper_session.cpp) | **Extend (keep legacy dormant)** | In [applyAdminHelperProcessHardening()](../src/filemanager/admin_helper_session.cpp#L475-L497), add `setenv("GIO_USE_VFS", "local", 1)` and `setenv("GVFS_DISABLE_FUSE", "1", 1)`. Add the shared-operation worker thread + `eventfd` wakeup to [runAdminHelperSession()](../src/filemanager/admin_helper_session.cpp#L499) executing `executeLocalOperation()` under `ScopedCreationUmask(config.creationUmask)` (default `0022`). **Retain** `ListDir`, `StatInfo`, `StatVfs`, `CountDir` as active read-only commands. **Retain** `executeCopy`, `executeCreateFile`, `executeCreateDir`, `executeRenameEntry`, `openAsInvoker`, `CopyTreeStage`, `AuthorizedDirectory` unmodified in-tree (dormant from GUI). |
| [src/filemanager/admin_session_client.h](../src/filemanager/admin_session_client.h) & [admin_session_client.cpp](../src/filemanager/admin_session_client.cpp) | **Extend (keep legacy dormant)** | Add non-blocking `runOperation(...)` / `sendConflictReply(...)` / `sendOpPause(...)` / `sendOpResume(...)` multiplexed on the existing `WorkerContext` `poll()` loop. **Retain** legacy `createFileAsync`, `createDirectoryAsync`, `renameEntryAsync`, `copyItems` as dormant methods (no GUI callers). |
| [src/filemanager/tab_presentation.h](../src/filemanager/tab_presentation.h) | **Change** | Update [TabCapabilities::forAdministrator(bool writable)](../src/filemanager/tab_presentation.h#L89-L108): enable `canMutate = writable`, `canCopyItems = true`, `canCreateBasic = writable`, `canCreate = writable` (for Text File/Duplicate/etc.; optionally keep templates/symlinks separately gated if undesired), `canRename = writable`; remove need for separate `canPasteAsAdministrator` flag (or make it alias `canMutate`). Keep `canTrash = false`, `canUndo = false`, `canLaunchApps = false`, `canRunExternalTools = false`, `canGenerateThumbnails = false`, `canChecksum = false`, `canPinToPlaces = false`, `canDragSource = false`, `canAcceptDrop = false`, `canOpenProperties = false`. |
| [src/filemanager/file_transfer_controller.cpp](../src/filemanager/file_transfer_controller.cpp) | **Simplify** | In [pasteClipboard()](../src/filemanager/file_transfer_controller.cpp#L247-L365), replace the separate `adminPaste`-only copy branch with unified Copy/Move dispatch passing `BrowserTab* adminTab = active.administratorBrowsing ? tab : nullptr` to `m_operations.copy(...)` / `m_operations.move(...)`. In [recordPasteCompletion()](../src/filemanager/file_transfer_controller.cpp#L367-L440) and [applyPendingPasteSelection()](../src/filemanager/file_transfer_controller.cpp#L442-L484), short-circuit directory equivalence via direct path equality when `tab.hasActiveAdminSession()` (avoiding unprivileged `m_places.compareDirectoriesAsync()`). |
| [src/filemanager/file_manager.h](../src/filemanager/file_manager.h) & [file_manager.cpp](../src/filemanager/file_manager.cpp) | **Simplify** | Remove active GUI use of `m_adminInlineRenameTarget`, `m_adminMutationsInFlight`, and the bespoke `renameEntryAsync` / `createFileAsync` / `createDirectoryAsync` branches ([#L1016-L1122](../src/filemanager/file_manager.cpp#L1016-L1122), [#L1909-L1987](../src/filemanager/file_manager.cpp#L1909-L1987)). Route inline rename (using `FileEntry` identity when present, fixing unprivileged `lstat`), Create New (`autoNumber = true` in engine, fixing unprivileged `creationDestination`), Duplicate, and Permanent Delete through `m_operations` with `adminTab`. Unify the [m_operations completion callback](../src/filemanager/file_manager.cpp#L493-L650) for `operation.adminOrigin` (including multi-tab refresh, selection reveal, and `m_inlineRenameTarget`/`m_createRequests`). Ensure window close ([#L1260-L1262](../src/filemanager/file_manager.cpp#L1260-L1262)) does not tear down `m_tabs` prematurely if background admin ops are allowed, OR explicitly cancels admin ops cleanly on close. |
| [src/filemanager/file_menu_controller.cpp](../src/filemanager/file_menu_controller.cpp) | **Minor update** | Update [newItemMenu()](../src/filemanager/file_menu_controller.cpp#L442-L455) and `editMenuEntries()` / `openContextMenu()` to reflect newly enabled shared operations (`Copy`, `Cut`, `Paste`, `Duplicate`, `Delete Permanently`, `Text File…`), while keeping `Move to Wastebin`, `Undo`/`Redo`, `Open With`, `Properties`, `Add to Places`, and `Tools` launchers disabled in Admin Mode. |
| [meson.build](../meson.build#L491-L521), [#L1291-L1299](../meson.build#L1291-L1299) | **Change** | Add `src/filemanager/file_operation_engine.cpp` to `nythera_core_lib`. In `nythera-admin-helper`, add `file_operation_engine.cpp` and `fs_compat.cpp` to `sources:` and change `dependencies: glib_dep` to `dependencies: gio_dep`. |
| [src/filemanager/fs_compat.{h,cpp}](../src/filemanager/fs_compat.cpp) | **Retain unchanged** | Already pure POSIX + GLib/GIO, already handles NFS fallbacks & backup recovery. |
| [src/filemanager/polkit_helper_launch.{h,cpp}](../src/filemanager/polkit_helper_launch.cpp), [data/polkit/dev.nythera.Files.admin-session.policy.in](../data/polkit/dev.nythera.Files.admin-session.policy.in), [src/filemanager/admin_helper_main.cpp](../src/filemanager/admin_helper_main.cpp) | **Retain unchanged** | Existing `pkexec --disable-internal-agent`, `installedFile()` verification, dedicated Polkit action (`auth_admin`), `PKEXEC_UID` validation, and `SIGTERM` handling all remain intact. |
| [src/filemanager/directory_session.{h,cpp}](../src/filemanager/directory_session.cpp), [src/filemanager/browser_tab.{h,cpp}](../src/filemanager/browser_tab.cpp), [src/filemanager/tab_manager.{h,cpp}](../src/filemanager/tab_manager.cpp), [src/filemanager/dialogs/progress_window.{h,cpp}](../src/filemanager/dialogs/progress_window.cpp), [src/filemanager/permanent_delete_prompt.h](../src/filemanager/permanent_delete_prompt.h) | **Retain unchanged** | Privileged browsing (`LIST_DIR`, `STAT_INFO`, `STATVFS`, `COUNT_DIR`), per-tab admin lifecycle, Finish/expiry recovery, warning header/tab visuals, `[Administrator]` title, and `ProgressWindow` all remain intact. *(Optional minor change in `browser_tab.cpp:119` only if selection symlinks are enabled in Admin Mode.)* |
| [docs/NYTHERA-CUSTOMISATIONS.md](../docs/NYTHERA-CUSTOMISATIONS.md#L43-L55) | **Update** | Update the Administrator Mode register entry to document shared-engine execution and protocol v6. |

---

## 4. Execution Design

### Comparison with KDE `kio-admin` (and Why Nythera's GIO Adaptation Differs)

- **What `kio-admin` does:** Dolphin stays unprivileged and accesses `admin:///` via `kio_admin`, which proxies operations over system D-Bus to a Polkit-authorized root helper (`kio-admin-helper`). The helper rewrites `admin:///` to `file:///` and executes KDE's normal KIO filesystem backend in root scope rather than maintaining a second filesystem engine; progress and conflict prompts cross IPC back to Dolphin.
- **How Nythera adapts this pattern cleanly without a daemon or GVfs:**
  1. **Keep the existing private `pkexec` pipe helper (`nythera-admin-helper`), not system D-Bus:** Nythera already has per-tab `pkexec --disable-internal-agent` sessions with `<allow_active>auth_admin</allow_active>` (zero Polkit grant caching, instant revocation on Finish/tab close/crash via `stdin` EOF, zero system daemon surface).
  2. **Avoid GVfs inside root (`GIO_USE_VFS=local`):** Unlike KIO's explicit `file://` worker, GLib's `g_file_new_for_path()` goes through `g_vfs_get_default()`. After `clearenv()`, leaving `GIO_USE_VFS` unset risks loading `libgvfsdbus.so` and attempting root session D-Bus autolaunch. Setting `GIO_USE_VFS=local` and `GVFS_DISABLE_FUSE=1` in [applyAdminHelperProcessHardening()](../src/filemanager/admin_helper_session.cpp#L475-L497) guarantees GLib uses its in-process `GLocalFile` backend exclusively (identical to Nythera's test-isolation rule in `AGENTS.md`).
  3. **Run whole high-level operations in the helper, not per-syscall RPCs:** Unlike KIO's worker protocol, Nythera's filesystem engine (`executeLocalOperation`) is already a self-contained high-level operation runner taking a `GCancellable*`, progress callback, and conflict callback.

### End-to-End Architecture

```text
[ Unprivileged Nythera Files GUI ]
  FileManager / FileTransferController / ModalDialogHost / ProgressWindow
        │
        ▼
  FileOperationManager (orchestration, JobViewServerV2 D-Bus, m_activePaths, m_workerDrain, GTask)
        │
        ├─ Normal mode (adminTab == nullptr):
        │     GTask thread ──► executeLocalOperation() [GLocalFile + fs_compat]
        │                          └─► ModalDialogHost::makeConflictResolver()
        │
        └─ Administrator Mode (adminTab != nullptr):
              GTask thread (waits on frame queue + drives ModalDialogHost::ConflictResolver)
                    │  (non-blocking enqueue / event wakeup)
                    ▼
              AdminSessionClient transport worker (poll on stdin/stdout pipes; NEVER blocks on UI)
                    │  line-framed IPC over private pkexec pipes (<= 64 KiB/frame)
                    ▼
[ Privileged nythera-admin-helper (euid == 0 via pkexec, GIO_USE_VFS=local) ]
  runAdminHelperSession() poll() loop (handles HELLO, PING, LIST_DIR, STAT*, CANCEL,
                                       CONFLICT_REPLY, PAUSE/RESUME, SHUTDOWN, 10m idle timer, EOF)
        │  dispatches 1 active mutating op at a time
        ▼
  Helper operation worker thread ──► executeLocalOperation() [EXACT SAME GLocalFile + fs_compat]
        ├─► streams PROG frames via eventfd -> poll loop -> stdout
        ├─► emits CONFLICT (+ pre-queried ConflictMetadata) & waits on CV for CONFLICT_REPLY or GCancellable
        └─► emits OP_COMPLETED / OP_MOVED / OP_RESULT when finished or unwound
```

### Key Mechanics Preserving All Lifecycle Invariants

1. **Zero-deadlock conflict & cancellation bridge:**
   - **Helper side:** Running `executeLocalOperation()` on one dedicated worker thread inside `runAdminHelperSession()` keeps the helper's `poll()` loop continuously reading `inFd`. When `CANCEL`, `SHUTDOWN`, `EOF`, `SIGTERM`, or the 10-minute inactivity deadline hits, the `poll()` loop immediately calls `g_cancellable_cancel(opCancellable)`, calls `opPauseState->cancel()`, and signals the pending conflict condition variable. The worker thread unwinds at the next `g_file_copy` / `transferCheckpoint` / conflict boundary, cleans up staged `.nythera-partial-*` temps and restores backups, and exits; the `poll()` loop joins the thread before returning.
   - **GUI side:** `AdminSessionClient`'s transport loop **never** calls `ModalDialogHost`'s blocking `ConflictResolver` directly. Instead, `FileOperationManager::worker()` (already on a `GTask` thread with `DrainGuard` and `GCancellable*`) blocks on a small frame queue fed by `AdminSessionClient`. When a `CONFLICT` frame arrives, the `GTask` thread invokes the existing synchronous `data->conflictResolver(...)` (which opens the modal on the UI thread via `g_idle_add` and polls `cancellable` every 5ms), then queues `CONFLICT_REPLY` on `AdminSessionClient`. If `cancel()`, Finish, helper disconnect, or `FileOperationManager::shutdown()` occurs during a conflict prompt, `cancellable` is cancelled, `ModalDialogHost` closes the dialog within 5ms, `CANCEL` is sent to the helper, and `m_workerDrain->cv` drains cleanly without deadlock.
2. **Large batches within the 64 KiB frame cap:**
   - `OP_BEGIN` starts an operation envelope, followed by zero or more `OP_SOURCE` frames (chunking source paths to stay well under `kAdminMaxFrameBytes = 65536`), followed by `OP_EXEC`. Results stream `OP_COMPLETED_ENTRY` and `OP_MOVED_SOURCE` frames prior to `OP_RESULT`. This removes Stage 4.2's 256-item/64 KiB single-frame ceiling while preserving strict bounded frame validation.
3. **Inactivity timeout:**
   - The helper continues enforcing the negotiated 10-minute (60–3600s) timeout in `remainingTimeMs()`. Starting an op (`OP_EXEC`), sending a `CONFLICT_REPLY`, or sending `PAUSE`/`RESUME` renews `lastActivity` (meaningful user interaction). Additionally, an active transfer making forward byte progress should renew `lastActivity` periodically (or pause the idle clock while actively copying bytes, while still enforcing expiry once idle or abandoned at a conflict prompt — note: see Risk #6 below).
4. **Mount protection & unified completion:**
   - Because all Admin Mode mutations start via `FileOperationManager::start()` and run on `GTask` with `g_task_set_return_on_cancel(task, false)` ([file_operation_manager.cpp:1655](../src/filemanager/file_operation_manager.cpp#L1655)), `m_activePaths` / `usesMount()` protects mounts until the helper's worker has actually finished unwinding. `JobViewServerV2`, `ProgressWindow`, `TransferRates`, cut-clipboard cleanup, and paste selection reveal all work through one shared path.

---

## 5. Risks, Concrete Blockers & Security Trade-Offs

### Concrete Code Blockers Discovered (Must Be Fixed During Migration)

1. **Unprivileged filesystem queries in `ModalDialogHost` (Blocker for conflicts in `0700 root:root` dirs):**
   - [ModalDialogHost::makeConflictResolver()](../src/filemanager/dialogs/modal_dialog_host.cpp#L199-L286) runs unprivileged `g_file_query_info()` and `g_file_query_file_type()`. Inside root-only directories these return `G_FILE_TYPE_UNKNOWN`, collapsing every conflict into `TypeMismatch` and hiding Replace/Merge.
   - [ModalDialogHost::closeDialog()](../src/filemanager/dialogs/modal_dialog_host.cpp#L599-L611) calls unprivileged `std::filesystem::exists(m_destination.parent_path() / name, error)` and rejects the user's alternative filename whenever `error` is non-zero (`EACCES`).
   - **Fix:** Pass pre-queried `ConflictMetadata` from the engine and skip the unprivileged `exists()` check on privileged conflicts.
2. **Unprivileged UI-thread `exists()` / `lstat()` / `symlink_status()` prechecks:**
   - [file_creation.h:55](../src/filemanager/file_creation.h#L55) (`creationDestination` / `creationDestinationWithExtension`) calls unprivileged `std::filesystem::exists()` before starting `CreateFile`/`CreateFolder`.
   - [file_manager.cpp:1128](../src/filemanager/file_manager.cpp#L1128) (`beginRenamePath`) calls unprivileged `lstat()` instead of reusing `FileEntry::{device,inode}` already loaded at line 991.
   - [file_manager.cpp:920](../src/filemanager/file_manager.cpp#L920) (`showSymlinkDialog`) calls unprivileged `std::filesystem::symlink_status()`.
   - [admin_session_client.cpp:1444](../src/filemanager/admin_session_client.cpp#L1444) (`copyItems`) calls unprivileged `::lstat()` on every source path (resolved automatically by retiring `copyItems` from the GUI path).
   - **Fix:** Move auto-numbering into the shared engine (`autoNumber` flag on `CreateFile`/`CreateFolder`), reuse `FileEntry` identity in `beginRenamePath()`, and skip unprivileged `symlink_status()` in Admin Mode.
3. **Helper `umask(0077)` vs. shared-engine creation permissions:**
   - [applyAdminHelperProcessHardening()](../src/filemanager/admin_helper_session.cpp#L476) sets `umask(0077)`. Running `createDirectoryExclusive(..., 0777)`, `writeBufferToNewFileExclusive(..., 0666)`, or `g_file_make_directory()` under `0077` creates `0700`/`0600` items instead of standard `0755`/`0644`.
   - **Fix:** Wrap shared-engine execution in the helper's existing `ScopedCreationUmask(config.creationUmask)` (`0022`), which leaves `g_mkdtemp()` staging/backup dirs private (`0700`) while producing normal `0755`/`0644` created entries.
4. **Main-window close vs. background Admin Mode operations:**
   - [file_manager.cpp:1260-1262](../src/filemanager/file_manager.cpp#L1260-L1262) (`m_surface->setClosedCallback`) immediately calls `m_tabs.shutdownAll()`, which closes every tab's `AdminSessionClient` pipe immediately. Unlike normal mode (which continues active operations in `ProgressWindow` after main window close), closing the main window during an Admin Mode transfer will abort the helper immediately unless `FileManager` either (a) prompts via `requestQuit` when an admin operation is active, or (b) defers shutting down an admin tab's `AdminSessionClient` until its active `FileOperationManager` op finishes.
   - **Minimal safe choice:** Ensure main window close cancels/drains admin operations cleanly (never leaving a hanging `m_workerDrain`), and explicitly decide whether closing the main window aborts admin ops (matching per-tab privilege lifetime) or prompts confirmation.
5. **Paste selection reveal in restricted directories:**
   - [FileTransferController::applyPendingPasteSelection()](../src/filemanager/file_transfer_controller.cpp#L452-L484) calls `m_places.compareDirectoriesAsync()` (unprivileged canonicalization) when the destination path isn't cached; in Admin Mode this must short-circuit on normalized path equality (`normalizeTabPath(pending.request.destination) == normalizeTabPath(tab.session.path())`), matching [recordPasteCompletion()](../src/filemanager/file_transfer_controller.cpp#L397-L404).
6. **Inactivity timeout during long transfers:**
   - Current Stage 4.2 [checkStreamingCancelled()](../src/filemanager/admin_helper_session.cpp#L596-L600) expires a copy if it takes longer than 10 minutes even while actively transferring bytes, AND [testAdministratorCopyLifecycle()](../tests/admin_mode_stage3_test.cpp#L1046) specifically tests helper timeout during a blocked copy. Keep the exact policy explicit: either preserve strict hard timeout across operations (zero behavioral surprise to timeout tests) or renew `lastActivity` only on active I/O progress while keeping idle/stalled conflict prompts bounded by the 10-minute deadline.
7. **NFS compatibility & `root_squash`:**
   - Sharing `file_operation_engine` + [fs_compat.cpp](../src/filemanager/fs_compat.cpp) immediately fixes Stage 4.2's `supportedCopyFilesystem` whitelist ([admin_helper_session.cpp:121-139](../src/filemanager/admin_helper_session.cpp#L121-L139)) and `O_TMPFILE` requirement, restoring full normal-mode NFS fallback behavior (`RENAME_NOREPLACE` -> `linkat`/Option A, `allocateReplacementBackup` private container, `isAmbiguousNetworkErrno` recovery).
   - **Concrete NFS constraint:** On `root_squash` NFS exports (the Linux default), `euid == 0` is squashed to `nobody` by the server and will receive `EACCES`/`EPERM` (identical to `sudo` and `kio-admin`). `fs_compat::probeCapabilities()` handles `EACCES` on `g_mkdtemp` gracefully and falls through to normal error reporting.
8. **Global test hooks in `fs_compat.cpp`:**
   - [fs_compat.cpp:21-26](../src/filemanager/fs_compat.cpp#L21-L26) uses process-global test hook `std::function`s. In production `nythera-admin-helper` they are null (harmless). In unit tests that run `runAdminHelperSession()` in-process on a background thread, tests must only mutate hooks before launching helper threads (already standard in existing tests).

### Genuine Security Consequences of Shared Execution & Simple Sufficient Safeguards

Sharing the `GLocalFile` + `fs_compat` engine as `euid == 0` explicitly trades away Stage 4.2's custom fd-pinned `openat2` / `openAsInvoker` / `AuthorizedDirectory` guarantees (accepted by this architectural decision). Concrete consequences and minimal safeguards:

| Consequence of Running Shared `GLocalFile`+`fs_compat` as `root` | Simple Existing / Minimal Safeguard |
| --- | --- |
| **Path-based TOCTOU / symlink traversal in untrusted user-writable dirs:** `GLocalFile` (`g_file_copy`, `g_file_enumerate_children`, `removeTree`) and `fs_compat::PinnedEntry::open()` use path traversal without `openat2(RESOLVE_NO_SYMLINKS \| RESOLVE_BENEATH \| RESOLVE_NO_XDEV)`. | Existing `G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS` / `G_FILE_COPY_NOFOLLOW_SYMLINKS` prevents following leaf symlinks during copy/delete/stat; `fs_compat` already uses `RENAME_NOREPLACE` / `linkat` / `O_EXCL \| O_NOFOLLOW` / `0700` `g_mkdtemp` staging and inode-verified replacement backups. This matches the accepted Dolphin/`kio-admin` security model (single authenticated local admin user). |
| **Confused-deputy read of root-only sources:** Removing `openAsInvoker()` allows the helper to read any root-readable source path (required to support Copy/Move *from* `/root` or `/etc`). | Protected by the Polkit boundary (`dev.nythera.Files.admin-session`, `auth_admin`, no grant caching), per-tab explicit opt-in, `stdin` EOF death-pact, and 10-minute inactivity timeout. |
| **Potential setuid/setgid preservation or FIFO blocking in `g_file_copy` as `euid == 0`:** *(Runtime assumption — verify against host GLib)* Copying a setuid file as root may preserve `S_ISUID`/`S_ISGID` on a root-owned destination, and opening a FIFO/special file may block `g_file_copy`. | Optional 5-line guard inside `copyTree()` when `geteuid() == 0`: reject `G_FILE_TYPE_SPECIAL` (or let `copyTree` check `G_FILE_TYPE_SPECIAL` before `g_file_copy`) and strip `S_ISUID \| S_ISGID` on newly copied regular files unless `preserveDirectoryMetadata` (cross-fs Move). |
| **Accidental catastrophic delete on `/` or virtual filesystems (`/proc`, `/sys`, `/dev`):** `removeTree()` does not enforce `RESOLVE_NO_XDEV` or block virtual trees. | Keep a tiny, 10-line helper-side input guard before `executeLocalOperation()` for `Delete` / `Move`: reject `/`, reject `trash:` URIs, reject non-absolute/unnormalized paths, and optionally refuse `Delete` targeting `/proc`, `/sys`, `/dev` (or mount-crossing in `removeTree` only if desired — though `kio-admin` relies on user confirmation via `permanentDeletePrompt()`). |
| **GVfs / D-Bus exposure inside root helper:** Default GIO could probe GVfs / session bus. | Mandatory `setenv("GIO_USE_VFS", "local", 1)` and `setenv("GVFS_DISABLE_FUSE", "1", 1)` in `applyAdminHelperProcessHardening()` plus existing `clearenv()`, `PR_SET_DUMPABLE=0`, `PR_SET_NO_NEW_PRIVS=1`, `closeExtraFileDescriptors()`. |

---

## 6. Implementation Sequence (Least Expensive Safe Migration)

A direct migration is feasible with **one narrowly scoped prerequisite** (Step 1: extracting `file_operation_engine.{h,cpp}` from `file_operation_manager.cpp` so `nythera-admin-helper` does not pull in D-Bus, Polkit, Wastebin, or `BrowserTab`). No intermediate generic VFS abstraction layer is needed.

1. **Step 1 — Extract `file_operation_engine.{h,cpp}` (zero behavior change in normal mode):**
   - Move `FileTransferPauseState`, `measureTree`, `copyTree`, `removeTree`, `cleanupStagedTree`, `isSameOrDescendant`, `createItem`, `PrivateStaging`, and the local execution branches (`Create*`, `Rename`, `Copy`, `Duplicate`, `Move`, `Delete`) out of [file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp) into `src/filemanager/file_operation_engine.{h,cpp}`.
   - Add `ConflictMetadata` population inside the engine and an `autoNumber` option for `CreateFile`/`CreateFolder`.
   - Keep `FileOperationManager::worker()` calling `executeLocalOperation()` directly for normal mode.
   - Verify with existing `file_operation_manager`, `file_transfer_pause`, `file_manager_conflict_dialog`, and `file_wastebin_operation` tests.

2. **Step 2 — Fix `ModalDialogHost` conflict metadata & alternative-name validation:**
   - Update [ModalDialogHost::makeConflictResolver()](../src/filemanager/dialogs/modal_dialog_host.cpp#L186-L434) to accept engine-supplied `ConflictMetadata` (eliminating unprivileged `g_file_query_info` calls when provided) and update [closeDialog()](../src/filemanager/dialogs/modal_dialog_host.cpp#L599-L611) to skip unprivileged `std::filesystem::exists()` for privileged conflicts.

3. **Step 3 — Wire `file_operation_engine` + `fs_compat` into `nythera-admin-helper` & Protocol v6:**
   - Update [meson.build](../meson.build#L1291-L1299) (`nythera-admin-helper`: `gio_dep`, `file_operation_engine.cpp`, `fs_compat.cpp`).
   - Add `GIO_USE_VFS=local` and `GVFS_DISABLE_FUSE=1` to [applyAdminHelperProcessHardening()](../src/filemanager/admin_helper_session.cpp#L475-L497).
   - Extend [admin_protocol.{h,cpp}](../src/filemanager/admin_protocol.h) (v6: `OpBegin`/`OpSource`/`OpExec`/`OpPause`/`OpResume`/`Conflict`+metadata/`ConflictReply`+`applyToAll`/`OpCompletedEntry`/`OpMovedSource`/`OpResult`) while keeping all Stage 1–4.2 commands intact and unmodified.
   - Add the single-op worker thread + `eventfd` loop integration in [runAdminHelperSession()](../src/filemanager/admin_helper_session.cpp#L499) calling `executeLocalOperation()` under `ScopedCreationUmask(config.creationUmask)`.
   - Add non-blocking `runOperation()` / `sendConflictReply()` / `sendOpPause()` / `sendOpResume()` to [AdminSessionClient](../src/filemanager/admin_session_client.h).

4. **Step 4 — Unify `FileOperationManager`, `FileTransferController`, `FileManager`, `FileMenuController`, and `TabCapabilities`:**
   - Route all Admin Mode mutations through `FileOperationManager` (`worker()` `GTask` bridge -> `AdminSessionClient::runOperation()`).
   - Remove active GUI calls to legacy `createFileAsync`, `createDirectoryAsync`, `renameEntryAsync`, `copyItems`, and remove the parallel `m_adminInlineRenameTarget` / `m_adminMutationsInFlight` / `copyAsAdministrator` GUI paths (leaving legacy helper/protocol/client code dormant).
   - Update [TabCapabilities::forAdministrator()](../src/filemanager/tab_presentation.h#L89-L108), [FileTransferController](../src/filemanager/file_transfer_controller.cpp), [FileManager](../src/filemanager/file_manager.cpp), and [FileMenuController](../src/filemanager/file_menu_controller.cpp).
   - Handle main-window close cleanly when an admin operation is active.

5. **Step 5 — Tests & Documentation:**
   - Update UI workflow assertions in [tests/admin_mode_stage3_test.cpp](../tests/admin_mode_stage3_test.cpp) (keep legacy helper-level tests passing; add shared-engine Copy/Move/Delete/Conflict/Pause/Cancel/Disconnect/NFS-fallback tests).
   - Update [docs/NYTHERA-CUSTOMISATIONS.md](../docs/NYTHERA-CUSTOMISATIONS.md#L43-L55).

---

## 7. Validation

### Focused Meson Tests (Unprivileged, Isolated per `AGENTS.md`)

Run incremental `just build`, then targeted `meson test -C build-debug <name> --print-errorlogs` for:

- **Shared engine & normal-mode regression:**
  - `file_operation_manager` (verifies normal Copy/Move/Rename/Create/Delete/Undo/NFS `fs_compat` fallbacks & replacement backup recovery are unchanged)
  - `file_transfer_pause`
  - `file_manager_conflict_dialog`
  - `file_manager_paste_selection`
  - `file_wastebin_operation` (confirms Wastebin paths remain intact)
  - `file_inline_rename`
- **Administrator Mode lifecycle, protocol & shared-engine integration:**
  - `admin_session_stage1` (handshake, Polkit verification, inactivity timeout, clean shutdown, EOF)
  - `admin_mode_stage2` (browsing, per-tab isolation, Finish/expiry recovery, warning UI)
  - `admin_mode_stage3` (keep legacy helper tests passing + update/extend UI integration tests for shared-engine Copy, Cut/Move, Duplicate, Create Folder/File/Text File, Rename, Permanent Delete, conflict round-trip including Replace/Merge/Keep Both/Skip/Apply-to-All/Cancel, mid-conflict Cancel/Finish/EOF/shutdown drain without deadlock, and `fs_compat` `RENAME_NOREPLACE`-unsupported fallback inside helper).
  - `file_manager_tabs`

### Manual Live Acceptance (Authorised Install Environment)

After installing the root-owned helper and updated protocol v6 policy (`meson install --tags admin-helper` + matching `nythera-file-manager`):

1. **Polkit & UI lifecycle:** *Tools -> Act as Administrator* and folder right-click *Open as Administrator* prompt Polkit; verify warning header, shield, Finish button, `[Administrator]` title, and 10-minute inactivity expiry.
2. **Root-only directory operations (`/root` or a `0700 root:root` test directory):**
   - Browse, Create Folder/Empty File/Text File (verify `0755`/`0644` permissions), Inline Rename (`F2`) with collision rejection.
   - Copy/Paste both *into* and *from* root-only directory (files, nested folders, symlinks), Cut/Move (same-fs and cross-fs), Duplicate, and Permanent Delete (`Shift+Delete` confirmation).
   - Trigger every conflict type inside a `0700 root:root` directory (File Replace/Keep Both with typed alternative name/Skip, Folder Merge, Type Mismatch, Apply to All) and verify metadata cards display accurately.
   - Test Pause/Resume, mid-transfer Cancel, Finish mid-conflict, tab close mid-transfer, and window close mid-transfer (verify no deadlocks and no leftover `.nythera-partial-*` or `.nythera-backup-*` files).
3. **Privilege containment:** Confirm Wastebin, Undo/Redo, Open/Open With on files, External Tools, Properties, Checksum, Add to Places, and DnD remain disabled in Admin Mode; confirm new tabs/windows open unprivileged.
4. **NFS check (if available):** Verify normal-mode NFS Copy/Move/Rename unchanged, and verify expected behavior on NFS (`no_root_squash` works via `fs_compat`; `root_squash` surfaces clean `EACCES`).

---

## 8. Estimated Effort

**Overall complexity: Moderate** (dramatically lower than building/maintaining a parallel fd-relative Move/Delete/Conflict/NFS engine in `admin_helper_session.cpp`, which would be High–Very High and double maintenance cost permanently).

**Main sources of implementation cost:**
1. **Clean extraction of `file_operation_engine.{h,cpp}` (~30%):** Untangling `FileOperationManager::worker()`'s local branches from `Task`/`Result`/D-Bus/Wastebin/History without subtly altering any normal-mode edge case or breaking `tests/file_operation_manager_test.cpp`.
2. **Helper worker thread + protocol v6 multiplexing & cancellation/conflict drain (~35%):** Adding the non-blocking `eventfd`-driven single-op thread in `runAdminHelperSession()`, chunked source streaming, `ConflictMetadata` serialization, and deadlock-free `GTask` <-> `AdminSessionClient` bridge.
3. **GUI unification & `ModalDialogHost` unprivileged-query fixes (~20%):** Updating `ModalDialogHost`, unifying `FileManager`/`FileTransferController`/`FileMenuController`/`TabCapabilities`, and retiring active GUI calls to legacy admin mutations.
4. **Test updates & edge-case coverage (~15%):** Updating Stage 3 UI tests for newly enabled capabilities and adding conflict/cancel/EOF/Move/Delete/NFS-fallback regression tests.

---

## Verified Findings vs. Assumptions

### Verified Directly from Repository Source
- `FileOperationManager` local execution (`copyTree`, `measureTree`, `removeTree`, `createItem`, `PrivateStaging`, `FileTransferPauseState`, conflict loop, and `fs_compat.cpp`) has **zero UI, Wayland, Render, or Polkit dependencies** — only GLib/GIO and POSIX.
- `nythera-properties-helper` already links `gio_dep` ([meson.build:1277](../meson.build#L1277)); `nythera-admin-helper` currently links `glib_dep` ([meson.build:1297](../meson.build#L1297)).
- `FileOperationManager::start()` already accepts `filemanager::BrowserTab* adminTab = nullptr` ([file_operation_manager.h:191](../src/filemanager/file_operation_manager.h#L191)).
- `admin_protocol.{h,cpp}` already reserves `CONFLICT` and `CONFLICT_REPLY` frame kinds ([admin_protocol.h:67, 167](../src/filemanager/admin_protocol.h#L67)).
- [ModalDialogHost::makeConflictResolver()](../src/filemanager/dialogs/modal_dialog_host.cpp#L199-L286) and [ModalDialogHost::closeDialog()](../src/filemanager/dialogs/modal_dialog_host.cpp#L603) currently run unprivileged filesystem queries that fail on `0700 root:root` directories.
- [file_creation.h:55](../src/filemanager/file_creation.h#L55), [file_manager.cpp:920](../src/filemanager/file_manager.cpp#L920), [file_manager.cpp:1128](../src/filemanager/file_manager.cpp#L1128), [admin_session_client.cpp:1444](../src/filemanager/admin_session_client.cpp#L1444), and [file_transfer_controller.cpp:458](../src/filemanager/file_transfer_controller.cpp#L458) contain concrete unprivileged filesystem checks that must be bypassed or moved into the engine.
- [applyAdminHelperProcessHardening()](../src/filemanager/admin_helper_session.cpp#L475-L497) calls `clearenv()` and `umask(0077)` and does not currently set `GIO_USE_VFS=local`.
- [file_manager.cpp:1262](../src/filemanager/file_manager.cpp#L1262) shuts down all tabs immediately when the main window closes.
- [fs_compat.cpp](../src/filemanager/fs_compat.cpp) already implements normal-mode NFS fallbacks (`RENAME_NOREPLACE` -> `linkat`/Option A, private backup containers, ambiguous network errno recovery).
- [permanent_delete_prompt.h](../src/filemanager/permanent_delete_prompt.h) is purely lexical (no filesystem I/O).

### Assumptions / Items Requiring Runtime Verification During Implementation
- **GLib `g_file_copy()` under `euid == 0`:** Exact host-GLib behavior for copying `S_ISUID`/`S_ISGID` bits and `G_FILE_TYPE_SPECIAL` (FIFOs/sockets/devices) when `euid == 0` without `G_FILE_COPY_ALL_METADATA` was not executed on this host and should be verified (or explicitly guarded in `copyTree()`) during implementation.
- **KDE `kio-admin` internals:** Summarized from public documentation/web references rather than a local checkout of `invent.kde.org/system/kio-admin`; Nythera's execution design does not depend on undocumented `kio-admin` details.
- **Live Polkit / root-only / NFS acceptance:** Cannot be verified without privileged installation (`meson install --tags admin-helper`) and a live desktop Polkit agent.
