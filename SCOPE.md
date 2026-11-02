# Scope

Noctalia is a native Wayland desktop shell built directly on Wayland and OpenGL ES, with no Qt or GTK. It is a desktop shell, not a full desktop environment: it provides the visual and service layer around your compositor, and leaves window management, system services, and standalone applications to the tools built for them. It is built by the people who use it daily. It already covers a lot of ground, and the fastest way to make it worse is to keep growing it in every direction at once. This file records what the project takes on and what it declines, so a request gets a predictable answer instead of a case-by-case one.

Read the project [ethos](https://noctalia.dev/ethos) for the values behind these lines, [CONTRIBUTING.md](CONTRIBUTING.md) for the technical principles they follow from, and [ARCHITECTURE.md](ARCHITECTURE.md) for the invariants reviewers enforce.

## In scope

- Bugs, crashes, regressions, and protocol misbehavior in anything Noctalia already does.
- Refinement of the surfaces that already exist: bars and their widgets, dock, launcher, control center, notifications, wallpaper, lock screen, idle behavior, session actions, clipboard history, OSDs, tray, and desktop widgets.
- Configuration keys that expose behavior Noctalia already implements, plus IPC actions for state it already tracks.
- Wayland protocol support the shell needs, and compositor integration through `CompositorPlatform` for compositors that expose the required protocols or IPC.
- Correct behavior on scaled outputs, multiple monitors, hotplug, and across the supported compositors.
- Theming, palettes, and templates that keep the desktop visually consistent.
- Plugin API surface that lets features outside the core live as plugins.
- Documentation, translations, packaging across major distros, and test coverage for all of the above.

## Out of scope

- **Window management and compositor work.** Tiling, window rules, monitor arrangement, and compositor configuration belong to the compositor, for example [Umbriel](https://github.com/noctalia-dev/umbriel). Noctalia's job is to build on what a compositor exposes, not to become one.
- **Managing your compositor settings or dotfiles.** Core Noctalia is non-invasive. Compositor-specific controls are opt-in plugins, not core shell behavior.
- **Replacing desktop applications and system services.** File management, removable-drive mounting, printer management, and screen mirroring or casting belong to dedicated tools. Login and display greeting live in [Noctalia Greeter](https://github.com/noctalia-dev/noctalia-greeter).
- **Features that are useful to some users but not essential to the shell.** Extra bar widgets, launcher providers, panels, hardware-specific controls, and third-party service integrations belong in plugins.
- **Toolkits and other rendering stacks.** No Qt, no GTK, no web views. UI is built from `src/ui/controls/`.
- **Configuration languages other than TOML**, and compatibility aliases, migration readers, or silent fallbacks for renamed keys, IPC names, or paths.
- **Patching vendored code** under `third_party/`.
- **A plugin permission or sandbox model.** Plugins are trusted Luau code, equivalent to a user-owned script.
- **Feature parity with another shell as the motivation.** "Project X has this" describes Project X, not a problem you have.
- **Per-compositor or per-application special cases** outside `CompositorPlatform`, or ones a plugin or script can already handle.
- **Anything already reachable** through configuration, IPC, custom script-backed widgets, or a plugin.

"Out of scope" is not a verdict on the idea. It means the work does not belong in this project, at this stage, with this maintenance budget.

## How a request is judged

1. **Does it need the shell core?** If the compositor, a dedicated application, a script, or a plugin can do it, that is where it belongs.
2. **Does it extend something that exists, or open a new front?** Sharpening current behavior beats adding surface.
3. **Does it fit the existing stack?** A feature that needs a new dependency, toolkit, or machinery of its own costs far more than its diff.
4. **Who maintains it in a year?** Every option, key, widget, and code path is carried forward and re-tested across every supported compositor and distro.

A request that survives all four is worth an issue. One that does not is still worth a conversation, just not a tracking issue.

## Where to raise things

- **Bug or regression:** open a bug report. These are always welcome, including small ones.
- **Idea, question, or "would you consider":** ask on [Discord](https://discord.noctalia.dev) first. It is faster than an issue and it survives being wrong.
- **Large or invasive pull request:** agree on the change before writing it. An unsolicited rewrite can be declined on scope alone, and nobody enjoys that outcome.

This file is not a promise that the answer stays no forever. Scope widens deliberately, when the foundation is stable enough to carry the addition, and it is recorded here when it does.
