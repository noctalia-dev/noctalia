# Nythera Agent Instructions

Repository-specific guidance supplementing the global Codex `AGENTS.md`. Follow global instructions for general research, initiative, safety, efficient tool use and reporting; do not treat this file as replacing them.

## Project context and invariants

- **Nythera** is a C++23 native Wayland shell fork, built with **Meson/Ninja** and orchestrated by `just`—not the original Noctalia or a QML project.
- Development primarily targets **Umbriel**. Preserve other compositor integrations unless explicitly changing them; route compositor-specific behaviour through `CompositorPlatform` and its backend.
- Preserve upstream functionality and approved Nythera design decisions: floating islands, dynamic bars, screen wrapping, configurable styling and dark sci-fi/fantasy aesthetics. Do not reinterpret approved behaviour or introduce unrelated redesigns.
- Distinguish Nythera-owned identifiers from Umbriel-owned integration contracts before renaming or modifying them.
- Follow relevant project documentation listed below. Surface conflicts with documented technical invariants rather than silently bypassing them.

## Repository workflow

1. **Orient and inspect.** Check `git status --short` before edits. Identify the owning subsystem using `rg --files`, focused `rg -n` searches and bounded `sed -n` reads. Exclude `third_party/`, build trees, generated output, caches and large assets unless relevant. Check only relevant docs and diffs.
2. **Implement coherently.** Keep related edits together and avoid touching unrelated code or mass reformatting. For research-only requests, inspect and explain without editing.
3. **Validate proportionately.** Use the change-type matrix below, generally one incremental build and focused tests rather than whole-tree checks. Additional validation is justified by concrete failures, risk or cross-cutting impact.
4. **Verify live UI when appropriate.** Use `nythera restart` only after successful build/applicable tests, when necessary and explicitly authorised to interrupt the active shell. Never restart during a locked session or after a failed build/test.
5. **Review before finishing.** Use `git diff --check`, review the changed-file list and focused diff, and check affected architectural invariants. Recheck after subsequent edits when necessary.

## Nythera-specific tool practices

- Prefer repository source to external browsing; consult external docs for unresolved API, compatibility or upstream-change questions.
- Prefer direct Git, `rg`/`sed` and focused patches for ordinary operations. Use Python or shell scripts for structured processing, image/font work, reliable transformations or reproducible diagnostics—not to wrap a single trivial command.
- Keep compiler/test output in local logs and inspect error excerpts rather than dumping full logs. Await process completion without repeated polling; check exit codes.
- Use existing captures for unchanged visual states. Capture new or cropped views when rendered behaviour changes or evidence is insufficient. The user's visual review can be the final acceptance check; do not equate compilation with visual verification.
- Do not use subagents for routine work; delegate only independent bounded work where coordination costs are justified.
- Do not modify live shell configuration, install packages, clean builds or change global system configuration without explicit authorisation.

## Choose validation by change type

| Change                                                                                    | Default validation                                                                                                                                                                                                   |
| ----------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Documentation or comments only                                                            | `git diff --check`; run focused docs formatting check when Markdown structure or syntax changes. No C++ build/tests.                                                                                                 |
| Pure UI styling, spacing, geometry or animations                                          | Incremental `just build` if C++ changed; visual inspection in the real shell only if an appropriate working session is available and it adds value. Do not run the entire test suite or fabricate mock visual tests. |
| Normal C++ behaviour changes                                                              | Incremental `just build`; run only directly related Meson tests using `meson test -C build-debug <test-name> --print-errorlogs`.                                                                                     |
| Configuration, persistence, parsing, security or nontrivial state transitions             | Incremental build and directly relevant tests; add a focused regression test when it protects a deterministic, meaningful contract not already covered.                                                              |
| Public headers, shared UI primitives, event loop, rendering backend or broad architecture | Incremental build plus a reasoned selection of affected tests; expand to full suite only when impact cannot be bounded.                                                                                              |
| Translation key additions or renames                                                      | `python3 tools/i18n-check.py` plus change-relevant checks. Edit the English catalogue only unless explicitly tasked otherwise.                                                                                       |
| Palette role/alpha changes                                                                | `just role-alpha-check` plus relevant checks.                                                                                                                                                                        |
| CI/workflow scripts                                                                       | `just test-workflows` when relevant; no C++ build unless build logic changes.                                                                                                                                        |

- Use the existing `build-debug/` directory and incremental builds when available and correctly configured. `just build` configures if necessary; do not run `just configure` before every build.
- Do not run `just rebuild`, `just clean`, ASan, release builds, full `just lint`, full `just test`, or multi-distro builds by default.
- Prefer focused clang-tidy for touched `.cpp` files if lint is warranted: `clang-tidy -p build-debug --warnings-as-errors='*' <touched-files>`. Whole-tree `just lint` is a CI/explicit-request operation.
- Prefer formatting **only touched C++ files** with the project's configured clang-format version (currently 22). Project-wide `just format-code` rewrites all source/test files and should not be used for narrow changes.
- For docs, use the repository remark formatter or `just format-docs-check` when needed; note that the docs recipe may install its pinned local npm dependencies.
- Before a targeted Meson test, use `meson test -C build-debug --list` if the exact test name is unknown; do this once, not repeatedly. If no matching test exists, don't invent an arbitrary test merely to tick a box.

## Working with project documentation

- `ARCHITECTURE.md`: UI lifecycle, rendering, services, surfaces and other implementation invariants; required for architectural/refactoring work.
- `CONTRIBUTING.md` and `BUILDING.md`: coding conventions, build/test guidance and contributor process as relevant.
- `SCOPE.md`: distinguish approved scope, experiments and future plans.
- `REBRAND.md`: Nythera-owned identity, external Noctalia contracts, plugin translations and migration; required for naming/API changes.
- `docs/NYTHERA-CUSTOMISATIONS.md`: documented Nythera behaviours, development status and compatibility risks. Check actual code before treating an item as implemented.
- `docs/UPSTREAM-INTEGRATION.md`: required procedure for integrating changes from Noctalia upstream. Do not use the standard narrow-change budget to justify skipping cross-subsystem compatibility checks for a broad update.
- When changing a documented behaviour or architectural contract, update its owning document without duplicating rules across files.

## Git safety and upstream integration

- Protect uncommitted work: do not reset, overwrite, stash, rebase, force-push or merge into `main` without explicit approval and an accounted-for state.
- Use `dev` for ordinary development. For substantial or risky changes, especially upstream integrations, create a temporary task/integration branch (or a separate worktree when warranted). Do not create a branch for every trivial adjustment.
- Treat upstream integration as a **semantic migration**, not just Git conflict resolution: evaluate changed APIs and indirect Nythera dependencies, including custom files absent from the upstream diff.
- Preserve Nythera's **intended behaviour**, not necessarily old code. Where upstream replaces an architecture, migrate customisation to the new interface instead of blindly retaining obsolete mechanisms.
- For the first Noctalia update, start from the verified shared-ancestor SHA in `docs/UPSTREAM-INTEGRATION.md` and verify it against freshly fetched history; no upstream merge has occurred since the fork. For later updates, track the last upstream target merged/imported and carry forward exclusions or deferred items. Never infer completeness from ahead/behind counts, a clean merge or selective imports.
- Account for each relevant upstream change as incorporated, adapted, deliberately excluded, deferred or blocked, with rationale. A clean Git merge and successful build alone do not prove compatibility.
- For integration tasks, follow `docs/UPSTREAM-INTEGRATION.md`, check impacted entries in `docs/NYTHERA-CUSTOMISATIONS.md`, and record actual validation plus unresolved work before declaring completion.
- GitHub CI currently runs on pull requests and pushes to `main`, not ordinary pushes to `dev`. Do not treat a successful `dev` push as proof that CI passed; report actual checks performed.

### Committing and pushing session work

- When asked to commit or push "your changes", identify the complete relevant change set from the current task or session, not merely the most recently edited file. Include related implementation, tests, assets, documentation, additions and deletions.
- Use conversation context, current Git status and focused diffs to distinguish session work from unrelated user changes. Do not assume pre-existing uncommitted files are unrelated merely because they were modified earlier in a long session.
- Review the files to be staged and commit the full relevant change set; do not blindly stage the entire working tree. If ownership or inclusion is genuinely uncertain, ask before including those files.
- After pushing, verify the remote branch contains the intended commit and report what was committed, what was pushed and any relevant changes left uncommitted.

## Test Isolation and Data Safety

- All tests involving filesystem mutations must operate exclusively on isolated, disposable test fixtures.
- Never use the user's actual Wastebin, home directory contents, existing files or mounted data as destructive test targets.
- Wastebin tests must use an injected test backend or fully isolated Trash environment. Changing `XDG_DATA_HOME` alone must not be assumed to isolate mounted filesystem Wastebins.
- Tests involving GIO, GVfs, D-Bus or other system services must isolate or mock any backend capable of modifying live user data.
- Real filesystem and network mount acceptance tests are permitted using uniquely named disposable fixtures. Clean up only artefacts created by the test.
- Before running unfamiliar tests that perform destructive operations, verify that their targets and backends are safely isolated.
- If isolation cannot be guaranteed, do not run the affected test. Report the limitation instead.
- Immediately stop and report any unintended modification or deletion of user data.
- Any test constructing `FileManager`, `ConfigService`, or `FileOperationManager` must isolate `HOME`, `NYTHERA_{CONFIG,STATE,DATA}_HOME`, `XDG_{CONFIG,STATE,DATA,CACHE}_HOME`, set `GIO_USE_VFS=local`, and disable or privately isolate `DBUS_SESSION_BUS_ADDRESS` before instantiation; never fall back silently to the live session bus.
- Destructive Wastebin tests must inject an isolated `WastebinBackend` and run with fail-closed production Wastebin guards enabled; automated tests must never inspect or delete entries under a live mount's `.Trash-*` directory.
- Fixture roots must be created exclusively (e.g. `mkdtemp` or exclusive `create_directory`) and cleaned up by exact owned path only—never via prefix or wildcard sweeps over shared or mount parent directories.
- Ensure asynchronous workers and helper sessions drain cleanly before teardown, and avoid `std::exit()` or unguarded cleanup paths that skip fixture disposal or leave workers racing shutdown.
