# Files Properties

Properties is a single native window with a stable item context. Alt+Enter and the item context menu open the same window; multiple-item Properties is unsupported. General, Permissions and Details retain the approved layout. Long Details values have full tooltips and a More/Less action that wraps the value inside its existing column.

General owns the item icon and editable filename; tabs and the compact close control sit at the top without a separate title row. Enter confirms a filename edit through the existing asynchronous rename manager; Escape restores the original draft. Validation is shared with inline rename. Checked renames verify the source device/inode before execution, reject collisions without replacement, retain failed input and update the Properties context and browser selection on success. Like the existing path-based rename API, these checks cannot make concurrent namespace edits atomic with the rename syscall.

General uses GIO content-type descriptions, while Details keeps the technical MIME identifier and literal Filename suffix. Drive-wide metadata and capacity appear only for an exact enclosing-mount-root match, never merely because the item belongs to a filesystem. Read-only filesystem capability checks remain separate from visible drive details. Subtle copy actions use Files' existing desktop clipboard writer and copy the complete value. Transparent edge/corner input regions provide resize cursors and resizing without a visible grip. Files' root breadcrumb is `/`; navigation paths and Places names are unchanged.

Owner/group controls use native selects, with the current account and normal users prioritised using local login policy, and eligible groups before other groups. Ownership changes remain staged. Read/Write/Execute (Traverse for folders) columns replace per-row summaries. Dropdown dismissal and keyboard navigation use the existing native popup implementation.

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

- Window and entry points: `src/filemanager/properties_window.cpp`, `properties_window.h`, `file_manager.cpp`, `file_manager.h`; shared name validation and root label: `src/filemanager/file_interaction.h`, `file_breadcrumbs.h`.
- Shared pinned-inode permission primitive: `src/filemanager/property_permissions.h`; item metadata helpers: `src/filemanager/property_metadata.h`.
- Request validation, account resolution and bounded traversal: `src/filemanager/property_operations.cpp`, `property_operations.h`.
- Unprivileged authorisation client and installed helper entry point: `src/filemanager/property_operation_client.cpp`, `property_helper_main.cpp`.
- Existing job integration: `src/filemanager/file_operation_manager.cpp`, `file_operation_manager.h`.
- Build and policy: `meson.build`, `data/polkit/dev.nythera.Files.modify-properties.policy.in`.
- Focused coverage: `tests/property_permissions_test.cpp`, `property_operations_test.cpp`, `property_metadata_test.cpp`, `file_operation_manager_test.cpp`, `file_interaction_test.cpp`.
- Behaviour and installation record: this document and `docs/NYTHERA-CUSTOMISATIONS.md`.
