# Nythera application identity

Nythera is an independent fork of [Noctalia](https://github.com/noctalia-dev/noctalia). The repository is now [Nythera-Dev/Nythera](https://github.com/Nythera-Dev/Nythera), with `main` and `dev` branches. Local Git remote names and URLs must be verified on each checkout rather than assumed. Original MIT licensing and third-party acknowledgements remain intact.

## Runtime and build

Build with `just build`; run `build-debug/nythera --daemon`, and use `nythera msg` for IPC. `just install debug` installs the executable, `share/nythera/assets`, `dev.nythera.Nythera*.desktop`, the Nythera icon and Bash/Fish/Zsh completions. Cross builds that cannot execute target programs must generate completions on a compatible host using `nythera completions <shell>`.

Application-owned C++ namespaces, macros, plugin global, layer namespaces, D-Bus helper names, IPC socket, instance lock and debug service use Nythera. The debug endpoint is `dev.nythera.Debug`; MPRIS helper API is `dev.nythera.Mpris`. Standard freedesktop/Wayland names are unchanged.

Default directories are `$XDG_CONFIG_HOME/nythera`, `$XDG_STATE_HOME/nythera`, `$XDG_DATA_HOME/nythera`, `$XDG_CACHE_HOME/nythera` (with standard HOME fallbacks). `NYTHERA_CONFIG_HOME`, `NYTHERA_STATE_HOME` and `NYTHERA_DATA_HOME` override the corresponding **base** directory; `/nythera` is appended, matching test/support-report layouts. Old `NOCTALIA_*` overrides are not runtime aliases.

## Deliberate data migration

Stop the old fork while unlocked before the final copy, so its last settings writes are preserved. Then run:

```sh
nythera config migrate-legacy --include-keyring
```

Default sources are `cyberpunk-shell/noctalia` beneath each normal XDG root. The migration never automatically imports the packaged `noctalia` profile. Select another source deliberately with `--source-config`, `--source-state`, `--source-data`, `--source-cache` (paths to the actual application directories, not their parents).

Existing destination files are preserved, original data is neither deleted nor modified, and missing trees are reported and skipped. Copies can be retried after partial failure. Destination symlinks are rejected; source symlinks are reported rather than followed into another installation. Recreate intended external asset links deliberately. Copied TOML is rewired for the selected profile paths, old fork build executable, application command prefix, and internal state section. Existing Nythera files are never rewritten by migration.

`--include-keyring` explicitly copies the legacy encrypted-state master key and credentials for calendar accounts named in the selected profile into Nythera's schema/application attributes. Existing Nythera keyring entries win. Originals are never erased; values are not logged. An unavailable/locked provider is reported as failure, allowing a retry. Both forks historically shared the same keyring namespace, so an item cannot be attributed to a particular executable retrospectively; the import is deliberately requested and restricted to known IDs. There is no permanent old-schema lookup fallback.

Encrypted on-disk stores retain their existing versioned magic/KDF context bytes (`NOCTALIAENC1`, `NOCTCLIP`, `NOCTCALE`, `NOCTNOTI`); these are legacy data-format contracts, not service identities. Changing them would make copied encrypted histories unreadable. Runtime keyring schema is `dev.nythera.Secret` with application `nythera`.

## External contracts retained

- Real upstream APIs under `api.noctalia.dev`: palettes/templates, plugin recommendations, location/geocoding, public IP, Google OAuth broker, and opt-in telemetry. A rebrand does not invent replacement services or alter user opt-ins.
- The recommendation voter-token prefix `noctalia-recommend-v1` is the existing upstream API identity contract; preserve it to keep the same voter identity after migration.
- Official/community plugins and templates remain hosted in `noctalia-dev` GitHub repositories. Their web catalogue, docs, translation links and upstream binary caches keep their real addresses.
- Upstream Luau plugins still publish a `noctalia` global. At source ingestion, Nythera translates that identifier to its `nythera` API before compilation, including required modules. It does not register a runtime `noctalia` alias. Comments, long strings, URLs and catalogue IDs are preserved. Literal host commands (`noctalia` or `noctalia ...`) and `dev.noctalia.*` helper identifiers are explicitly rewired too. Dynamically constructed shell commands outside these recognised forms require plugin-side updates. Bundled consumers and Nythera documentation use `nythera`.
- Firefox uses the third-party Pywalfox extension: native host `pywalfox`, extension ID `pywalfox@frewacom.org`, `pywalfox.json`, extension settings and message fields remain its external contract. The registered executable and Nythera-owned host description/runtime socket are renamed. A pre-existing foreign/Noctalia manifest is not silently overwritten; explicitly install Nythera's host when switching the extension to this shell.
- Standard services/interfaces (`org.freedesktop.*`, `org.mpris.MediaPlayer2`, etc.), system package/library names, and protocol definitions are unchanged.
- Upstream-only GitHub project and mirror workflows retain upstream repository guards and external destinations; these remain disabled on the personal fork. Nythera build/release targets and artifact names are renamed. The local Git remote configuration is not documented here; inspect it before integration.

See [CREDITS.md](CREDITS.md) and [LICENSE](LICENSE) for source and asset attribution. This rename reuses the existing artwork and glyphs; it does not claim new authorship or redesign the interface.

## Validation record — 6 November 2026

- Clean `build-rebrand` configured as debugoptimised; Nythera executable and staged `/usr/local` layout built successfully. Normal `build-debug` rebuilt with the same optimised/debug settings and is the daily runtime.
- Full 190-test suite: 187 initially passed; encryption-marker expectation and two upstream recommendation-protocol tests were corrected and passed on targeted reruns. Ten affected CLI, plugin, migration and security tests passed again after final integration fixes. GitHub workflow helper suite: 29 passed.
- Bash/Fish/Zsh completion syntax and both desktop files validated. Existing annotation desktop entry retains its informational category hint.
- Migration exercised missing sources, existing destination files, partial/repeated copies, symlink safety, XDG roots and source preservation. Live import copied 3,769 files and one keyring item, with no reported failures; the absent old data tree was skipped.
- Live config validation: valid with the same seven legacy warnings. Nythera started on four outputs, shut down gracefully, restarted from a relative executable path, answered `nythera msg status`, and registered renamed debug/MPRIS services. The notification server reports Nythera/Kangy1103.
- Fish switcher round trip Nythera → packaged Noctalia → Nythera passed. Final profile/runtime is Nythera; the old Noctalia IPC command reports no running instance. Existing visualizer plugin's schema warning was verified in the old log too; no new runtime error lines were observed.
- Repository identifier audit classified all surviving old names as attribution, real upstream URLs/services, external plugin/data-format contracts, or explicit migration code/tests. No old application-owned filenames remain. LICENSE, CREDITS.md, third_party and protocols were not modified; Local Git remotes were not verified as part of this historical validation.

Limits: Firefox native messaging was covered by automated tests; no local Pywalfox host is installed for an end-to-end browser check. Interactive authentication prompts and every third-party plugin action were not manually exercised. No visual redesign or browser screenshot comparison was performed.
