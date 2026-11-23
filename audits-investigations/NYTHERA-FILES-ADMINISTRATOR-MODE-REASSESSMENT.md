# Nythera Files — Administrator Mode Reassessment

- **Repository:** `Kangy1103/Nythera`
- **Branch:** `dev` (`1636655f9`)

**Scope:** Read-only reassessment against current `dev` (`1636655f9`). No code, policies, or dependencies were modified.
*(Note: the transcript of the earlier Administrator Mode investigation is outside this conversation's context, so this comparison evaluates the pre-F-01–F-11 architectural blockers and privileged-helper prerequisites—verified from `git log` `1da78005a..926e8a6b5` and [FILE-PROPERTIES.md](../docs/FILE-PROPERTIES.md)—against the current post-F-11 implementation.)*

---

## Executive Verdict

**Roughly 55–60% of the structural and safety groundwork is now complete, and it is reasonable to proceed with a tightly scoped Administrator Mode MVP.**

F-01 through F-11 plus the existing Polkit Properties helper (`nythera-properties-helper`) eliminated the most dangerous foundational blockers: monolithic UI coupling, blocking UI-thread I/O, unsafe destination clobbering, async use-after-free/shutdown races, spoofing in conflict UI, and the absence of a vetted non-setuid Polkit helper pattern.

What remains is **concentrated and well-bounded**, not a whole-app rewrite:
1. Introducing a minimal filesystem backend seam in `DirectorySession` and `FileOperationManager` (plus fixing 3 small remaining direct `lstat`/`exists` calls and passing backend-supplied metadata into `ModalDialogHost::makeConflictResolver()`).
2. Building a **private, pipe-bound, session-scoped `pkexec` helper** (`nythera-admin-helper`) backed by fd-relative `openat2` mutation primitives—**never** reusing path-based `GFile` mutations as root and **never** elevating the GUI or child apps.
3. Enforcing strict Administrator Mode UX and capability guardrails (banner, timeout, disabled root app launch / root thumbnails / root Wastebin / root Undo).

---

## 1. Original Blockers vs. Current `dev` Status

| Original Blocker / Prerequisite | Current `dev` Status | Evidence in Current Codebase |
| --- | --- | --- |
| **Monolithic `FileManager` impossible to isolate for a privileged mode** | **Satisfied (F-11)** | `926e8a6b5` extracted 8 controllers: [directory_session.h](../src/filemanager/directory_session.h), [file_menu_controller.h](../src/filemanager/file_menu_controller.h), [file_transfer_controller.h](../src/filemanager/file_transfer_controller.h), [modal_dialog_host.h](../src/filemanager/dialogs/modal_dialog_host.h), [progress_window.h](../src/filemanager/dialogs/progress_window.h), [places_sidebar.h](../src/filemanager/places_sidebar.h), [file_list_header.h](../src/filemanager/file_list_header.h), [file_manager_settings.h](../src/filemanager/file_manager_settings.h). `FileManager` is now primarily a coordinator. |
| **Unsafe destination replacement, TOCTOU overwrites & cross-fs partial moves** | **Satisfied for local ops (F-02–F-03); pattern ready for root port** | `1da78005a` & `ceea83de7`: `renameWithoutReplaceErrno()` (`SYS_renameat2` + `RENAME_NOREPLACE`), `allocateReplacementBackup()` with pre/post `verifyReplaceableTarget()` inode snapshots, `PrivateStaging` (`0700`), `ReplayParent` pinned dirfd, split-state backup preservation ([file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp#L551-L705)). **Gap:** still uses `GFile` / `AT_FDCWD` paths rather than fd-relative `openat2` primitives required for `euid == 0`. |
| **Blocking UI-thread filesystem I/O** | **Mostly satisfied (F-04); 3 small local leaks remain** | `5f600adce` moved `statvfs`, selection info, directory writability, `FileIcon`, Places path resolution, DnD validation, and post-paste identity off the UI thread. **Remaining synchronous calls:** `lstat()` in [beginRenamePath()](../src/filemanager/file_manager.cpp#L681), `symlink_status()` in [showSymlinkDialog()](../src/filemanager/file_manager.cpp#L619), and `std::filesystem::exists()` loop in [file_creation.h](../src/filemanager/file_creation.h#L55). |
| **Async worker / callback lifetime & shutdown races** | **Satisfied (F-01, F-06)** | `04eeea269` & `ceea83de7`: `m_alive` weak guards across all controllers, `FileOperationManager::shutdown()` cancels all `GCancellable`s and drains `m_workerDrain->cv` ([file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp#L1186-L1234)), `g_task_set_return_on_cancel(task, false)` preserves mount protection until unwind completes ([file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp#L1606-L1610)). |
| **Conflict UI parsing fragility & filename spoofing** | **Partially satisfied (F-07)** | `18b9b13d4`: structured `ConflictDialogData` / `ConflictItemInfo` and centralized `sanitizeFilenameForDisplay()`. **Remaining gap:** [makeConflictResolver()](../src/filemanager/dialogs/modal_dialog_host.cpp#L191-L278) directly calls unprivileged `g_file_query_info()` on `source` and `destination`, which fails with `EACCES` inside `0700 root:root` directories. |
| **Untrusted file decoding & thumbnailing risks** | **Satisfied (F-05) for local; must be disabled for admin paths** | `44643b4a9`: hardened `ThumbnailService` (`O_NOFOLLOW`, private `0700` cache, bounded decoders). For Admin Mode, thumbnails and `.directory` reads ([file_icon.h](../src/ui/dialogs/file_icon.h#L325)) must remain **strictly unprivileged** (degrading cleanly to lexical/MIME icons for restricted paths). |
| **Template creation, `.desktop` Exec injection, shared Places guard** | **Satisfied (F-08–F-10)** | `32c1ce664`: template creation uses `FileOperationManager::createFileFromTemplate()`; `.desktop` Exec parsing hardened; `PlacesModel` uses per-instance `OperationLease`. |
| **Unprivileged GUI + Polkit helper privilege-separation architecture** | **Proven for one-shot Properties; not yet built for interactive Admin session** | `nythera-properties-helper` ([property_helper_main.cpp](../src/filemanager/property_helper_main.cpp), [property_operation_client.cpp](../src/filemanager/property_operation_client.cpp), [property_operations.cpp](../src/filemanager/property_operations.cpp)) and [dev.nythera.Files.modify-properties.policy.in](../data/polkit/dev.nythera.Files.modify-properties.policy.in) establish the exact trust model, `openat2` traversal, `pkexec --disable-internal-agent` launch, `installedFile()` verification, and `stdin` EOF death-pact. **Missing:** session-scoped helper & general fs operations. |
| **Filesystem backend abstraction (local vs. privileged)** | **Outstanding** | `DirectorySession` and `FileOperationManager` hard-code local GIO/POSIX calls (aside from `Kind::Properties` and `WastebinBackend`). |
| **Admin session UX, lifecycle, & capability restrictions** | **Outstanding** | No Admin Mode state, visual warning banner, idle timeout, exit action, or menu/action capability gating (comparable to existing `m_session.browsingTrash()` gates). |

---

## 2. Existing Infrastructure We Can Directly Reuse

1. **Polkit privilege-boundary template (zero new auth mechanism needed):**
   - Nythera Shell already provides a native Polkit agent ([polkit_agent.cpp](../src/dbus/polkit/polkit_agent.cpp)) and works with any standard desktop agent when Files runs standalone.
   - [property_operation_client.cpp](../src/filemanager/property_operation_client.cpp#L12-L96) already implements:
     - `installedFile()` rejecting user-writable install prefixes, symlinks, or non-root helper/policy files.
     - Explicit `polkit_authority_enumerate_actions_sync()` verification of the dedicated action ID and `org.freedesktop.policykit.exec.path` annotation (no fallback to generic `org.freedesktop.policykit.exec`).
     - `/usr/bin/pkexec --disable-internal-agent` execution via `g_subprocess_newv()`.
     - Stdin readiness handshake (`"G"`) + `GCancellable` -> `g_output_stream_close(stdin)` EOF cancellation/death channel ([property_helper_main.cpp](../src/filemanager/property_helper_main.cpp#L21-L42)).
   - Meson already wires `polkit_gobject_dep`, `policydir` discovery, configured policy templates, and `--tags` helper installation ([meson.build](../meson.build#L1252-L1267)).

2. **`openat2` fd-pinned traversal & kernel-safe primitives:**
   - [property_operations.cpp](../src/filemanager/property_operations.cpp#L57-L68) and [property_operations.cpp](../src/filemanager/property_operations.cpp#L258-L435) already provide `scopedOpen()` (`O_PATH | O_CLOEXEC | O_NOFOLLOW` with `RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_BENEATH | RESOLVE_NO_XDEV`), `/proc/self/fd/<n>` pinning, depth (`128`) and entry (`1,000,000`) bounds, hardlink/mount-crossing exclusion, and `flock` inode locking. These directly inform the root helper's traversal and delete/copy engine.

3. **Job orchestration, progress, cancellation, & mount safety:**
   - `FileOperationManager` already provides `GTask` workers, `GCancellable`, `FileTransferPauseState`, `TransferRates`, `m_activePaths` / `usesMount()`, `m_workerDrain` shutdown draining, `org.kde.JobViewServerV2` notifications, and automatic `ProgressWindow` fallback ([progress_window.cpp](../src/filemanager/dialogs/progress_window.cpp#L92-L153)).
   - `FileManager`'s completion callback ([file_manager.cpp](../src/filemanager/file_manager.cpp#L229-L276)) already handles status updates, refresh after mutating failed/cancelled operations, capacity refresh, and `FileTransferController::registerPasteRequest()` selection reveal.

4. **Native dialogs & UI guards:**
   - `ModalDialogHost` already renders sanitized conflict cards (F-07), `permanentDeletePrompt()`, and `requestQuit()` active-operation confirmation.
   - `FileMenuController` already models a restricted namespace cleanly via `m_session.browsingTrash()` (disabling unsupported actions in both context and app menus without leaving broken separators).
   - `FileEntry` already carries `device`, `inode`, `hasIdentity`, `isSymlink`, and `type` from enumeration ([directory_session.cpp](../src/filemanager/directory_session.cpp#L17-L44)).

---

## 3. Reassessing the Polkit Helper Design: Pipe-Bound Session Helper vs. Alternatives

With `nythera-properties-helper` already proven in-tree, the key design choice for Administrator Mode is **how privilege is scoped across browsing and operations**:

| Option | Security & Lifecycle Trade-Off | Fit for Nythera Files |
| --- | --- | --- |
| **A. Private pipe-bound `pkexec` session helper (`nythera-admin-helper`)** | Spawned per window on explicit *Enter Administrator Mode* via `/usr/bin/pkexec --disable-internal-agent` with dedicated action `dev.nythera.Files.admin-session` (`<allow_active>auth_admin</allow_active>`, no cached Polkit retention). Communicates exclusively over `stdin`/`stdout` (since `pkexec` closes fds > 2). Closing Admin Mode, window close, GUI crash, or helper-enforced idle timeout (`POLLHUP`/timer) immediately terminates the root helper. Zero system D-Bus daemon, zero multi-client exposure, zero systemd unit. | **Recommended.** Directly extends `nythera-properties-helper`'s vetted trust model and keeps standalone separation trivial. |
| **B. System D-Bus daemon (`kio-admin` / `gvfsd-admin` model)** | Requires root systemd/D-Bus activation, `/usr/share/dbus-1/system.d/*.conf`, `auth_admin_keep` caching, and per-caller Polkit authorization checks on a shared root daemon. (`gvfs-backend-admin` is not even installed on this host.) | Heavyweight; unnecessary attack surface and packaging overhead for Nythera Files. |
| **C. One-shot `pkexec` per operation** | No persistent root process, but cannot browse `0700 root:root` directories and either re-prompts Polkit on every step or relies on `auth_admin_keep`. | Already right for single-action Properties; insufficient for browsing Administrator Mode. |

**Why Model A fits cleanly now:**
- The unprivileged GUI never handles passwords, never runs with `euid == 0`, and never shares a root daemon across windows.
- Because `<allow_active>auth_admin</allow_active>` does not cache Polkit grants, clicking **Exit Administrator Mode** (closing `stdin`) immediately revokes privilege with zero lingering grace period.
- The helper itself (trusted side) enforces the idle inactivity timer (e.g. 5–10 minutes of no requests), rejects any command after timeout or EOF, validates every path/inode/flag independently, and runs with restrictive `umask(0077)` for temporary staging.

---

## 4. Remaining Genuine Blockers

### 4.1 Architectural & Backend Seams (Essential)

1. **No backend abstraction in `DirectorySession`:**
   - [directory_session.cpp](../src/filemanager/directory_session.cpp#L221-L490) hard-codes `g_file_enumerate_children_async`, `g_file_query_info_async` (`ACCESS_CAN_WRITE`), `g_file_monitor_directory`, and `countImmediateFolderItems`.
   - Needs a narrow read/enumerate seam (e.g., `DirectoryBackend` or session-delegated callbacks) returning `std::vector<FileEntry>` + writability + optional stat/free-space, with `GFileMonitor` skipped when running on the admin backend (relying on explicit post-op refresh + Ctrl+R).
2. **No privileged execution path in `FileOperationManager` for core mutations:**
   - [file_operation_manager.cpp](../src/filemanager/file_operation_manager.cpp#L1793-L2949) runs `Copy`, `Move`, `Rename`, `CreateFolder`, `CreateFile`, `CreateSymlink`, and `Delete` via direct `GFile`/POSIX calls.
   - Needs a clean per-request dispatch path to `AdminSessionClient` when an operation targets the active admin session, while keeping existing `GTask`, `TransferProgress`, `m_activePaths`, `m_workerDrain`, and `completed()` lifecycle intact.
3. **Unprivileged filesystem leaks in UI & conflict paths:**
   - [ModalDialogHost::makeConflictResolver()](../src/filemanager/dialogs/modal_dialog_host.cpp#L191-L278): must accept optional pre-queried `ConflictItemInfo` from the backend instead of calling `g_file_query_info()` directly (otherwise conflicts inside `0700 root:root` dirs degrade to `G_FILE_TYPE_UNKNOWN` / `TypeMismatch`).
   - [FileManager::beginRenamePath()](../src/filemanager/file_manager.cpp#L681): reads `lstat()` synchronously on the UI thread instead of using `FileEntry::{device,inode}` (or backend stat).
   - [FileManager::showSymlinkDialog()](../src/filemanager/file_manager.cpp#L619) and [creationDestinationWithExtension()](../src/filemanager/file_creation.h#L55): do synchronous `symlink_status()` / `exists()` checks on the UI thread; in Admin Mode, free-name allocation and collision checks must run inside the helper atomically (`O_CREAT | O_EXCL` / `mkdirat` / `symlinkat`), never via unprivileged pre-checks.
   - [FileManager::queryStatusFreeSpaceAsync()](../src/filemanager/file_manager.cpp#L1647) and [querySelectionInfoAsync()](../src/filemanager/file_manager.cpp#L1729): need to delegate through the active session backend (or fall back to `FileEntry` metadata) when browsing restricted paths.

### 4.2 Root-Safe Filesystem Engine & Framed Helper Protocol (Essential — Highest Risk)

1. **Do NOT run `GFile` / `AT_FDCWD` mutations as root:**
   - The root helper (`nythera-admin-helper`) should link only libc/GLib-core (no GIO/GVFS/Wayland/GUI libs) and execute all mutations via **fd-relative POSIX + Linux `openat2`** (`O_PATH | O_NOFOLLOW | O_CLOEXEC`, `RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS`, `RESOLVE_BENEATH | RESOLVE_NO_XDEV` for recursive walks), pinned parent dirfds for `openat`, `mkdirat`, `unlinkat`, `renameat2(RENAME_NOREPLACE)`, and verified `device`/`inode` snapshots for approved replacements.
   - Re-use the exact F-02–F-03 state machine (staged copies beside destination, backup-then-swap on Replace, preserve backup & report split-state on cross-fs Move source-cleanup failure), re-expressed over pinned fds instead of `GFile*`.
   - Enforce recursive bounds (depth 128, entry cap, no symlink following, no child-mount crossing) and refuse destructive recursive writes against virtual kernel trees (`/proc`, `/sys`, `/dev`).
2. **Framed `stdin`/`stdout` RPC protocol & deadlock-free cancellation:**
   - Because `pkexec` closes fds > 2, all requests, progress frames, conflict queries/responses, and explicit `CANCEL` messages share `stdin`/`stdout` (length-prefixed frames or strict line-delimited base64/JSON frames with a hard max frame size, e.g. 64 KiB).
   - **Critical lifecycle constraint:** If the user cancels or quits while the helper is blocked awaiting a conflict decision (or executing a syscall), the client closes/cancels cleanly so `FileOperationManager::shutdown()` never deadlocks on `m_workerDrain->cv` and the helper receives `POLLHUP` and unwinds staged temps before exiting.
   - **Critical concurrency constraint:** Serialize mutating helper commands (or keep one active job + lightweight read queries on a dedicated helper mutex/queue) so protocol framing stays simple and conflict prompts cannot interleave.

### 4.3 Security & UX Guardrails (Essential)

- **Visual distinction & non-persistence:** Unmistakable Administrator Mode top banner (with *Exit Administrator Mode* button and session indicator), window title badge (`[Administrator]`), never persisted in `FileManagerSettings`, never inherited by [launchNewWindow()](../src/filemanager/file_manager.cpp#L1376-L1391).
- **Explicit capability containment while in Admin Mode:**
  - **Block elevated app execution:** Never launch apps, terminals, or external tools as root. Disable *Open* / *Open With* / *Tools* external launchers on root-restricted items (or clearly gate/warn that external apps launch unprivileged and cannot read root-only files).
  - **Disable `ThumbnailService` and `.directory` reads** on admin sessions (degrade `FileIcon` to lexical/MIME themed icons from `FileEntry::type`).
  - **Disable Wastebin / Trash** (`Trash`, `Restore`, `Empty Wastebin`; navigating to `trash:/` exits or prompts to exit Admin Mode). Permanent Delete uses the existing confirmed `permanentDeletePrompt()`.
  - **Disable Undo/Redo** for privileged operations (clear/block replay while in Admin Mode so privilege boundaries are never crossed).
  - **Disable drag-and-drop in MVP** (avoids unprivileged `checkedDropTarget()` failures and accidental system-directory drops).

### 4.4 Packaging & Installation (Essential for Real Runtime)

- Add `data/polkit/dev.nythera.Files.admin-session.policy.in` and `nythera-admin-helper` target + install tag in [meson.build](../meson.build#L1252-L1267), reusing the same root-owned path-chain invariant as `nythera-properties-helper`.
- Note: Like `nythera-properties-helper`, real Polkit authentication, root-only directory traversal, and install-prefix verification require an authorised installation environment (unit tests use injected mock/unprivileged helper streams and temporary fixtures—never authenticating as root).

---

## 5. Minimum Viable Administrator Mode Scope

Distinguishing **essential MVP** from **optional / deferred** keeps risk and surface area low:

### Essential MVP

- Explicit **Enter / Exit Administrator Mode** (menu action in *Tools* + prominent dismissible/exitable banner + helper-enforced idle timeout + automatic teardown on window close).
- **Privileged browsing & inspection** of restricted local directories (`enumerate`, `stat`/selection info, `statvfs` free space, writability).
- **Core safe mutations** through `FileOperationManager` with native progress, cancellation, F-07 conflict cards, and post-op refresh:
  - Create Folder & Create Empty/Text File (helper-allocated no-replace name or user-named).
  - Checked Rename (device/inode verified, `RENAME_NOREPLACE`).
  - Copy & Move (via Copy/Cut/Paste, staged atomic write, native conflict resolution, F-03 cross-fs recovery).
  - Permanent Delete (explicit confirmation, bounded `openat2` non-crossing recursive unlink).
- **Hard security guardrails:** unprivileged GUI, no root app launches, no root thumbnails, no root Wastebin/Undo/DnD, virtual-fs write protection, `installedFile()` + dedicated Polkit action verification, `stdin` EOF death-pact.

### Optional / Explicitly Deferred (Post-MVP)

- Privileged `PropertiesWindow` metadata inspection & folder-size scan inside `0700 root:root` dirs (existing `nythera-properties-helper` already handles ownership/permission edits on accessible paths; routing `PropertiesWindow` initial stat through the admin session is a clean Stage 4 follow-up).
- Privileged live directory watching (`inotify` inside helper) — explicit post-op refresh + Ctrl+R is sufficient for MVP.
- Drag-and-drop into/within Administrator Mode (requires privileged drop target validation).
- Privileged symlinks & office template creation in Admin Mode.
- Privileged archive extraction/compression.

---

## 6. Recommended Staged Implementation Roadmap

### Stage 1 — Shared Privileged Client Validation & Framed Protocol Core (Testable Without Root)

- Extract the reusable Polkit helper-verification logic (`installedFile()`, dedicated action + `org.freedesktop.policykit.exec.path` check, `pkexec --disable-internal-agent` spawn) from [property_operation_client.cpp](../src/filemanager/property_operation_client.cpp#L12-L96) into a small shared `polkit_helper_launch.{h,cpp}` utility (no behavioural change to Properties).
- Implement `admin_protocol.{h,cpp}` (strict bounded request/response/progress/conflict frame encoding & validation) and `admin_operations.{h,cpp}` (fd-relative `openat2` POSIX engine for `list`, `stat`, `statvfs`, `mkdir`, `create_file`, `rename`, `copy`, `move`, `delete`, with `RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_BENEATH | RESOLVE_NO_XDEV`, staged backup/swap, and virtual-fs guards).
- Implement `admin_helper_main.cpp` (`nythera-admin-helper` executable, non-setuid, `geteuid() == 0 && PKEXEC_UID` check, `umask(0077)`, idle timer, `POLLHUP`/EOF cancellation) + `data/polkit/dev.nythera.Files.admin-session.policy.in`.
- **Tests:** Focused Meson tests (`tests/admin_operations_test.cpp`, `tests/admin_protocol_test.cpp`) exercising the real `admin_operations` engine unprivileged in temp fixtures: symlink/TOCTOU rejection, `RENAME_NOREPLACE`, conflict decisions, mount/depth/entry bounds, cross-fs split-state recovery, malformed frame rejection, and EOF shutdown.

### Stage 2 — Backend Seams in `DirectorySession`, `FileOperationManager`, & `ModalDialogHost`

- Add `AdminSessionClient` (`admin_session_client.{h,cpp}`) managing helper process lifetime, `GCancellable` forwarding, idle/unexpected exit notification, and an injectable transport seam for isolated tests (same pattern as `WastebinBackend` and `PropertyAuthorizer`).
- Fix the 3 remaining UI-thread filesystem checks (`beginRenamePath()` using `FileEntry` identity, `creationDestination` / `showSymlinkDialog` delegating collision checks cleanly).
- Extend `ModalDialogHost::makeConflictResolver()` to accept optional backend-supplied `ConflictItemInfo` metadata so conflict cards work identically when the GUI cannot `lstat` paths.
- Add minimal backend hooks to `DirectorySession` (enumerate, writability, skip `GFileMonitor` in admin mode) and `FileOperationManager` (route active admin-session ops through `AdminSessionClient` on existing `GTask` workers while preserving `ProgressWindow`, `JobViewServerV2`, `usesMount()`, and `shutdown()` drain).
- **Tests:** Mock-helper integration tests in `tests/file_operation_manager_test.cpp` & `tests/file_manager_async_fs_test.cpp` covering conflict round-trip, mid-operation cancellation, helper crash/idle timeout during active op, and `FileOperationManager::shutdown()` drain without deadlock.

### Stage 3 — Administrator Mode UI, Guardrails, & Menu Wiring

- Add `AdminModeController` (`admin_mode_controller.{h,cpp}`) owned by `FileManager` managing enter/exit transitions, idle/crash recovery (falling back cleanly to an accessible local directory like `$HOME` if the current folder is root-only on exit), and capability state.
- Add the high-contrast Administrator Mode banner in `FileManager`'s main pane + `[Administrator]` window title suffix.
- Gate `FileMenuController`, `FileTransferController`, `ThumbnailService`, and `FileIcon` when Admin Mode is active (disable Wastebin, Undo/Redo, DnD, thumbnails, `.directory` reads, and root app launches; add *Enter/Exit Administrator Mode* in *Tools*).
- **Tests & Validation:** Menu enablement matrix tests, live manual acceptance in an authorised install environment (`meson install --tags admin-helper`), testing Polkit auth/cancel/denial, browsing `/root`, rename/copy/move/delete with conflicts, idle expiry, and clean exit back to unprivileged mode.

### Stage 4 (Optional Follow-Up) — Privileged `PropertiesWindow` Inspection & Folder Size

- Allow `PropertiesWindow` to query initial item metadata and recursive folder size via `AdminSessionClient` when opened from Administrator Mode on items inside `0700 root:root` directories.

---

## 7. Updated Complexity & Risk Assessment

| Dimension | Pre-F-01–F-11 | Current `dev` (Post-F-11) |
| --- | --- | --- |
| **Overall complexity** | Very High (required untangling `FileManager`, fixing unsafe transfers, removing UI-thread I/O, and inventing Polkit patterns first) | **Moderate-High** (concentrated in the fd-relative root helper engine + narrow backend seams) |
| **Privilege-escalation / TOCTOU risk** | Severe (path-based overwrites, symlink races, unbounded walks) | **Manageable** if Stage 1 strictly enforces `openat2` + pinned dirfds + `RENAME_NOREPLACE` + `RESOLVE_BENEATH`/`RESOLVE_NO_XDEV` rather than calling `GFile` as root |
| **UI / lifecycle deadlock risk** | High (unguarded callbacks, blocking I/O, unstructured conflict prompts) | **Low-Moderate** (F-01/F-04/F-06/F-07 solved the GUI side; main care needed is unblocking pending conflict promises on helper EOF/shutdown) |
| **Standalone Files separation impact** | High coupling | **Low** (private `pkexec` helper + Polkit policy mirrors existing `nythera-properties-helper` with zero Nythera Shell runtime requirement) |
| **Readiness to proceed** | Blocked | **Ready to proceed** with Stage 1–3 MVP once MVP scope and Model A (pipe-bound `pkexec` session helper) are approved |
