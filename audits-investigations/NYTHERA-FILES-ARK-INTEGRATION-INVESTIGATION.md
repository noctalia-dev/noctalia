# Nythera Files — Ark Integration: Technical Investigation

- **Repository:** `Kangy1103/Nythera`
- **Branch:** `dev` (`1636655f9`)
- **Related issue:** https://github.com/Kangy1103/Nythera/issues/25
- **Environment:** Ark `26.08.3`, KF6, `libarchive` `3.8.11`

**Scope:** Investigation only. No application code was modified, no dependencies were installed, and the workspace remains clean. All empirical checks were performed in disposable `/tmp` directories against the installed **Ark 26.08.3**, **KF6**, and **libarchive 3.8.11** packages.

---

## Executive Summary

**Ark cannot reliably satisfy the objective of retaining Nythera Files' native UI for archive creation, extraction, progress, cancellation, conflict handling, password entry, and error handling.**

Inspection of Ark's upstream architecture and isolated experiments against installed Ark `26.08.3` demonstrate that:

1. **Dolphin does not use a public Ark IPC/CLI API.** Dolphin's *Compress* and *Extract* menus are in-process Qt6/KF6 plugins (`compressfileitemaction.so`, `extractfileitemaction.so`) that link directly against Ark's **private** `libkerfuffle.so.26` library.
2. **`libkerfuffle` is explicitly private and ABI-unstable** (installed with `LIBRARY NAMELINK_SKIP`, zero headers, no pkg-config/CMake metadata, annual SOVERSION bumps, and hard dependencies on Qt6Widgets + 12 KF6 libraries).
3. **Ark exposes no archive-control D-Bus API.** Live introspection of `org.kde.ark-<pid>` confirms it only exports `KDBusService` boilerplate (`org.freedesktop.Application` and `org.kde.KDBusService.CommandLine`).
4. **Ark's CLI (`--batch`, `--add-to`) cannot suppress Ark's own Qt/KF6 dialogs**, provides **zero CLI flags for compression options or passwords**, reports **coarse/inconsistent `percent`-only progress** (never bytes/speed) over a session-singleton `JobViewServer`, returns **exit code `0` even when cancelled**, and **leaves orphan child processes (e.g. `7z`) running on `SIGTERM`**.

**Recommendation:** Use **`libarchive` (pure C, optional Meson feature)** as the primary in-process archive backend inside `FileOperationManager`—the exact same engine Ark's own `kerfuffle_libarchive.so` uses—paired with an optional `7z`/`unrar` subprocess adapter only for formats `libarchive` cannot decrypt/write (encrypted `.7z`/`.rar`), and keep **Ark strictly as an optional external application** (via `org.kde.ark.desktop` / *Open in Ark*) for interactive archive browsing and editing.

---

## 1. Ark's Integration Interfaces (Upstream Source & Empirical Findings)

### 1.1 Command-Line Interface (`app/main.cpp`, `app/batchextract.cpp`, `kerfuffle/addtoarchive.cpp`, `ark(1)`)

Upstream references:
- `invent.kde.org/utilities/ark/-/blob/master/app/main.cpp`
- `invent.kde.org/utilities/ark/-/blob/master/app/batchextract.cpp`
- `invent.kde.org/utilities/ark/-/blob/master/kerfuffle/addtoarchive.cpp`
- Installed `ark(1)` and `ark 26.08.3 --help`

The CLI is Ark's **only public cross-toolkit interface**:
- **Extraction:** `-b/--batch`, `-o/--destination <dir>`, `-a/--autosubfolder`, `-e/--autodestination`, `-d/--dialog`, `-O/--opendestination`.
- **Creation:** `-c/--add`, `-t/--add-to <filename>`, `-p/--changetofirstpath`, `-f/--autofilename <suffix>`, `-d/--dialog`.
- **Discovery:** `-m/--mimetypes` (prints 42 MIME types on this host, mixing read-only formats like `application/vnd.debian.binary-package`, `application/x-rpm`, `application/vnd.ms-cab-compressed` with writable ones).

**Critical runtime behaviours verified on Ark 26.08.3:**
- **Mandatory GUI/Qt startup:** `app/main.cpp` constructs `QApplication` before parsing options. Even `ark --version` and `ark --mimetypes` abort (`SIGABRT`, exit 134) without a Wayland/X11 display unless `QT_QPA_PLATFORM=offscreen` is forced, and hang under `offscreen` if the platform theme/D-Bus session is unreachable.
- **No non-interactive mode:** There is no `--no-dialog`, `--overwrite`, `--skip`, `--password`, `--compression-level`, or `--method` flag.
- **Unreliable exit status:** `app/main.cpp` connects `KJob::result` to `QCoreApplication::quit()` and returns `QApplication::exec()`. In our mid-job D-Bus cancellation experiment (`ark --changetofirstpath --add-to out.tar.gz`), Ark terminated the job with `JobViewV3.terminate(errorCode=1)` and deleted the partial archive, **yet exited with process status `0`**.

### 1.2 D-Bus Interfaces

- Ark installs **no** `/usr/share/dbus-1/services/org.kde.ark*.service` file.
- `app/main.cpp` instantiates `KDBusService(KDBusService::Multiple)` (per `/usr/include/KF6/KDBusAddons/kdbusservice.h`), registering `org.kde.ark-<pid>`.
- **Verified empirically** via live `org.freedesktop.DBus.Introspectable.Introspect` of `org.kde.ark-41` while compressing:
  - `/org/kde/ark` exports **only** `org.freedesktop.Application` (`Activate`, `Open`, `ActivateAction`) and `org.kde.KDBusService` (`CommandLine`).
  - `/MainApplication` exports **only** `org.qtproject.Qt.QApplication`.
  - **No Ark-specific D-Bus methods, properties, or signals exist.**

### 1.3 KJob & KDE Job Reporting (`KIO::getJobTracker`, `KDynamicJobTracker`, `KUiServerV2JobTracker`)

Upstream references:
- `invent.kde.org/frameworks/kio` (`src/widgets/kdynamicjobtracker.cpp`)
- `invent.kde.org/frameworks/kjobwidgets` (`/usr/include/KF6/KJobWidgets/kuiserverv2jobtracker.h`)
- Symbols verified via `readelf -Ws /usr/bin/ark /usr/lib/libkerfuffle.so.26.08.3`

Both `BatchExtract` and `AddToArchive` register their `KJob` with `KIO::getJobTracker()` (`KDynamicJobTracker`), and attach `KIO::createDefaultJobUiDelegate()`.

**Key empirical discovery on `KDynamicJobTracker` protocol:**
1. `KDynamicJobTracker` first probes **`org.kde.kuiserver`** at `/JobViewServer` (`org.kde.kuiserver.requiresJobTracker()`).
2. If `org.kde.kuiserver` is **not owned**—which is true in standalone environments *and* under current Nythera Shell, since [job_progress_service.cpp](../src/dbus/notification/job_progress_service.cpp#L13-L16) only claims `org.kde.JobViewServer`—`KDynamicJobTracker` logs:
   ```text
   kf.kio.widgets: Failed to check which JobView API is supported "The name org.kde.kuiserver was not provided by any .service files"
   ```
   and **falls back to `KWidgetJobTracker`**, opening a standalone Qt progress window.
3. Even when `org.kde.kuiserver` returns `requiresJobTracker() == false` and Ark registers on `org.kde.JobViewServerV2.requestView(...)`:
   - `nythera-file-manager` **cannot** claim `org.kde.JobViewServer`/`org.kde.kuiserver` on the user's session bus because those are desktop-shell singletons owned by `nythera` or Plasma.
   - Ark only reports `capabilities = 1` (`KJob::Killable`, not `Suspendable`).
   - Ark **never** emits `processedBytes`, `totalBytes`, or `speed`—only `percent` (and `totalFiles: 1` for archive count).
   - `percent` fidelity is severely plugin-dependent (verified empirically):
     - `.zip` creation (`kerfuffle_libzip.so`): emits `percent` 1..50 then jumps to 100.
     - `.tar.gz` creation (`kerfuffle_libarchive.so`): coarse per-file jumps (`25, 50, 75, 100` for 4 files).
     - `.tar.xz` creation (`kerfuffle_libarchive.so`): hits `100` before `archive_write_close()` spends 3.6 s compressing LZMA blocks.
     - `.7z` creation (`kerfuffle_cli7z.so`): zero intermediate updates; jumps straight to `100`.
     - Fast extractions (< 500 ms) emit zero updates; malicious archive failures emitted nonsensical `percent: 120` and `percent: 150`.

*(Bonus shell observation: if Nythera Shell later wants KDE apps such as Ark/Dolphin to route their own standalone jobs into Nythera's notification toast instead of spawning `KWidgetJobTracker` windows, [job_progress_service.cpp](../src/dbus/notification/job_progress_service.cpp#L13-L16) would also need to provide `org.kde.kuiserver` / `requiresJobTracker() -> false`. That does not solve Nythera Files' in-app progress/dialog requirements.)*

### 1.4 Dolphin Integration, `libkerfuffle`, Plugins, and Service Menus

Upstream references:
- `invent.kde.org/utilities/ark/-/blob/master/kerfuffle/CMakeLists.txt`
- `invent.kde.org/utilities/ark/-/blob/master/app/compressfileitemaction.cpp`
- `invent.kde.org/utilities/ark/-/blob/master/app/extractfileitemaction.cpp`

- **No KIO `.desktop` service menus:** Modern KF6 Ark installs zero files in `/usr/share/kio/servicemenus/`.
- **How Dolphin actually integrates:** `pacman -Ql ark` and `readelf -d`/`readelf -Ws` on `/usr/lib/qt6/plugins/kf6/kfileitemaction/{compress,extract}fileitemaction.so` confirm Dolphin loads these two `KAbstractFileItemActionPlugin` Qt6 plugins **in-process**, linking directly to `libkerfuffle.so.26` (`Kerfuffle::AddToArchive`, `Kerfuffle::Archive::batchExtract`, `Kerfuffle::PluginManager`, `Kerfuffle::ExtractionDialog`). Dolphin does **not** spawn `ark` via CLI.
- **`libkerfuffle` is strictly internal/private:** Upstream `kerfuffle/CMakeLists.txt` explicitly builds it with:
  ```cmake
  set_target_properties(kerfuffle PROPERTIES VERSION ${KERFUFFLE_VERSION} SOVERSION ${KERFUFFLE_SOVERSION})
  install(TARGETS kerfuffle ${KDE_INSTALL_TARGETS_DEFAULT_ARGS} LIBRARY NAMELINK_SKIP)
  ```
  Verified via `pacman -Ql ark`: no headers under `/usr/include`, no `libkerfuffle.so` namelink, no `.pc`/CMake config, `SONAME = libkerfuffle.so.26` (changes every KDE Gear year), and links `libQt6Widgets`, `libKF6KIOFileWidgets`, `libKF6KIOWidgets`, `libKF6WidgetsAddons`, etc.
- **Kerfuffle plugins (`/usr/lib/qt6/plugins/kerfuffle/*.so`)** are internal Qt6 plugins wrapping:
  - `libarchive` (`kerfuffle_libarchive.so`, `kerfuffle_libarchive_readonly.so`)
  - `libzip` (`kerfuffle_libzip.so`)
  - External CLIs via `QProcess`: `7z`/`7za`, `zip`/`unzip`, `unrar`/`rar`, `unar`, `arj`.

---

## 2. Capability Comparison of Candidate Approaches

| Capability | A. Ark CLI (`--batch` / `--add-to`) | B. Ark CLI + Private D-Bus `JobViewServer` Shim | C. `libkerfuffle` / KF6 Plugins (In-Process) | D. Native `libarchive` (+ Optional `7z`/`unrar` Fallback + Optional "Open in Ark") |
| --- | --- | --- | --- | --- |
| **Archive creation & extraction** | Yes (delegated to Ark plugins) | Yes | Yes | Yes (`libarchive` covers `.zip`, `.tar.*`, `.7z`, `.rar` read, `.iso`, `.deb`, `.cab`, `.cpio`, etc.; optional `7z`/`unrar` covers encrypted `.7z`/`.rar`) |
| **Format & MIME discovery** | Partial: `ark --mimetypes` mixes read-only & writable; needs Qt QPA init | Same as A | Yes (`PluginManager`), private ABI | Full & exact: deterministic built-in read/write format table + runtime capability check for optional CLI tools |
| **Multiple selected files & folders** | Yes (`-p --add-to <file> -- <paths...>`) | Yes | Yes | Yes (explicit relative root calculation + multi-archive extraction) |
| **Custom archive names & destinations** | Yes (`--add-to`, `-o`); note `-a` is conditional, not forced subfolder | Yes | Yes | Full control (including deterministic *Extract Here* vs *Extract to Subfolder* vs *Extract To…*) |
| **Compression options (level, method, encryption)** | **No** (zero CLI flags; only in Qt `CreateDialog`) | **No** | Yes (private C++/Qt API) | **Yes** (`libarchive` options for level, method, ZIP AES-256/ZipCrypto; `7z` for encrypted 7z) |
| **Real progress (bytes / percent / speed)** | **No** in `nythera-file-manager` (sent to session `JobViewServer`, never bytes/speed) | Coarse `percent` only (jumps/0 ticks on `7z`/`tar.xz`, no bytes/speed) | Coarse `percent` only (`ReadOnlyArchiveInterface::progress(double)`) | **Yes**: exact `processedBytes`, `totalBytes`, `processedFiles`, `totalFiles`, live transfer speed via `TransferRates` + `FileTransferPauseState` pause/resume |
| **Cancellation & reliable cleanup** | **Poor**: `SIGTERM` orphans `7z` child; D-Bus cancel returns `rc=0`; partial extracted files remain | D-Bus `cancelRequested` works for `libarchive`/`libzip`, still needs `kill(-pgid)` fallback | Qt/KJob cancellation; still leaves extracted files | **Full**: `GCancellable` checked per I/O chunk, zero orphan processes for `libarchive`, `setpgid`/`kill(-pgid)` for optional CLI, staged archive temp file deleted on cancel |
| **Success / failure / partial completion reporting** | **Unreliable**: `rc=0` on cancel; errors block on `KMessageBox` | `JobViewV3.terminate` codes, but fatal errors still block on `KMessageBox` | `KJob::error()` / `errorString()` | **Exact**: structured result counts (`extracted`, `skipped`, `failed`), error text, `mutated` flag matching `FileOperationManager` |
| **Password-protected archives** | **Blocks** on Ark's Qt `KPasswordDialog` (cannot pass or prompt natively) | **Blocks** on Ark's Qt `KPasswordDialog` | Custom `Query` handler possible, private API | **Full native**: `archive_read_add_passphrase()` / `archive_write_set_passphrase()` driven by `ModalDialogHost::textPrompt(..., password=true)` |
| **Overwrite & conflict handling** | **Blocks** on Ark's Qt `KIO::RenameDialog` / `KMessageBox` (both extract & existing `--add-to`) | **Blocks** on Ark's Qt dialogs | Custom `OverwriteQuery` possible, private API | **Full native**: routes directly through existing `ModalDialogHost::makeConflictResolver()` (F-07 cards, Replace/Skip/Rename/Merge/Apply-to-All) |
| **Suppress Ark's own dialogs & progress UI** | **Impossible** | Only suppresses `KWidgetJobTracker`; **cannot** suppress `KMessageBox`, `RenameDialog`, `PasswordDialog` | Still links Qt6Widgets & KF6 UI libs | **100% native**: zero external dialogs or Qt windows ever spawned |
| **Suitability for non-Qt C++23 Wayland app** | Low (violates native UI & reliability goals) | Low (fragile private `dbus-daemon`, still pops Qt dialogs) | **Disqualified** (private ABI, Qt6Widgets + 12 KF6 libs, Qt event loop) | **High**: pure C `libarchive`, zero Qt/KDE/GTK deps, clean GLib/`GTask` integration |

---

## 3. Recommended Backend Approach & Technical Justification

### Recommendation: Native `libarchive` Backend + Optional `7z`/`unrar` Adapter + Optional "Open in Ark"

1. **Primary backend (`libarchive` via C API):**
   - `libarchive` is already the underlying engine inside Ark (`kerfuffle_libarchive.so` and `kerfuffle_libarchive_readonly.so`), is standard on every Linux distribution, is pure C with zero Qt/KDE/GTK dependencies, and integrates cleanly into `FileOperationManager`'s existing `GTask` + `GCancellable` + `FileTransferPauseState` worker model.
   - It natively supports **both creation and extraction** of all core formats requested in the menu and dialog workflows (`.zip` including AES-256/ZipCrypto encryption and compression level, `.tar`, `.tar.gz`, `.tar.bz2`, `.tar.xz`, `.tar.zst`, `.tar.lz4`, `.7z` unencrypted) plus read-only extraction of `.rar` (v4/v5 unencrypted in libarchive 3.8), `.deb`, `.rpm`, `.cab`, `.cpio`, `.iso`, `.ar`, `.xar`, and single-stream `.gz`/`.bz2`/`.xz`/`.zst`.
   - Because Nythera controls the read/write loop entry-by-entry and chunk-by-chunk, we get:
     - **Real, non-fabricated progress** (pre-measured source bytes for creation; archive entry sizes or compressed stream byte position `archive_filter_bytes(a, -1)` vs archive file size for extraction—honestly switching to indeterminate/item-count mode when uncompressed sizes are absent, never fabricating percentages).
     - **Direct reuse of Nythera's F-07 native conflict dialog** (`ModalDialogHost::makeConflictResolver()`), **native password prompt** (`ModalDialogHost::textPrompt(..., true)`), **`ProgressWindow`**, and **`FileOperationManager`'s `JobViewServerV2` notifications** (including Pause/Resume).
     - **Stricter security than Ark** (entry validation *before* creating any file or symlink on disk; no dangling escape symlinks; decompression-bomb limits; `O_NOFOLLOW` + atomic staged writes).

2. **Optional Phase-2 subprocess adapter (`7z`/`7zz` and `unrar`) for encrypted `.7z` / `.rar`:**
   - `libarchive` cannot decrypt encrypted `.7z` or encrypted `.rar`, nor create encrypted `.7z`.
   - Rather than pulling in Ark (which hangs on `KPasswordDialog`/`RenameDialog`), an optional backend adapter discovered at runtime (`7zz`/`7z`, `unrar`) can handle only those specific cases when installed, with strict process-group cleanup (`setpgid(0, 0)` + `kill(-pgid)`), pre-extraction listing (`7z l -slt`) for security/conflict checks, and stdin password piping (never passing secrets on `argv` where `/proc/<pid>/cmdline` exposes them).
   - If the optional tool is absent, Nythera Files fails cleanly with an actionable message (e.g. *"Encrypted 7z archives require 7z/7zz"* or offers *"Open in Ark"* if installed).

3. **Optional Ark integration (where Ark actually shines):**
   - Keep Ark strictly as an **optional external viewer/editor** discovered via `GDesktopAppInfo` (`org.kde.ark.desktop`, matching [external_tools.h](../src/filemanager/external_tools.h#L104-L229)): e.g. opening/inspecting archive contents without rebuilding an interactive archive browser inside Nythera Files (consistent with [SCOPE.md](../SCOPE.md#L16-L20)).

---

## 4. Requirements Ark Cannot Satisfy Reliably (Empirical Evidence) & Alternatives

| Requirement | Empirical Ark 26.08.3 Failure Mode | Realistic Alternative |
| --- | --- | --- |
| **Suppress Ark's own dialogs & progress UI** | Blocked indefinitely (`timed_out=True`) on overwrite conflicts (`KIO::RenameDialog`), existing `--add-to` target (`KMessageBox::warningTwoActions`), passwords (`KPasswordDialog`), corrupt/missing archives (`KMessageBox::error`), and symlink/traversal errors. Spawns `KWidgetJobTracker` whenever `org.kde.kuiserver` is absent or returns `requiresJobTracker() == true`. | In-process `libarchive` driving Nythera's `ModalDialogHost`, `ProgressWindow`, and `FileOperationManager` D-Bus job reporting. |
| **Native conflict & overwrite handling** | Cannot disable `OverwriteQuery` or query Nythera's resolver; `--add-to` on an existing archive blocks on overwrite confirmation rather than letting Nythera apply Replace/Skip/Rename. | Per-entry conflict check in `libarchive` extraction + destination check in creation, wired directly into `ModalDialogHost::makeConflictResolver()`. |
| **Native password handling** | Encrypted `.zip`, `.7z`, and header-encrypted `.7z` all hung waiting for Qt `KPasswordDialog`. No CLI password input mechanism exists. | `libarchive` `archive_read_add_passphrase()` / `archive_write_set_passphrase()` + `ModalDialogHost::textPrompt(..., password=true)` (using the same worker-thread `std::promise`/`g_idle_add` pattern as `makeConflictResolver()`). |
| **Compression options** | Zero CLI flags for level, method, encryption, or split volumes. | Native *Compress to…* modal dialog (`ModalDialogHost::CustomContent`) passing structured `ArchiveCreateOptions` to `libarchive`. |
| **Real byte/speed progress** | `JobViewV3` never sends `processedBytes`/`totalBytes`/`speed`; `7z`/`tar.xz` emit zero intermediate percentages; `nythera-file-manager` cannot own session `org.kde.JobViewServer`. | `libarchive` chunk callbacks feeding `FileOperationManager::updateProgress()` & `TransferRates`. |
| **Reliable exit/cancellation status & process cleanup** | D-Bus-cancelled `ark --add-to` returned `rc = 0`. `SIGTERM` killed `ark` while leaving `/usr/lib/7zip/7z` alive as an orphan still writing `term.7z`. | `GCancellable` inside `FileOperationManager`'s `GTask` worker (zero child processes for `libarchive`; `setpgid` + `kill(-pgid)` for any optional CLI adapter). |
| **Distinguishing read-only vs writable formats** | `ark --mimetypes` returns one undifferentiated flat list including `.deb`, `.rpm`, `.iso`, `.cab`, `.msi`. | Explicit format registry in `archive_formats.{h,cpp}` separating extractable MIME types/extensions from creatable formats and their supported options (level/encryption). |
| **No dangling unsafe symlinks on malicious archives** | In isolated tests (`sym_abs.tar`, `sym_rel.tar`, `sym_abs.zip`), Ark blocked writes outside the destination (`outside=[]`), **but left the malicious symlink (`esc -> /tmp/.../outside`) created inside the destination folder** and hung on a Qt error dialog. | Pre-validate every symlink/hardlink target against the canonical extraction root before creating it (reject or strip escaping symlinks up front). |

---

## 5. Native Interface Integration & Proposed Component Boundaries

### 5.1 Current Post-F-11 Architecture Fit

The F-11 decomposition (`926e8a6b5`) gives us clean extension points with zero speculative refactoring:

- **[FileMenuController](../src/filemanager/file_menu_controller.h#L41-L185)** & **[file_view_menu.h](../src/filemanager/file_view_menu.h#L21-L64):**
  - Add menu IDs in `filemanager::menu_id` for:
    - **Compress** submenu: *Compress to ZIP*, *Compress to TAR.GZ*, *Compress to…*
    - **Extract** submenu: *Extract Here*, *Extract to Subfolder*, *Extract To…*
  - Expose them in **both** the selection context menu (`openContextMenu()`, respecting `groupFileMenuEntries()` so optional actions never leave orphaned separators) and the **Tools** menu (`toolsMenuEntries()`, alongside Checksums).
  - Visibility/enablement rules (computed purely from enumerated `FileEntry` metadata + `g_content_type_guess()`, preserving **F-04 zero blocking UI-thread filesystem I/O**):
    - *Compress*: enabled when `!browsingTrash()`, `!loading()`, `currentDirectoryWritable()` (for quick ZIP/TAR.GZ; *Compress to…* can target another writable directory), `!hasActiveDeviceOperations()`, `!replaying()`, and `!selectedPaths().empty()`.
    - *Extract*: shown/enabled when `!browsingTrash()`, `!loading()`, and all selected items (1 or more regular files) match a supported extractable archive format (using `FileEntry::type` + compound-extension check for `.tar.gz`/`.tar.xz`/`.tar.zst`/`.tar.bz2`), plus `currentDirectoryWritable()` for *Extract Here* / *Extract to Subfolder*.
  - If archive support is compiled out (`-Darchives=disabled`) or unavailable, hide or omit the archive submenus cleanly without breaking menu separators.

- **[FileOperationManager](../src/filemanager/file_operation_manager.h#L22-L210):**
  - Add `Kind::Compress` and `Kind::Extract` (both included in `reportsProgress(kind)` and transfer-rate/pause tracking).
  - Add `compress(ArchiveCreateRequest, ...)` and `extract(ArchiveExtractRequest, ...)` methods that launch on the existing `GTask` worker pool with:
    - `m_activePaths` tracking (so `usesMount()` protects both source archives/files and destination mounts during active archive ops—preserving F-10/mount safety).
    - `m_workerDrain` tracking (so `shutdown()` cancels and drains active archive workers cleanly before destruction—preserving F-06).
    - `FileTransferPauseState` & `TransferRates` integration (real speed, ETA, Pause/Resume, 450 ms `progressRevealed` threshold, `org.kde.JobViewServerV2` publication).
    - `completedEntries` populated with the created archive path or extracted top-level entry paths (device/inode) so **[FileTransferController](../src/filemanager/file_transfer_controller.h#L36-L100)**'s `registerPasteRequest()` / `applyPendingPasteSelection()` automatically selects and reveals the new archive or extracted folder(s) after refresh.
    - Undo/redo policy: archive creation and extraction are marked `noteExternalMutation()` without pushing destructive undo entries (same as `Kind::Delete` / `Kind::Properties`; avoid adding `Undo unavailable` noise, or treat explicitly like `Properties`).

- **[ProgressWindow](../src/filemanager/dialogs/progress_window.cpp#L36-L154):**
  - Works out of the box once `Kind::Compress` and `Kind::Extract` are registered in `FileOperationManager`. Because `ProgressWindow` reads `m_operations.operations()` directly and cancels via `m_operations.cancel(id)`, archive operations automatically appear in the fallback window (when no `org.kde.JobViewServer` is running) and in the shell notification centre (when running under Nythera Shell), with filenames sanitized via `sanitizeFilenameForDisplay()` (F-07).

- **[ModalDialogHost](../src/filemanager/dialogs/modal_dialog_host.h#L59-L124):**
  - **Already provides every primitive needed:**
    1. `CustomContent` (proven by [ChecksumDialogController](../src/filemanager/dialogs/checksum_dialog.cpp#L61-L107)) for the native **Compress to…** dialog (archive name, format selector `.zip` / `.tar.gz` / `.tar.xz` / `.tar.zst` / `.tar.bz2` / `.tar` / `.7z`, destination folder browse via `FileDialogMode::SelectFolder`, compression level selector, optional password + confirmation when the chosen format supports encryption, inline validation, theme-reload preservation).
    2. `makeConflictResolver()` for per-entry extraction conflicts and destination archive overwrite conflicts (F-07 structured comparison cards).
    3. `textPrompt(..., password=true, ...)` (plus a worker-thread synchronous wrapper `makePasswordResolver()` mirroring `makeConflictResolver()` with `std::promise` + `g_idle_add` + `GCancellable` + `m_alive` weak guard) for encrypted archive extraction.
  - **Extract To…** reuses the existing `FileDialog::open({.mode = FileDialogMode::SelectFolder, ...})` pattern already used by [restoreWastebinSelection()](../src/filemanager/file_manager.cpp#L1003-L1011) (plus a simple option/toggle or default subfolder behaviour if extracting multiple archives).

- **[DirectorySession](../src/filemanager/directory_session.h#L21-L111):**
  - No structural changes needed. When `FileOperationManager` finishes `Kind::Compress` or `Kind::Extract` with `operation.succeeded || operation.mutated`, [FileManager](../src/filemanager/file_manager.cpp#L258-L268) already triggers `enumerate(m_session.path())`, `m_places.refreshCapacities()`, and `m_transfers.onOperationFinished(operation)` (which respects `navigationGeneration()` so navigating away mid-operation never hijacks selection in another folder).

- **[FileManagerSettings](../src/filemanager/file_manager_settings.h#L33-L76):**
  - Add lightweight persisted defaults under the existing `"files"` namespace (matching `loadFileView()` / `saveFileView()`):
    - `archive_last_format` (e.g. `"zip"`, `"tar.gz"`, `"tar.zst"`, `"tar.xz"`, `"7z"`)
    - `archive_last_compression_level` (validated enum/int range)
    - `archive_open_destination_after_extract` (optional bool, default `false`)
  - Dialog size persistence already works automatically via `dialogSizeStateKey()` / `loadDialogSize()` / `saveDialogSize()`.

### 5.2 Proposed Dedicated Files & Boundaries

Keep all archive execution, format metadata, and dialog orchestration out of `FileManager`:

```text
src/filemanager/
  archive_formats.h / .cpp          # Pure format registry, compound extension/MIME matching,
                                    #   default archive/subfolder name derivation, filename validation
  archive_operations.h / .cpp       # Request/Result/Progress/PasswordResolver/ConflictResolver types,
                                    #   security policy (path/symlink/device/bomb checks),
                                    #   backend dispatch (libarchive primary + optional CLI hook)
  archive_backend_libarchive.cpp    # Isolated libarchive C API implementation (compiled only when
                                    #   libarchive is enabled); chunked I/O, pause & cancel gates,
                                    #   staged atomic creation/extraction
  archive_controller.h / .cpp       # UI-side coordinator owned by FileManager:
                                    #   builds Compress/Extract menu entries, handles quick actions,
                                    #   Extract To… folder picker, Compress to… modal dialog
                                    #   (via ModalDialogHost::CustomContent), and invokes
                                    #   FileOperationManager + FileTransferController::registerPasteRequest
```

**Role of `FileManager`:** Pure wiring only—owns `ArchiveController` (constructed with callbacks matching `ChecksumDialogController` and `FileMenuController`), routes the new archive menu IDs in `activateContextMenuEntry()` to `m_archives`, wires `m_modalDialog.makePasswordResolver()` alongside `m_modalDialog.makeConflictResolver()`, and shuts down `m_archives.dismiss()` in `~FileManager()`.

---

## 6. Security & Reliability Analysis (Preserving F-01–F-10 Invariants)

1. **Path traversal & unsafe symlinks:**
   - **Never rely solely on `archive_write_disk` defaults.** Before extracting *any* entry, validate its pathname in `archive_operations.cpp`:
     - Reject NUL bytes, empty names, Windows-style `\` separators if normalized into `..`, absolute paths (leading `/`), and any `..` path component.
     - Reject special filesystem objects (`AE_IFBLK`, `AE_IFCHR`, `AE_IFIFO`, sockets) and strip `S_ISUID` / `S_ISGID` / `S_ISVTX` permission bits.
     - **Symlink & hardlink containment:** Resolve every symlink target lexically relative to its entry's parent directory within the extraction root; reject (or warn & skip, configurable fail-closed by default) any symlink or hardlink whose target is absolute or escapes the extraction root (`..` past root). This prevents both active symlink-traversal writes *and* the dangling escape symlink Ark left behind in our test.
     - **Kernel-enforced `O_NOFOLLOW` / `openat` extraction:** Open intermediate directories and target staging files without following symlinks (`O_NOFOLLOW | O_CLOEXEC`), matching `FileOperationManager` and F-05 `ThumbnailService` hardening.

2. **Accidental file overwrites (F-02 / F-03 / F-08):**
   - **Creation:** Write the new archive to a private temporary file in the destination directory (e.g. `.nythera-archive-XXXXXX.tmp`, mode `0600` before final permissions, matching `FileOperationManager` staging). If the final target path already exists, route through `ConflictResolver` (Replace / Rename / Cancel) rather than silently clobbering or appending; finalize via atomic `RENAME_NOREPLACE` (or backup-and-swap on approved Replace, matching F-02).
   - **Extraction:** For *Extract to Subfolder*, derive a non-colliding folder name (or prompt via `ConflictResolver` if existing). For *Extract Here* / *Extract To…*, check every destination entry with `lstat` (no symlink follow) and route collisions through `ModalDialogHost::makeConflictResolver()`, supporting Replace / Skip / Rename / Merge (for directories) / Apply-to-All and refusing directory-vs-file type replacement (matching [FILE-MANAGER-CONFLICTS.md](../docs/FILE-MANAGER-CONFLICTS.md#L7-L9)).

3. **Malicious or malformed archives (F-05 parity):**
   - Enforce bounded extraction limits (configurable constants, surfaced clearly on trip):
     - Max entry count (e.g. `250,000` entries), max path depth (e.g. `128`, matching [property_operation_client.cpp](../src/filemanager/property_operation_client.cpp#L183)), max path length.
     - Decompression-bomb guard: track cumulative extracted bytes and check destination filesystem available space (`statvfs`) plus suspicious expansion-ratio alerts when uncompressed output vastly exceeds compressed input without declared header size.
   - Sanitize every archive entry name and backend error message through `filemanager::sanitizeFilenameForDisplay()` (F-07) before rendering in progress summaries, conflict cards, status bar, or error dialogs.

4. **Argument injection & unusual filenames (F-09 parity):**
   - With `libarchive` in-process, filenames are raw `std::filesystem::path` byte strings—zero shell parsing, zero flag injection (`-h`, `--batch`, `foo:bar`, `#`, `?`, `%20`, newlines).
   - For any optional CLI adapter (`7z`/`unrar`) or optional *"Open in Ark"* launch: always pass `argv` directly via `g_subprocess_newv()` / `GAppInfo` (no shell), always pass `--` before positional paths, pass absolute paths (avoiding relative `-file` or `scheme:name` ambiguity—empirically, `ark --add-to -dashout.zip -evil.txt` without `--` failed with `Unknown options: i, l, .`), and never pass passwords on `argv`.

5. **Asynchronous execution, callback lifetimes, & shutdown (F-01 / F-04 / F-06 parity):**
   - All format probing (when needed beyond filename/MIME), archive scanning, compression, and extraction run inside `FileOperationManager`'s `GTask` worker threads—never on the Wayland/UI thread.
   - All worker->UI posts (`g_idle_add` for progress, conflict prompts, password prompts, completion) guard with `std::weak_ptr<bool> m_alive` and `GCancellable`.
   - `FileOperationManager::shutdown()` cancels all `GCancellable`s, wakes any waiting `FileTransferPauseState` or conflict/password `std::promise` (by cancelling when `m_modalDialog.forceCloseImmediate()` runs), and blocks on `m_workerDrain->cv` until workers exit cleanly.

6. **Cancellation, partial results, & post-completion refresh/selection:**
   - **Cancelled archive creation:** Delete the staged temporary archive file completely; zero corrupt partial archive left in the user's folder (`mutated = false`).
   - **Cancelled / failed archive extraction:** Consistent with [FILE-MANAGER-CONFLICTS.md](../docs/FILE-MANAGER-CONFLICTS.md#L9), remove the currently in-flight staged temp file, preserve already-finalized extracted entries (never silently deleting user files), report exact counts (*"Extraction cancelled — 14 of 50 items extracted"*), set `mutated = (extractedCount > 0)`, and **never** report cancelled/partial failure as success.
   - **Refresh & selection:** Populate `operation.completedEntries` with the final created archive (or extracted top-level paths) and register a `PasteRequest` with `m_transfers.registerPasteRequest(id, {destDir, navigationGeneration})`. On completion, `FileManager` refreshes the view (including on mutating failed/cancelled ops, per `ceea83de7`) and selects/reveals the created or extracted items only if the user is still viewing `destDir` at the same `navigationGeneration`.

---

## 7. Dependencies & Standalone Separation Implications

### Required & Optional Dependencies

| Dependency | Kind | Scope | Notes |
| --- | --- | --- | --- |
| **`libarchive`** (`dependency('libarchive', required: get_option('archives'))`) | Optional compile-time C library (`feature`, default `auto`) | Files archive backend only | Pure C (`-larchive`). Pulls only standard compression/crypto C libs (`zlib`, `zstd`, `lzma`, `bz2`, `lz4`, `libcrypto`, `libxml2`—most already transitively present). **Zero Qt, KDE, or GTK dependencies.** |
| **`7zz` / `7z`**, **`unrar`** | Optional runtime executables (Phase 2) | Encrypted `.7z`/`.rar` fallback only | Discovered lazily on `PATH` when needed; zero build-time dependency. |
| **KDE Ark** (`org.kde.ark.desktop`) | Optional runtime desktop app | *"Open in Ark"* interactive browsing/editing only | Zero build-time dependency, zero Qt/KF6 linkage. Discovered via `GDesktopAppInfo` like existing `external_tools.h`. |

### Implications for Eventual Standalone Separation of `nythera-file-manager`

1. **Current build coupling to be aware of:** In [meson.build](../meson.build#L491-L510) and [meson.build](../meson.build#L1237-L1294), `src/filemanager/*.cpp` is currently compiled into `nythera_core_lib`, which is linked into both `nythera` (shell) and `nythera-file-manager`.
   - Adding `libarchive_dep` directly to `_nythera_dependencies` would unnecessarily link `libarchive` into the shell executable `nythera`.
   - **Cleaner approach:** Keep archive implementation sources (`archive_formats.cpp`, `archive_operations.cpp`, `archive_backend_libarchive.cpp`, `archive_controller.cpp`) scoped to `nythera-file-manager` (or a small `filemanager_lib` static library) and link `libarchive_dep` only there. That directly supports future extraction of `nythera-file-manager` into a standalone repository with minimal dependency surface.
2. **No shell-coupled D-Bus dependency:** Because progress reporting uses `FileOperationManager`'s existing dual-path design (`org.kde.JobViewServerV2` when present, falling back seamlessly to native `ProgressWindow` when absent), archive operations work identically whether `nythera-file-manager` runs under Nythera Shell, KDE Plasma, another Wayland compositor, or completely standalone.
3. **Licensing compatibility:** `libarchive` is 2-clause BSD—compatible with GPL/MIT/Apache/custom packaging without imposing copyleft or Qt licensing constraints.

---

## 8. Staged Implementation & Testing Plan

### Stage 1 — Core Format Registry & Hardened `libarchive` Engine (Headless, No UI Yet)

- Add optional Meson feature `option('archives', type: 'feature', value: 'auto')` and `libarchive_dep` scoped to Files targets.
- Implement `src/filemanager/archive_formats.{h,cpp}`:
  - Supported extractable MIME types + compound-extension matching (`.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.tar.zst`, `.tzst`, `.tar.bz2`, `.tbz2`, `.tar.lz4`, `.tar`, `.zip`, `.7z`, `.rar`, `.cab`, `.cpio`, `.iso`, `.deb`, `.rpm`, single-stream `.gz`/`.bz2`/`.xz`/`.zst`).
  - Creatable formats (`.zip`, `.tar.gz`, `.tar.zst`, `.tar.xz`, `.tar.bz2`, `.tar`, `.7z`) with per-format option metadata (compression levels, encryption support).
  - Default output naming (single selection `<stem>.<ext>`, multi-selection `<parent-dir-name>.<ext>` or `Archive.<ext>`, subfolder stem stripping compound `.tar.*`).
- Implement `src/filemanager/archive_operations.{h,cpp}` and `src/filemanager/archive_backend_libarchive.cpp`:
  - Creation with pre-measured total bytes/files, chunked streaming, staged temp file + `RENAME_NOREPLACE` / `ConflictResolver` finalization, `GCancellable` & `FileTransferPauseState` checkpoints, ZIP AES-256 encryption + compression levels.
  - Extraction with two-pass/streaming inspection, strict security validation (reject `..`, absolute paths, special devices, setuid/setgid, escaping symlinks/hardlinks, entry-count/depth/bomb limits), `O_NOFOLLOW` staged extraction, per-entry `ConflictResolver`, and `PasswordResolver` callback with retry on wrong password.
- **Stage 1 Tests (Meson `tests/archive_operations_test.cpp`):**
  - Round-trip creation & extraction (single file, multi-file, nested folders, empty folders, internal relative symlinks, weird filenames including `-h`, `--batch`, `a:b`, `#`, `?`, `%20`, Unicode, spaces).
  - Security regression suite: `..` traversal, absolute path entries, absolute & relative symlink escape attempts (assert zero files written outside destination **and** zero escaping symlinks left inside destination), hardlink escape, FIFO/device node rejection, decompression-bomb / entry-limit rejection, malformed/truncated archive handling.
  - Conflict handling: Replace, Skip, Rename, Merge, Apply-to-All, Cancel, and existing-archive conflict on creation.
  - Password-protected ZIP creation & extraction (correct password, wrong password retry, cancelled password prompt).
  - Mid-operation cancellation (creation removes temp archive cleanly; extraction cleans in-flight temp file and reports `cancelled=true` with accurate `mutated` state) and pause/resume boundary test.

### Stage 2 — `FileOperationManager` Integration & Native Password Resolver

- Add `Kind::Compress` and `Kind::Extract` to `FileOperationManager` with `startCompress()` / `startExtract()`, `m_activePaths` mount protection, `TransferRates` / `FileTransferPauseState` / `JobViewServerV2` integration, and `completedEntries` population.
- Add `ModalDialogHost::makePasswordResolver()` (worker-thread synchronous promise/future bridge over `textPrompt(..., password=true)` with `m_alive`, `GCancellable`, and shutdown unblock—mirroring `makeConflictResolver()`).
- **Stage 2 Tests (`tests/file_operation_manager_test.cpp` additions):**
  - Verify `usesMount()` stays active through archive operations until worker unwind completes.
  - Verify `shutdown()` cancels and drains active archive workers cleanly without use-after-free or deadlock when a conflict/password prompt is pending.
  - Verify `Operation` progress fields (`processedBytes`, `totalBytes`, `fraction`, `status`), `completedEntries`, and `mutated` flags on success, cancel, and error.

### Stage 3 — Native Menus, Dialogs, & `FileManager` Coordination

- Implement `src/filemanager/archive_controller.{h,cpp}`:
  - Quick actions: *Compress to ZIP*, *Compress to TAR.GZ*, *Extract Here*, *Extract to Subfolder*.
  - *Extract To…*: opens native `FileDialog` (`SelectFolder`), then launches `FileOperationManager::extract()`.
  - *Compress to…*: opens native `ModalDialogHost::CustomContent` dialog (archive name, format selector, destination browse, compression level, optional password + confirmation for encrypting formats; remembers last format/level in `FileManagerSettings`).
  - Registers `PasteRequest` with `FileTransferController` so newly created archives or extracted top-level items are selected and scrolled into view after directory refresh.
- Wire `ArchiveController` into `FileMenuController` (both context menu and Tools menu, updating `groupFileMenuEntries()` in `file_view_menu.h`) and `FileManager::activateContextMenuEntry()`.
- Add optional *"Open in Ark"* action only when `org.kde.ark.desktop` is installed (zero hard dependency).
- **Stage 3 Tests (`tests/file_interaction_test.cpp` / focused menu tests + live Wayland visual review):**
  - Menu visibility/enablement matrix (single file, multi-selection, folder, archive, non-archive, mixed selection, read-only directory, Wastebin, `-Darchives=disabled`).
  - Dialog validation (empty name, invalid `/` filename, mismatched password confirmation, navigation change guard).
  - Live visual verification of context menu, Tools menu, *Compress to…* modal, native password prompt, conflict cards, and `ProgressWindow`/shell toast.

### Stage 4 (Optional Follow-Up) — Encrypted `.7z` / `.rar` Adapter

- Only if encrypted `.7z`/`.rar` support is prioritized after Stage 3: add an isolated `archive_backend_cli.cpp` adapter for `7zz`/`7z` and `unrar` using `GSubprocess`, `setpgid(0, 0)` + `kill(-pgid)` cancellation, `7z l -slt` pre-inspection (enforcing the same path/symlink/conflict policy before extracting), `-bsp1` progress parsing, and stdin password delivery (never via `argv`).

---

## Key Open Decisions for Review

Before implementation begins, please confirm:
1. **Backend direction:** Confirm adopting **`libarchive` as primary native backend** (with optional Ark purely for *"Open in Ark"* interactive browsing) vs. relaxing native-UI requirements to delegate interactively to Ark.
2. **Encrypted `.7z` / `.rar` priority:** Defer Stage 4 (`7z`/`unrar` subprocess adapter) until after Stages 1–3 land, or include it in the initial milestone?
3. **Default *Extract Here* vs *Extract to Subfolder* behaviour:** Confirm whether *Extract Here* extracts directly into the current directory (always, using native conflict prompts on collision) while *Extract to Subfolder* always creates a dedicated `<archive-stem>/` directory (distinct from Ark's conditional `--autosubfolder`), or whether *Extract Here* should mirror Ark's auto-subfolder heuristic for multi-root archives.
