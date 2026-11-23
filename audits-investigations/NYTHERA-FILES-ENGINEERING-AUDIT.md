# Nythera Files — Independent Engineering Audit

Repository: `Kangy1103/Nythera`\
Branch: `dev` (`1b18e662f`)

---

## Executive Assessment

**Overall Engineering Quality: High (Hardened Production-Grade Systems Code with Specific Residual Edge Cases)**

Nythera Files exhibits unusually disciplined systems engineering for a custom C++23 native Wayland file manager. Unlike naive file manager implementations that wrapper-call `std::filesystem::copy` or shell commands, Nythera Files has been built around explicit threat and failure models:

- **Kernel-enforced traversal boundaries:** Property mutations and folder-size scans use `openat2` with `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV` and operate via `/proc/self/fd/<fd>`.
- **Symlink discipline:** Operations act on symlink directory entries (`G_FILE_COPY_NOFOLLOW_SYMLINKS`, `O_NOFOLLOW`, `lstat`) rather than traversing or clobbering link targets.
- **No-overwrite commit & fingerprint-verified Undo/Redo:** Transfers stage beside the target before finalising; Undo/Redo pins parent directory file descriptors (`ReplayParent`), verifies 7-tuple `(dev, ino, nlink, mode, size, mtim, ctim)` revisions before and after mutation, stages deletions in a private `0700` sibling directory before unlinking, and rolls back if the entry changed mid-operation.
- **Privilege separation:** The Polkit properties helper (`nythera-properties-helper`) is non-setuid, validates root-owned non-writable prefix chains before invocation, refuses generic `pkexec` actions, requires a readiness byte on `stdin`, and aborts immediately on `stdin` EOF if the client cancels or dies.
- **Mount & device safety:** Unmount/eject logic cross-checks `/sys/class/block` topology, Btrfs subvolume sharing, systemd unit dependencies (`RequiredBy`, `BoundBy`, `PropagatesStopTo`), active transfer paths (`usesMount`), and busy-device signals without ever offering forced or lazy unmounts.

The defects and risks identified below are therefore **not** signs of careless architecture; they are specific boundary conditions where the existing safety invariants were bypassed on secondary code paths (such as the `Replace` conflict path, `GTask`/idle lifetime teardown, synchronous main-thread metadata probes on network mounts, and `ThumbnailService` symlink/memory bounds).

---

## Summary of Prioritised Findings

| ID | Category | Title | Classification | Severity | Likelihood | Confidence |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **F-01** | Concurrency / Lifetime | `FileOperationManager` raw `this` in `GTask` & `g_idle_add` callbacks causes UAF if destroyed while workers are in flight | Confirmed Defect | **High** | Low–Medium | High |
| **F-02** | Data Integrity | `Replace` conflict resolution in Copy/Move/Rename uses non-atomic backup name generation and non-`NOREPLACE` moves | Confirmed Defect | **High** | Low | High |
| **F-03** | Data Integrity | Cross-filesystem `Move` leaves duplicate data at both source and destination if cancelled at final checkpoint or if source removal fails | Confirmed Defect | **Medium** | Medium | High |
| **F-04** | Concurrency / UI Blocking | Synchronous main-thread filesystem probes (`statvfs`, `canonical`, `queryFileArtwork`, `queryFileTypeDescription`, `directorySupportsCreation`) stall Wayland UI on slow/hung NFS/CIFS/FUSE mounts | Confirmed Defect | **Medium** | High | High |
| **F-05** | Security / Concurrency | `ThumbnailService` follows symlinks, reads up to 256 MiB per worker into memory before decoding, and writes non-atomically to predictable cache filenames | Confirmed Defect | **Medium** | Medium | High |
| **F-06** | Concurrency / Lifetime | `FileManager` GIO async callbacks (`enumerate`, `fetchNext`, `queryMonitoredFile`, `activatePath`, `closeDialog`) capture unguarded raw `this` | Confirmed Defect | **Medium** | Low | High |
| **F-07** | Data Integrity / UX | Conflict resolver dialog parser splits on `\n\n` and `\n`, corrupting conflict card display and `"Suggest New Name"` when filenames or paths contain newlines | Confirmed Defect | **Medium** | Low | High |
| **F-08** | Data Integrity / Creation | Office template creation (`New Document/Spreadsheet/Presentation`) runs synchronously on the UI thread, bypasses `PrivateStaging`, and has a TOCTOU window | Confirmed Defect | **Low–Medium** | Low | High |
| **F-09** | Security / Robustness | `external_tools::launchEntry` appends arguments to `.desktop` `Exec=` before `%f`/`%F` stripping and uses `std::filesystem::is_regular_file` (following symlinks) for tool eligibility | Plausible Risk | **Low–Medium** | Low | High |
| **F-10** | Concurrency / Correctness | `PlacesModel::operate` uses a function-local `static bool active` guard shared across all instances without thread synchronisation | Confirmed Defect | **Low** | Low | High |
| **F-11** | Architecture | `src/filemanager/file_manager.cpp` is a 7,469-line monolith combining Wayland windowing, scene-graph construction, menus, dialogs, monitors, and state persistence | Architectural Weakness | **Low** | N/A | High |

---

## Detailed Findings

### F-01: `FileOperationManager` raw `this` in `GTask` & `g_idle_add` callbacks causes Use-After-Free if destroyed while workers are in flight

- **Category:** Concurrency / Object Lifetimes
- **Classification:** Confirmed Defect
- **Severity:** High (Memory corruption / crash on teardown)
- **Likelihood:** Low–Medium (Mitigated in normal `FileManager::run()` by waiting for `!m_operations.active()`, but directly exposed if `MainLoop` exits on Wayland disconnect or in any standalone `FileOperationManager` consumer/test that destroys the manager with an active worker)
- **Confidence:** High

#### Supporting Code Evidence
In [file_operation_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L130-L137), `publishProgress` posts an idle callback with a raw `FileOperationManager*` and **no** `m_alive` check:
```cpp
    auto* owner = context.owner;
    g_idle_add([](gpointer data) -> gboolean {
      auto* report = static_cast<std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>*>(data);
      const auto& values = report->second;
      report->first->updateProgress(values[0], values[1], values[2], values[3], values[4], values[5]);
      delete report;
      return G_SOURCE_REMOVE;
    }, new std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>{owner, ...});
```
The same unguarded raw-pointer `g_idle_add` pattern appears at:
- [file_operation_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1700-L1707) (initial transfer measurement report)
- [file_operation_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L2082-L2089) (Trash/Delete item progress report)
- [file_operation_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1109-L1116) (`GTask` creation passing raw `this` to `FileOperationManager::completed`)
- [file_operation_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1798-L1800) (`data->owner->m_conflictResolver(...)` dereferencing `data->owner` on the worker thread without holding a lock or checking `data->alive`)

By contrast, lines [1275–1295](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1275-L1295) and [1358–1365](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1358-L1365) in the exact same file *do* capture `m_alive` (`std::weak_ptr<bool>` / `std::shared_ptr<bool>`) because the author recognised that idle callbacks can fire after `~FileOperationManager()` sets `*m_alive = false` ([line 822](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L822)).

Furthermore, if a Wayland compositor disconnect or fatal display error causes `MainLoop::run()` ([file_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L1687)) to exit while a transfer or conflict resolution is active, `FileManager` and its member `m_operations` are destroyed while the `GTask` worker thread is still running.

#### Potential Consequences
- Dereferencing freed `FileOperationManager` memory on the worker thread (`data->owner->m_conflictResolver`, `data->owner->m_wastebinBackend`) or on the GLib main context if it is pumped after `~FileOperationManager()`.
- If `FileManager` is destroyed while a worker thread is blocked in `m_conflictResolver` ([file_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L720)), `~FileOperationManager()` cancels the `GCancellable` and returns immediately without joining the worker thread; `~FileManager()` then destroys `m_conflictMutex`, while the worker thread wakes up from `answer.wait_for(...)` and unlocks the destroyed `m_conflictMutex` at scope exit ([line 563](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L563)).

#### Recommended Remediation
1. Copy `m_conflictResolver` and `m_wastebinBackend` into `FileOperationManager::Task` on the main thread inside `start()` so the worker thread never dereferences `data->owner`.
2. Guard all `g_idle_add` callbacks and `FileOperationManager::completed` with `std::weak_ptr<bool> alive = m_alive` (matching lines 1275 and 1360).
3. In `FileManager`'s conflict resolver lambda, capture `m_alive` (and/or move `m_conflictMutex` into a `std::shared_ptr<std::mutex>`) and check `*alive` before posting UI work or accessing `m_dialogAlternativeName`.

---

### F-02: `Replace` conflict resolution in Copy/Move/Rename uses non-atomic backup name generation and non-`NOREPLACE` moves

- **Category:** Data Integrity / Race Conditions
- **Classification:** Confirmed Defect
- **Severity:** High (Potential data loss on collision or concurrent destination mutation)
- **Likelihood:** Low
- **Confidence:** High

#### Supporting Code Evidence
1. **Unchecked 1-shot random backup name in `Rename` and `Copy`/`Move`:**
   In `gioMoveWithBackup` (used by Wastebin `Restore`, [file_operation_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L415-L424)), the code loops up to 100 times checking `!g_file_query_exists(backup, cancellable)` and passes `G_FILE_COPY_NO_FALLBACK_FOR_MOVE`.
   However, in `Kind::Rename` ([lines 1932–1939](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1932-L1939)) and `Kind::Copy`/`Kind::Move` ([lines 1999–2006](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1999-L2006)), `replaceExisting` generates a single random name without checking existence and calls `g_file_move`:
   ```cpp
   char* backupName = g_strdup_printf(".nythera-backup-%u", static_cast<unsigned>(g_random_int()));
   GFile* parent = g_file_get_parent(target);
   backup = g_file_get_child(parent, backupName);
   g_object_unref(parent);
   g_free(backupName);
   result->ok = g_file_move(target, backup, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, nullptr, nullptr, &error);
   ```
   And then moves `staged` (or `source`) into `target` using `g_file_move(..., G_FILE_COPY_NOFOLLOW_SYMLINKS, ...)` ([lines 1950, 2033](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L2033))—which **omits** `G_FILE_COPY_OVERWRITE`, yet also does not use `renameWithoutReplace` (`RENAME_NOREPLACE`) for non-replace transfers!
   Wait: in `g_file_move` without `G_FILE_COPY_OVERWRITE`, GIO checks if `target` exists before calling `rename(2)`, which is a TOCTOU check in GIO's local VFS backend (`g_local_file_move` does `g_lstat` then `g_rename` without `RENAME_NOREPLACE`).
2. **Non-replace `Copy`/`Move`/`Rename` finalisation uses `g_file_move` instead of `renameWithoutReplace`:**
   Look at [line 1950](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1950) and [line 2033](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L2033):
   Even though the comment at [line 1973](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1973) states:
   ```cpp
   // Stage beside the destination so finalisation is a single no-overwrite rename.
   ```
   `Kind::Duplicate` ([line 2021](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L2021)), `CreateFile/Folder/Symlink` ([line 1628](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1628)), and `replay` ([line 747](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L747)) actually call `renameWithoutReplace` (`renameat2(..., RENAME_NOREPLACE)`).
   **`Kind::Copy`, `Kind::Move`, and `Kind::Rename` do not!** They call `g_file_move(staged, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, ...)` at lines 1950 and 2033. Inside GLib's `glocalfile.c`, `g_local_file_move` without `G_FILE_COPY_OVERWRITE` performs a user-space `g_lstat()` check followed by POSIX `rename()`, leaving a TOCTOU race window where a newly created file at `target` (created while a large file was being copied to `staged`!) can be silently overwritten by `rename(2)`.
3. **Failure to remove backup in `Copy`/`Move`/`Rename` is silently ignored:**
   In `gioMoveWithBackup` ([lines 507–514](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L507-L514)), if `removeTree(backup)` fails, it sets an explicit message explaining that a backup remains. In `Rename` ([lines 1965–1969](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1965-L1969)) and `Copy`/`Move` ([lines 2041–2045](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L2041-L2045)), both `g_file_move(backup, target, ...)` on rollback and `removeTree(backup, ...)` on success discard `restoreError` / `cleanupError`, potentially leaving hidden `.nythera-backup-*` files behind without notifying the user.

#### Potential Consequences
- During a long `Copy` or `Move` where `target` did not exist at the start of the transfer (when `g_file_query_exists(target)` ran at line 1779 before `copyTree`), if another process creates `target` while `copyTree(source, staged)` is running, finalising `staged -> target` via `g_file_move` has a TOCTOU window (and if `replaceExisting` was approved for an earlier item, moving `target -> backup` does not use `RENAME_NOREPLACE` or `PrivateStaging`).
- If rollback `g_file_move(backup, target)` fails after a failed replacement, the user is told `"Operation failed"` without being informed that their original destination file was moved to `.nythera-backup-<rand>`.

#### Recommended Remediation
1. Stage replacement backups inside `PrivateStaging` (`g_mkdtemp` `.nythera-stage-XXXXXX`) or allocate backup paths using `renameWithoutReplace(targetPath, backupPath)` in a bounded loop with `G_IO_ERROR_EXISTS` retry.
2. At lines 1950 and 2033, use `renameWithoutReplace(stagePath, targetPath, &error)` (with fallback to `g_file_move` only when `renameat2` returns `ENOSYS`/`EINVAL` on non-local/FUSE filesystems, just like lines 1628–1634).
3. If `replaceExisting` was approved, verify that the `target` being moved to `backup` still has the same `(st_dev, st_ino)` or type that the user approved in the conflict dialog, or use `RENAME_EXCHANGE` (`renameat2(AT_FDCWD, stagePath, AT_FDCWD, targetPath, RENAME_EXCHANGE)`) on supported filesystems to atomically swap `staged` and `target` in one kernel operation before deleting `staged`!
4. Surface backup recovery paths in the error message if rollback `backup -> target` fails, matching `gioMoveWithBackup` ([lines 495–501](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L495-L501)).

---

### F-03: Cross-filesystem `Move` leaves duplicate data at both source and destination if cancelled at final checkpoint or if source removal fails

- **Category:** Data Integrity
- **Classification:** Confirmed Defect
- **Severity:** Medium (Inconsistent state / partial move without clear user guidance; no data destruction, as both copies survive)
- **Likelihood:** Medium (Occurs whenever `removeTree(source)` fails due to permission denied on a nested file/directory during a move, or when cancellation arrives after `staged` is moved to `target`)
- **Confidence:** High

#### Supporting Code Evidence
In [file_operation_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1972-L2053), `Kind::Move` (even on the **same** filesystem!) always executes the `copyTree(source, staged)` + `g_file_move(staged, target)` + `removeTree(source)` pipeline rather than attempting a fast atomic same-filesystem `renameWithoutReplace(source, target)` first:
```cpp
        } else if (result->ok) {
          result->ok = g_file_move(staged, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, nullptr, nullptr, &error);
        }
        result->mutated |= result->ok;
        ...
        if (result->ok && data->kind == Kind::Move) {
          if (g_cancellable_set_error_if_cancelled(cancellable, &error)) result->ok = false;
          else result->ok = removeTree(source, nullptr, &error);
        }
```
Look closely at two behaviours here:
1. **Same-filesystem `Move` copies the entire tree instead of renaming:**
   Because `Kind::Move` enters the `else` branch at [line 1972](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1972) unconditionally, moving a 50 GiB folder or file from `/home/user/Downloads` to `/home/user/Documents` on the same filesystem performs a full 50 GiB byte-by-byte `copyTree` into `.nythera-partial-*`, renames it to `target`, and then recursively `removeTree`s the source!
   Even more telling: in `remember()` ([line 1347](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1347)) and `replay()` ([line 709](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L709)), Undo/Redo for `Kind::Move` expects same-device `Move` to have preserved the inode (`afterMove(actual, expected)` where `entry.sourceRevision` and `entry.resultRevision` share the same inode, because `replay()` undoes `Kind::Move` with `renameWithoutReplace(from, to)` and checks `afterMove(actual, *entry.sourceRevision)`!).
   Wait—verify this!
   In `worker()` ([line 2063](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L2063)):
   - During `Kind::Move`, `sourceBefore` is `revision(sourcePath)` (inode $I_1$).
   - `copyTree(source, staged)` creates a **new** inode $I_2$ at `staged`!
   - `staged` is moved to `target` (still inode $I_2$).
   - `removeTree(source)` deletes `sourcePath` (inode $I_1$).
   - Now look at [line 2036](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L2036):
     ```cpp
     const bool stableSource = sourceBefore && revision(sourcePath) == sourceBefore;
     ```
     Wait: `stableSource` is evaluated at **line 2036**, *before* `removeTree(source)` at line 2052! So `stableSource` is `true`!
   - Then at line 2063, `remember(sourcePath, committedPath, sourceBefore, current, sourceParent, destinationParent)` records:
     - `sourceRevision` = `sourceBefore` (inode $I_1$)
     - `resultRevision` = `current` (inode $I_2$)
   - Now what happens when the user clicks **Undo Move**?!
     Look at `FileOperationManager::replay()` ([lines 739–757](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L739-L757)):
     ```cpp
     if (moving) {
       const auto& from = undoing ? destinationPath : sourcePath;
       const auto& to = undoing ? sourcePath : destinationPath;
       const auto& expected = undoing ? entry.resultRevision : *entry.sourceRevision;
       if (!expectedAt(from, expected))
         return changedEntry(error, "The item is missing, replaced, or has been modified.");
       if (g_cancellable_set_error_if_cancelled(cancellable, error))
         return false;
       if (!renameWithoutReplace(from, to, error))
         return false;
       const auto actual = revision(to);
       if (!afterMove(actual, expected) || (S_ISDIR(expected.mode) && !emptyDirectory(to)))
         return restoreMovedItem(to, from, undoing ? entry.source : entry.destination);
       if (undoing)
         entry.sourceRevision = actual;
       else
         entry.resultRevision = *actual;
       return true;
     }
     ```
     `replay()` checks `expectedAt(from, entry.resultRevision)` (inode $I_2$), renames `destinationPath -> sourcePath` (inode $I_2$), and updates `entry.sourceRevision = actual` (inode $I_2$). So Undo works for single files, **except**:
     - For any non-empty directory move, `remember()` at [line 1345](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L1345) rejects directory transfers (`if (S_ISDIR(state.mode) && (isTransfer(data->kind) || !emptyDirectory(destination))) return;`), so directory moves never get Undo history.
     - And for same-filesystem file/directory moves, copying the entire file/tree instead of atomic `renameat2` wastes disk space, I/O bandwidth, wears SSDs, breaks hard links, changes inodes, and loses xattrs/ACLs not copied by `g_file_copy`!
2. **If cancelled between line 2033 and line 2051, or if `removeTree(source)` fails at line 2052:**
   At line 2033, `staged` has already been moved into `target` (`result->mutated = true`). If `cancellable` was triggered during the final bytes of `copyTree` or `g_file_move`, line 2051 sets `result->ok = false` and skips `removeTree(source)`. Now **both** `target` and `source` exist on disk, `result->ok` is `false` (reported as `"Transfer to <dest> was cancelled"`), and the user is not told that `target` was already committed while `source` was left intact (unlike `gioMoveWithBackup` lines 476–480, which explicitly reports `"A restored copy remains at <target>; the original could not be removed..."`).

#### Potential Consequences
- Moving a large file or directory within the same filesystem requires 2x free disk space and takes $O(\text{bytes})$ time instead of an $O(1)$ atomic directory entry rename.
- Cancellation or partial permission failure during the `removeTree(source)` phase of `Move` leaves both source and destination copies on disk without an explicit split-state diagnostic message.

#### Recommended Remediation
1. For `Kind::Move` when `!replaceExisting` (or after moving `target` to `backup`), try a direct same-filesystem move first (`renameWithoutReplace` when no conflict exists) before falling back to `copyTree` + `removeTree` on `EXDEV`.
2. In the cross-filesystem `copyTree` fallback for `Kind::Move`, once `staged` has been published to `target` (line 2033), if cancellation arrives or `removeTree(source)` fails at line 2052, either roll back `target` (if no pre-existing item was replaced) or set an explicit split-state status message matching `gioMoveWithBackup` (`"Moved copy remains at <destination>, but the original could not be removed from <source>: <error>"`).

---

### F-04: Synchronous main-thread filesystem probes (`statvfs`, `canonical`, `queryFileArtwork`, `queryFileTypeDescription`, `directorySupportsCreation`) stall Wayland UI on slow or hung NFS/CIFS/FUSE mounts

- **Category:** Concurrency / UI Blocking
- **Classification:** Confirmed Defect
- **Severity:** Medium (Complete UI freeze / Wayland compositor "application not responding" on slow network shares)
- **Likelihood:** High (Whenever browsing or having Places/bookmarks pointing to NFS, CIFS/SMB, sshfs, or rclone FUSE mounts)
- **Confidence:** High

#### Supporting Code Evidence
While `FileManager` uses async GIO calls for directory enumeration (`g_file_enumerate_children_async`) and sidebar capacity (`PlacesModel::queryCapacity`), several frequently executed UI-thread paths perform synchronous blocking POSIX/filesystem syscalls:

1. **Status bar `::statvfs(m_path.c_str(), &capacity)` and `queryFileTypeDescription(path)` on every selection or status change:**
   - [file_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L3102): `updateStatusBar()` runs synchronously on the main thread on every selection change, navigation, and timer tick, calling `::statvfs(m_path.c_str(), &capacity)`.
   - [file_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L3018): When 1 item is selected, `updateStatusBar()` calls `queryFileTypeDescription(path)` ([file_artwork.h](file:///mnt/Vegita/git-repos/nythera-shell/src/ui/dialogs/file_artwork.h#L30-L65)), which runs up to two synchronous `g_file_query_info` calls on the main thread (one `NOFOLLOW_SYMLINKS` and, for symlinks, one following symlinks).
2. **`FileIcon::doLayout` calls synchronous `queryFileArtwork` during frame layout:**
   - [file_icon.h](file:///mnt/Vegita/git-repos/nythera-shell/src/ui/dialogs/file_icon.h#L126): Inside `FileIcon::doLayout(Renderer&)`, every newly bound visible row or tile calls `queryFileArtwork(m_path, m_directory)` ([file_artwork.h](file:///mnt/Vegita/git-repos/nythera-shell/src/ui/dialogs/file_artwork.h#L67-L107)), which executes synchronous `g_file_query_info` (twice for symlinks!) right inside the Wayland frame layout pass. Even though `FileManager::enumerate` already queried `standard::type,standard::content-type` asynchronously, `FileEntryRow::bind` only passes `(entry.absPath, entry.isDir)` to `FileIcon::setFile`, causing `FileIcon` to re-query the filesystem synchronously during layout.
3. **Menu construction and Places discovery call `std::filesystem::canonical` and `directorySupportsCreation` (`access(2)`/`statfs(2)`) on the main thread:**
   - [file_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L2625-L2631): `fileMenuEntries()` calls `std::filesystem::canonical` on the selected entry and every personal place row whenever the File menu opens.
   - [places_model.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/places_model.cpp#L482-L484) and [mount_backends.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/mount_backends.cpp#L265): `PlacesModel::discover()` and `mountRecords()` run synchronously on the main thread during `rebuildPlacesSidebar()`, calling `std::filesystem::canonical`, `g_volume_get_identifier`, and `/sys/class/block` directory iteration.

#### Potential Consequences
- Scrolling a directory on a high-latency NFS/SSHFS/SMB mount performs synchronous `g_file_query_info` calls per newly visible row inside `doLayout()`, causing frame drops and input stutter.
- Selecting files or opening menus in a stalled network mount blocks the main thread in `statvfs()` / `g_file_query_info()`, freezing the entire Wayland client until the kernel RPC times out.

#### Recommended Remediation
1. Populate `FileArtwork` (or `GIcon` / symlink state) from the `GFileInfo*` already fetched asynchronously in `FileManager::fetchNext` and `queryMonitoredFile`, and pass it into `FileEntry` / `FileIcon` so `FileIcon::doLayout()` does not perform synchronous `g_file_query_info` calls for listed entries.
2. Replace the synchronous `::statvfs(m_path.c_str(), ...)` in `updateStatusBar()` with cached capacity data from `m_capacityCache` or an async `g_file_query_filesystem_info_async` tied to `m_enumerationGeneration`.

---

### F-05: `ThumbnailService` follows symlinks, reads up to 256 MiB per worker into memory before decoding, and writes non-atomically to predictable cache filenames

- **Category:** Security / Resource Exhaustion / Concurrency
- **Classification:** Confirmed Defect
- **Severity:** Medium (Memory exhaustion DoS or cross-file symlink reading/clobbering in cache)
- **Likelihood:** Medium
- **Confidence:** High

#### Supporting Code Evidence
In [thumbnail_service.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/render/core/thumbnail_service.cpp#L496-L544):
1. **Follows symlinks without `O_NOFOLLOW`:**
   - `FileEntryRow::bind` ([file_entry_row.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/ui/dialogs/file_entry_row.cpp#L214)) and `FileEntryTile::bind` ([file_entry_tile.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/ui/dialogs/file_entry_tile.cpp#L116)) check only `!entry.isDir && DirectoryScanner::isImagePath(entry.absPath)` (which is purely a filename extension check, e.g. `evil.png`).
   - `cachePathForSource` ([thumbnail_service.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/render/core/thumbnail_service.cpp#L60-L84)) and `FileUtils::readBinaryFile` ([file_utils.h](file:///mnt/Vegita/git-repos/nythera-shell/src/util/file_utils.h#L454-L486)) use `std::filesystem::is_regular_file`, `file_size`, `last_write_time`, and `std::ifstream`, all of which follow symlinks.
2. **Up to 256 MiB per worker thread (up to 4 workers = 1 GiB) read into memory before image header check:**
   - `FileUtils::readBinaryFile` ([file_utils.h](file:///mnt/Vegita/git-repos/nythera-shell/src/util/file_utils.h#L470)) allows files up to `kMaxBinaryReadBytes = 256 MiB`, plus `decodeRasterImage` allocates the decoded RGBA buffer. Entering a folder with four 250 MiB `.png`-named files (or symlinks to large files) simultaneously allocates >1 GiB across the 4 worker threads.
3. **Non-atomic, world/umask-permissive thumbnail cache writes:**
   - `writeFile` ([thumbnail_service.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/render/core/thumbnail_service.cpp#L86-L99)) creates `~/.cache/nythera/thumbnails` with default `std::filesystem::create_directories` permissions (typically `0755`, rather than `0700` via `FileUtils::createPrivateDirectories`) and writes directly in-place with `std::ofstream(path, std::ios::binary | std::ios::trunc)` instead of `writeTextFileAtomic` or `O_NOFOLLOW | O_CREAT | O_EXCL` temp-file rename.
   - Because `fnv1a64` over `(sourcePath, size, mtime, targetPx, version)` is deterministic and non-cryptographic, if the cache directory is readable on a shared system (or if `/tmp/nythera/thumbnails` fallback at [line 38](file:///mnt/Vegita/git-repos/nythera-shell/src/render/core/thumbnail_service.cpp#L38) is ever reached when `HOME` and `XDG_CACHE_HOME` are unset!), a local attacker in `/tmp/nythera/thumbnails` could pre-stage symlinks or poison cached thumbnails.

#### Potential Consequences
- `/tmp/nythera/thumbnails` fallback (when `HOME` and `XDG_CACHE_HOME` are unset) combined with `std::ofstream(..., std::ios::trunc)` allows classic `/tmp` symlink clobbering of arbitrary user files.
- High memory spikes when browsing directories containing large files with image extensions.
- Thumbnail cache files in `~/.cache/nythera/thumbnails` are created with default umask (`0644` inside `0755`), exposing thumbnail previews of private directories to other local users on multi-user systems.

#### Recommended Remediation
1. In `ThumbnailService`, create the cache directory with `FileUtils::createPrivateDirectories` (`0700`), and never fall back to a shared `/tmp/nythera/thumbnails` path without a per-UID `0700` directory check (or `g_get_user_cache_dir()`).
2. Write cached `.webp` files atomically via a sibling temporary file and `rename` (or `O_NOFOLLOW`), and set `0600` permissions.
3. Cap thumbnail source file reads at a tighter bound (e.g., 32–64 MiB) and use `O_NOFOLLOW` (or skip symlinks if non-follow policy is desired).

---

### F-06: `FileManager` GIO async callbacks (`enumerate`, `fetchNext`, `queryMonitoredFile`, `activatePath`, `closeDialog`) capture unguarded raw `this`

- **Category:** Concurrency / Object Lifetimes
- **Classification:** Confirmed Defect
- **Severity:** Medium (Potential UAF during shutdown or test teardown)
- **Likelihood:** Low (In production `main.cpp`, `FileManager` lives until `run()` returns; however, unit tests like `file_manager_paste_selection_test.cpp` and `file_manager_wastebin_test.cpp` instantiate and destroy `FileManager` directly while the default `GMainContext` persists across tests)
- **Confidence:** High

#### Supporting Code Evidence
`FileManager` maintains `std::shared_ptr<bool> m_alive = std::make_shared<bool>(true)` ([file_manager.h](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.h#L197)) and sets `*m_alive = false` in `~FileManager()` ([file_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L1387)). It uses `m_alive` properly in folder counts ([line 1774](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L1774)), checksums ([line 5881](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L5881)), device operations ([line 3532](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L3532)), and location-edit cancellation ([line 4322](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L4322)).

However, the following async GIO / GLib callbacks pass raw `this` with **no** `GCancellable` and **no** `m_alive` check:
- `FileManager::enumerate` -> `g_file_enumerate_children_async` ([lines 1857–1895](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L1857-L1895))
- `FileManager::fetchNext` -> `g_file_enumerator_next_files_async` ([lines 1961–2047](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L1961-L2047))
- `FileManager::queryMonitoredFile` -> `g_file_query_info_async` ([lines 4738–4807](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L4738-L4807))
- `FileManager::activatePath` -> `g_app_info_launch_default_for_uri_async` ([lines 2955–2967](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L2955-L2967))
- `FileManager::closeDialog` -> `g_idle_add` ([lines 6169–6206](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L6169-L6206))
- Surface closed callbacks -> `g_idle_add` ([lines 1519–1540](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L1519-L1540), [5429–5444](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L5429-L5444))

#### Potential Consequences
- If `FileManager` is destroyed while an enumeration, monitored-file query, URI launch, or queued dialog/surface close idle source is still pending on the default `GMainContext`, the next main-context iteration dereferences the destroyed `FileManager`.
- In `FileManager::enumerate()`, because no `GCancellable*` is passed to `g_file_enumerate_children_async` or `g_file_enumerator_next_files_async`, rapidly navigating across slow directories keeps old enumerator worker tasks alive in GIO's thread pool until they finish and are discarded by the `requestGeneration != self->m_enumerationGeneration` check.

#### Recommended Remediation
1. Include `std::weak_ptr<bool> alive = m_alive` in the user-data structs for `enumerate`, `fetchNext`, `queryMonitoredFile`, `activatePath`, and the `g_idle_add` closures in `closeDialog` and surface-close handlers.
2. Add a `GCancellable* m_enumerationCancellable` member to `FileManager` that is cancelled and replaced on each `enumerate()` call and in `~FileManager()`.

---

### F-07: Conflict resolver dialog parser splits on `\n\n` and `\n`, corrupting conflict card display and `"Suggest New Name"` when filenames or paths contain newlines

- **Category:** Data Integrity / UI Correctness
- **Classification:** Confirmed Defect
- **Severity:** Medium (Wrong destination path parsed for `"Suggest New Name"` / alternative-name existence check, and garbled conflict cards)
- **Likelihood:** Low (Requires filenames or directory names containing `\n`, which are valid on Linux filesystems and explicitly tested in `tests/file_transfer_payload_test.cpp` and `tests/checksum_test.cpp`)
- **Confidence:** High

#### Supporting Code Evidence
In [file_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L654-L664), the conflict resolver serialises 6 fields into a single newline-delimited string `message`:
```cpp
    const std::string message = sourceName
        + "\n"
        + describe(source)
        + "\n"
        + source
        + "\n\n"
        + destName
        + "\n"
        + describe(destination)
        + "\n"
        + destination;
```
Then, in `buildDialog()` ([lines 6336–6389](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L6336-L6389) and [lines 6451–6458](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L6451-L6458)), `FileManager` parses `m_dialogMessage` back out by searching for `\n\n` and counting `\n` offsets:
```cpp
      const auto incomingName = m_dialogMessage.substr(0, m_dialogMessage.find('\n'));
      const auto existingStart = m_dialogMessage.find("\n\n") + 2;
      const auto existingMetadataEnd = m_dialogMessage.find('\n', m_dialogMessage.find('\n', existingStart) + 1);
      const auto destinationEnd = m_dialogMessage.find('\n', existingMetadataEnd + 1);
      const auto destinationPath = std::filesystem::path(
          m_dialogMessage.substr(existingMetadataEnd + 1, destinationEnd - existingMetadataEnd - 1)
      );
      m_dialogDestination = destinationPath;
```
And in `closeDialog()` ([lines 6158–6160](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L6158-L6160)):
```cpp
      const bool occupied = std::filesystem::exists(m_dialogDestination.parent_path() / name, error);
```

#### Potential Consequences
- If a filename or ancestor directory path contains `\n` (or `\n\n`), `m_dialogMessage.find("\n\n")` and the fixed line-index `find('\n')` calls split at the wrong offsets.
- `m_dialogDestination` is set to a truncated/corrupted path, causing `"Suggest New Name"` and `closeDialog()`'s pre-check to probe the wrong directory, and the comparison cards display wrong names/paths.

#### Recommended Remediation
Pass a structured `ConflictDialogInfo` (containing `sourcePath`, `destinationPath`, `sourceDescription`, `destinationDescription`, `comparisonSummary`, `isDirectory`, `kind`) through the `g_idle_add` tuple into `FileManager` instead of packing and re-parsing a `\n`-delimited string.

---

### F-08: Office template creation (`New Document/Spreadsheet/Presentation`) runs synchronously on the UI thread, bypasses `PrivateStaging`, and has a TOCTOU window

- **Category:** Data Integrity / Architecture
- **Classification:** Confirmed Defect
- **Severity:** Low–Medium
- **Likelihood:** Low
- **Confidence:** High

#### Supporting Code Evidence
In [file_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L2431-L2475), creating an Office document (`entry.id` 114–116):
1. Resolves a free filename via `filemanager::creationDestinationWithExtension(m_path, defaultName, extension, destinationError)` ([file_creation.h](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_creation.h#L55)), which uses `std::filesystem::exists(candidate, error)`.
   - **Bug 1 (Dangling symlink collision):** `std::filesystem::exists()` follows symlinks and returns `false` for a **dangling symlink**! If `New Document.odt` is a dangling symlink in `m_path`, `creationDestinationWithExtension` selects `New Document.odt` as unoccupied, and `g_file_copy` then fails with `G_IO_ERROR_EXISTS` (or for `CreateFolder`/`CreateFile` at lines 2483–2491, `FileOperationManager` fails instead of picking `New Folder 2`).
2. Executes `g_file_copy(source, target, G_FILE_COPY_NONE, nullptr, nullptr, nullptr, &error)` **synchronously on the UI thread** instead of routing through `FileOperationManager`.
3. Because it bypasses `FileOperationManager`, creating an Office document from a template **cannot be undone** with `Ctrl+Z` (it calls `m_operations.noteExternalMutation()`, clearing the Redo stack without adding an Undo entry).

#### Potential Consequences
- Dangling symlinks named `New Folder`, `New File.txt`, or `New Document.odt` cause item creation to fail instead of advancing to suffix ` 2`.
- Synchronous `g_file_copy` blocks the UI thread on slow/network filesystems.
- Inconsistent UX: `New Folder`, `New Empty File`, `New Text File`, and `New Symlink` are all undoable via `FileOperationManager`, whereas `New Document/Spreadsheet/Presentation` is not undoable and clears Redo history.

#### Recommended Remediation
1. In `filemanager::creationDestinationWithExtension` ([file_creation.h](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_creation.h#L55)) and `closeDialog` ([file_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L6159)) and `"Suggest New Name"` ([line 6470](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L6470)), use `std::filesystem::exists(std::filesystem::symlink_status(candidate, error))` so dangling symlinks are treated as occupied directory entries.
2. Route template-based file creation through `FileOperationManager` (or `copy()` / a template-aware creation helper) so it runs on a worker thread, stages atomically, and participates in Undo/Redo.

---

### F-09: `external_tools::launchEntry` appends arguments to `.desktop` `Exec=` before field codes are stripped and uses `is_regular_file` (following symlinks) for tool eligibility

- **Category:** Security / Robustness
- **Classification:** Plausible Risk / Edge-Case Defect
- **Severity:** Low–Medium
- **Likelihood:** Low
- **Confidence:** High

#### Supporting Code Evidence
In [external_tools.h](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/external_tools.h#L275-L332):
1. When `launchEntry` prepares a terminal `.desktop` file, it reads the raw `Exec` key from the `.desktop` `GKeyFile` ([line 288](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/external_tools.h#L288)) and **appends** `--gtk-single-instance=false`, `TerminalArgDir`, and `TerminalArgExec /bin/sh -c 'cd -- "$1" || exit; exec "${SHELL:-/bin/sh}"' sh <directory>` to the end of `commandLine`.
   - If a terminal's `.desktop` `Exec=` line already ends with a command flag or field code (such as `foot %F` or `xterm -e %F` or a wrapper script), appending `TerminalArgDir` or `TerminalArgExec` *after* the existing `Exec` string can place the directory/exec flags *after* a positional or `-e` argument.
2. In `validTargets` ([lines 228–238](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/external_tools.h#L228-L238)):
   ```cpp
   return compare ? std::filesystem::is_regular_file(path, error) : std::filesystem::is_directory(path, error);
   ```
   Unlike `checksum::eligibleSelection` ([checksum.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/checksum.cpp#L126)), which uses `std::filesystem::symlink_status(path, error)` to reject symlinks, `validTargets` uses `std::filesystem::is_regular_file(path, error)` (which follows symlinks), and only checks `paths[0] != paths[1]` lexically.

#### Potential Consequences
- A terminal `.desktop` entry whose `Exec=` line contains trailing field codes or `-e` options may misparse the appended `TerminalArgDir` / `TerminalArgExec` arguments.

#### Recommended Remediation
- Strip standard desktop entry field codes (`%f`, `%F`, `%u`, `%U`) from `commandLine` when synthesising a terminal `Exec=` override in `launchEntry`, or only append `TerminalArgExec` when no conflicting exec token is present.

---

### F-10: `PlacesModel::operate` uses a function-local `static bool active` guard shared across instances

- **Category:** Concurrency / Architecture
- **Classification:** Confirmed Defect
- **Severity:** Low
- **Likelihood:** Low
- **Confidence:** High

#### Supporting Code Evidence
In [places_model.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/places_model.cpp#L737-L746):
```cpp
  void PlacesModel::operate(
      const Place& place, PlaceOperation operation, GMountOperation* prompts, GCancellable* cancellable,
      std::function<void(std::string)> completion
  ) {
    static bool active = false;
    if (active) {
      completion("Wait for the current device operation to finish.");
      return;
    }
    active = true;
    completion = [done = std::move(completion)](std::string message) {
      active = false;
      done(std::move(message));
    };
```
If the `completion` functor is ever destroyed without being invoked (for example, if a future change allows `GTask` or subprocess cancellation without invoking the callback), `active` would remain permanently `true` for the lifetime of the process. Currently all paths in `operate`, `operateManagedMount`, and `waitForUnmount` do invoke `completion`, and `operate` is only called on the main thread, so this does not cause a data race today—but a hidden process-global `static bool` with no RAII reset guard is fragile.

#### Recommended Remediation
Wrap the `active` flag reset in a move-only RAII token stored inside the `completion` capture (so `active` is guaranteed to reset even if the callback object is destroyed without being called), or use `std::atomic<bool>`.

---

### F-11: `src/filemanager/file_manager.cpp` is a 7,469-line monolith combining windowing, scene-graph construction, dialogs, menus, monitors, and state persistence

- **Category:** Architecture / Maintainability
- **Classification:** Architectural Weakness
- **Severity:** Low
- **Likelihood:** N/A
- **Confidence:** High

#### Supporting Code Evidence
- [src/filemanager/file_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp) is 7,469 lines long, and [src/filemanager/file_manager.h](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.h) has over 150 member variables spanning 8 distinct responsibilities:
  1. Main window chrome, toolbar, breadcrumbs, and status bar (`prepareFrame`, `updateStatusBar`)
  2. Directory enumeration and `GFileMonitor` delta batching (`enumerate`, `fetchNext`, `handleMonitorEvent`, `queryMonitoredFile`)
  3. Places sidebar UI and capacity caching (`rebuildPlacesSidebar`, `requestPlaceCapacity`, `operatePlace`)
  4. Drag-and-drop and clipboard orchestration (`updateProtocolDrag`, `receiveProtocolDrop`, `pasteClipboard`)
  5. Generic modal dialog surface & layout engine (`showDialog`, `buildDialog`, `closeDialog`, `prepareDialogFrame`)
  6. Checksum tool dialog and worker orchestration (`showChecksumDialog`, `startChecksum`, `buildChecksumFields`)
  7. Symlink creation dialog (`showSymlinkDialog`, `submitSymlinkDialog`)
  8. Fallback progress window (`syncProgressWindow`, `prepareProgressFrame`)
- Notably, recent features have already started extracting pure logic headers (`file_columns.h`, `file_sort.h`, `file_interaction.h`, `file_creation.h`, `external_tools.h`, `checksum.h`/`checksum.cpp`, `properties_window.cpp`), which keeps unit testability high.
- Similarly, as documented in [NYTHERA-FILES-STANDALONE-AUDIT.md](file:///mnt/Vegita/git-repos/nythera-shell/NYTHERA-FILES-STANDALONE-AUDIT.md), the `nythera-file-manager` build target currently compiles against the monolithic `_nythera_sources` static core (pulling in loader dependencies like PipeWire, WirePlumber, libqalculate, PAM, and libical even though Files never uses them).

#### Potential Consequences
- High cognitive load and shared mutable dialog state (`m_dialogSurface`, `m_dialogAlternativeName`, `m_textPrompt`, `m_passwordPrompt`, `m_symlinkDialogDestination`, `m_checksumDialog` all multiplexing one dialog host in `FileManager`).
- For example, because `showDialog` only supports a single active `m_dialogSurface` ([line 5959](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_manager.cpp#L5959)), if a background transfer hits a conflict while the user has the **Checksum** or **Create Symlink** or **Mount Password** dialog open, `showDialog` immediately returns `response(-1)` (cancelling the background file transfer conflict automatically without showing the conflict dialog!).

#### Recommended Remediation
- Do **not** perform a speculative rewrite.
- As a targeted fix for the dialog multiplexing hazard: hold `m_conflictMutex` or wait on a main-thread condition when `m_dialogSurface` is already occupied by a non-conflict dialog (or give the conflict dialog its own `ToplevelSurface` / queue) so an open Checksum or Symlink dialog does not silently cancel a background transfer when a conflict arises.

---

## Existing Protections to Preserve

During any remediation work, the following well-engineered protections in Nythera Files **must be preserved**:

1. **Kernel-scoped descendant opening (`openPropertyDescendant` in [property_operations.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/property_operations.cpp#L57-L68)):**
   Uses `SYS_openat2` with `O_PATH | O_CLOEXEC | O_NOFOLLOW` and `RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_BENEATH | RESOLVE_NO_XDEV`, followed by `/proc/self/fd/<fd>` operations. This defeats symlink races, magic-link escapes, and mount-crossing during recursive chmod and folder-size calculation.
2. **Parent-FD pinning and 7-tuple revision checks in Undo/Redo (`ReplayParent` and `FileOperationManager::replay` in [file_operation_manager.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/file_operation_manager.cpp#L589-L811)):**
   Pins parent directories by `(dev, ino)` with `O_PATH | O_DIRECTORY`, verifies full `lstat` fingerprints of both source and destination before and after replay, isolates items in a `0700` `PrivateStaging` directory before unlinking, refuses to undo a copy if the original source was deleted or modified, and uses `renameat2(..., RENAME_NOREPLACE)`.
3. **Polkit helper privilege hardening ([property_operation_client.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/property_operation_client.cpp#L12-L95) & [property_helper_main.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/property_helper_main.cpp#L15-L53)):**
   Verifies root ownership and `!(st_mode & 0022)` on every ancestor directory of the helper binary and policy file, verifies Polkit action registration rather than falling back to generic `pkexec`, base64-encodes paths with canonical round-trip checks, locks target inodes with `flock(LOCK_EX | LOCK_NB)`, and uses a non-blocking `stdin` `'G'` readiness byte + `POLLHUP` pipe monitor so client cancellation or crash immediately stops privileged traversal.
4. **Mount/Unmount/Eject safety gates ([mount_backends.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/mount_backends.cpp#L202-L246) & [places_model.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/places_model.cpp#L66-L93)):**
   Protects critical system paths (including symlink-relocated `/home`), rejects shared Btrfs device unmounts via UDisks/GIO in favour of exact `systemd` mount units, checks systemd unit dependency properties, blocks unmount/eject when `m_operations.usesMount(path)` is active (even while a cancelled worker is still unwinding), and refuses to pass `--force` or `--lazy` to unmount helpers.
5. **Checksum TOCTOU and manifest traversal protections ([checksum.cpp](file:///mnt/Vegita/git-repos/nythera-shell/src/filemanager/checksum.cpp#L48-L111)):**
   Opens files with `O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC`, compares `lstat` before open, `fstat` after open, `fstat` at EOF, and `lstat` at EOF, caps manifests at 4 MiB, and treats manifest filenames strictly as inert labels.

---

## Testing Audit: Existing Coverage vs. Missing Coverage

### Strong Existing Test Coverage
Nythera Files has an extensive suite of 25+ focused C++ tests covering:
- **File operations & Undo/Redo:** [tests/file_operation_manager_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/file_operation_manager_test.cpp) (copy, move, duplicate, symlink creation, conflict resolution, directory merge, cross-device `/dev/shm` moves, mount protection during cancellation, Undo/Redo revision checks, parent relocation, source deletion protection).
- **Transfer pause/resume:** [tests/file_transfer_pause_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/file_transfer_pause_test.cpp) & [tests/transfer_rates_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/transfer_rates_test.cpp).
- **Wastebin operations & UI:** [tests/file_wastebin_operation_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/file_wastebin_operation_test.cpp) & [tests/file_manager_wastebin_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/file_manager_wastebin_test.cpp).
- **Paste & DnD selection identity:** [tests/file_manager_paste_selection_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/file_manager_paste_selection_test.cpp), [tests/file_transfer_payload_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/file_transfer_payload_test.cpp), [tests/native_data_transfer_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/native_data_transfer_test.cpp).
- **Properties & permissions:** [tests/property_operations_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/property_operations_test.cpp), [tests/property_permissions_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/property_permissions_test.cpp), [tests/property_metadata_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/property_metadata_test.cpp), [tests/property_scan_presentation_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/property_scan_presentation_test.cpp), [tests/folder_size_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/folder_size_test.cpp).
- **Places & mount backends:** [tests/places_model_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/places_model_test.cpp) & [tests/mount_backends_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/mount_backends_test.cpp).
- **Checksums:** [tests/checksum_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/checksum_test.cpp).

### Missing Test Coverage (Recommended Additions)
1. **`FileOperationManager` destruction while a worker thread is in-flight (`F-01`):**
   - No test destroys a `FileOperationManager` instance while a copy/move/conflict-resolution task is active and then pumps the GLib main loop (`g_main_context_iteration(nullptr, false)`) under ASan/TSan.
2. **Concurrent destination creation during non-replace `Copy`/`Move` (`F-02`):**
   - Using `FileOperationManagerTestAccess::boundary`, create a file at the destination path *after* `copyTree` has started writing to `.nythera-partial-*` and verify that finalisation refuses to overwrite the newly appeared destination file (`RENAME_NOREPLACE`).
3. **Failed `removeTree(source)` or late cancellation during cross-filesystem `Move` (`F-03`):**
   - Test a `Move` of a directory containing an unremovable child (e.g. `0500` subdirectory) or cancellation at the post-copy checkpoint, verifying that the resulting operation status clearly reports the split state or rolls back cleanly.
4. **Dangling symlink collision in `filemanager::creationDestination` (`F-08`):**
   - Add a case in [tests/file_interaction_test.cpp](file:///mnt/Vegita/git-repos/nythera-shell/tests/file_interaction_test.cpp) where `creationRoot / "notes"` is a dangling symlink (`std::filesystem::create_symlink("missing", creationRoot / "notes")`) and verify `creationDestination(creationRoot, "notes", false, ec)` returns `creationRoot / "notes 2"`.
5. **Unit tests for `src/filemanager/external_tools.h` (`F-09`):**
   - `external_tools.h` (354 lines of `.desktop` parsing, `xdg-terminals.list` parsing, `mimeapps.list` lookup, Ghostty single-instance argument rewriting, and shell quoting) currently has **no dedicated unit test** in `tests/` (only the split-menu UI widget is tested in `file_entry_row_layout_test.cpp`).
6. **Filenames containing `\n` in conflict resolution dialogs (`F-07`):**
   - Test conflict dialog metadata extraction and `"Suggest New Name"` when a conflicting filename or directory path contains a newline character.
