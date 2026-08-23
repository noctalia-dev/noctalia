# Noctalia

Noctalia is a native Wayland desktop shell for people who want a polished, configurable Linux desktop without stitching
together a separate bar, launcher, notification daemon, lock screen, wallpaper tool, and settings UI.

It provides the shell layer around your compositor: bars, widgets, dock, launcher, control center, notifications,
wallpaper, lock screen, session actions, clipboard history, OSDs, tray integration, and desktop widgets. The project is
built directly on Wayland and OpenGL ES with no Qt or GTK dependency, so the UI, rendering, configuration, and IPC model
are designed as one cohesive shell instead of a collection of unrelated panels and scripts.

> [!IMPORTANT]
> Noctalia v5 is currently in Beta. While the core features and architecture are stabilizing, you may still encounter occasional configuration or behavior adjustments as we prepare for the final release.

<p><br/></p>

<p align="center">
  <img src="https://assets.noctalia.dev/noctalia-logo.svg?v=2" alt="Noctalia Logo" style="width: 192px" />
</p>

<p align="center">
  <a href="https://docs.noctalia.dev/noctalia/getting-started/installation/">
    <img
      src="https://img.shields.io/badge/Install_Noctalia-FFF59B?style=for-the-badge&labelColor=FFF59B"
      alt="Install Noctalia"
      style="height: 50px"
    />
  </a>
</p>

<p><br/></p>

<p align="center">
  <a href="https://github.com/noctalia-dev/noctalia/commits">
    <img src="https://img.shields.io/github/last-commit/noctalia-dev/noctalia?style=for-the-badge&labelColor=FFF59B&color=FFF59B&logo=git&logoColor=070722&label=commit" alt="Last commit" />
  </a>
  <a href="https://github.com/noctalia-dev/noctalia/stargazers">
    <img src="https://img.shields.io/github/stars/noctalia-dev/noctalia?style=for-the-badge&labelColor=FFF59B&color=FFF59B&logo=github&logoColor=070722" alt="GitHub stars" />
  </a>
  <a href="https://docs.noctalia.dev">
    <img src="https://img.shields.io/badge/docs-FFF59B?style=for-the-badge&logo=gitbook&logoColor=070722&labelColor=FFF59B" alt="Documentation" />
  </a>
  <a href="https://discord.noctalia.dev">
    <img src="https://img.shields.io/badge/discord-FFF59B?style=for-the-badge&labelColor=FFF59B&logo=discord&logoColor=070722" alt="Discord" />
  </a>
</p>

## Why Noctalia?

Most Wayland setups leave the desktop shell to a stack of small tools: one bar, another launcher, another notification
daemon, a lock screen, a wallpaper daemon, scripts for session actions, and separate config formats for each piece. That
can be flexible, but it also makes a complete desktop feel fragile and hard to keep visually consistent.

Noctalia solves that by providing one configurable shell layer that owns the common desktop surfaces and services while
still fitting into compositor-driven Wayland workflows. It is meant for users who want the control of a custom desktop
environment with fewer moving parts and a consistent UI.

To understand the values and philosophy guiding the project, read our [ethos](https://noctalia.dev/ethos).

## What It Includes

- Multi-monitor bars with configurable widgets, taskbar, workspaces, system tray, media, network, battery, brightness,
  weather, clipboard, and custom script-backed widgets.
- Dock, launcher, control center, notification toasts/history, wallpaper picker, OSD overlays, lock screen, session
  panel, and desktop widgets.
- TOML configuration with hot reload, GUI-managed overrides, theme/palette support, template application, and IPC for
  runtime control.
- Direct Wayland integration for layer-shell, session lock, idle behavior, clipboard, foreign toplevels, workspaces,
  fractional scaling, and compositor-specific workspace backends where needed.

## Wayland Compositor Support

Noctalia supports Wayland compositors that provide the layer-shell protocols it needs for shell surfaces. Workspace
integration works through compositor-native backends where needed, or through `ext-workspace-v1` on compositors that
implement it.

Current compositor integrations include Niri, Hyprland, Sway, Scroll, Mango, Labwc, Triad, dwl, and other compatible
Wayland compositors. Other compositors may run Noctalia but can have reduced workspace, window, output, or
session-action integration depending on the protocols and IPC they expose.

## Scope

Noctalia is a desktop shell, not a full desktop environment. It provides the visual and service layer around your
Wayland compositor: bars, panels, launcher, notifications, dock, lock screen, idle behavior, OSDs, theming, wallpapers,
desktop widgets, and multi-monitor shell surfaces.

Window management, tiling, file management, removable-drive mounting, printers management and screen mirroring/casting
belong to the compositor, dedicated desktop applications, or system services.

Display/login greeter support lives in the separate [Noctalia Greeter](https://github.com/noctalia-dev/noctalia-greeter)
project. Noctalia may integrate with those pieces when useful, but it does not replace them.

The plugin system is available for user-installed extensions. Features that are useful to some users but not essential
to the core shell can live there: extra bar widgets, launcher providers, desktop widgets, panels, shortcuts, background
services, compositor-specific extras, hardware-specific controls, and third-party service integrations.

## Build from source

Source dependencies, distro-specific package commands, build modes, and install layouts live in
[BUILDING.md](BUILDING.md).

## Configuration

A ready-to-use starting config with all defaults is at [example.toml](example.toml). The full configuration reference
lives in the [documentation site](https://docs.noctalia.dev/noctalia/).

## Contributing

Developer notes, architecture overview, code style, project layout, and debugging commands live in
[CONTRIBUTING.md](CONTRIBUTING.md).

Bug reports, fixes, documentation updates, themes, and configuration examples are welcome. For general help and design
discussion, join the community on [Discord](https://discord.noctalia.dev).

## Credits

Thank you to the [contributors](https://github.com/noctalia-dev/noctalia/graphs/contributors) and community
members who test Noctalia, report issues, share configurations, and help shape the project.

## Donations

Donations are appreciated but completely optional.

<p>
  <a href="https://www.buymeacoffee.com/noctalia">
    <img src="https://img.shields.io/badge/Buy_Me_a_Coffee-FFF59B?style=for-the-badge&logo=buymeacoffee&logoColor=070722&labelColor=FFF59B" alt="Buy Me a Coffee">
  </a>
  <a href="https://ko-fi.com/noctaliadev">
    <img src="https://img.shields.io/badge/Ko--fi-FFF59B?style=for-the-badge&logo=kofi&logoColor=070722&labelColor=FFF59B" alt="Ko-fi">
  </a>
</p>

## License

MIT License. See [LICENSE](LICENSE) for details.

## Packaging

Distro packaging notes (description, deps, install layout, Meson options) live in
[PACKAGING.md](PACKAGING.md).

## Star History

<p align="center">
  <a href="https://github.com/noctalia-dev/noctalia/stargazers">
    <img src="https://api.noctalia.dev/stars" alt="Star History" />
  </a>
</p>
