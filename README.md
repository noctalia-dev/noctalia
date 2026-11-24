<p align="center">
  <img src="assets/Nythera%20Neon%20Nebula%20Emblem.png" alt="Nythera emblem" width="360">
</p>

# Nythera

Nythera is my personal fork of [Noctalia](https://github.com/noctalia-dev/noctalia), a native Wayland desktop shell written in C++.

Noctalia already had a solid foundation, but I wanted more freedom over how my desktop looks, behaves and fits together. What started with floating islands and screen wrapping has grown into a flexible, customisable shell with a darker sci-fi/fantasy identity.

I'm not trying to replace everything Noctalia does. I'm building on the parts I like and experimenting with the things I wish it had.

## A Note on Development

I'll get this out of the way early: **Nythera is largely vibe coded** 🤣

I'm learning to code, but C++ is outside my wheelhouse. The ideas, design direction and decisions are mine; much of the implementation is AI-generated.

I'm testing things, breaking things and figuring out what works as I go. That doesn't mean maintainability, error handling or proper testing get a free pass. AI-generated code isn't automatically good code, after all.

I'm just someone who enjoys messing around with Linux and now has the tools to build some of the ideas that used to stay in my head.

## What's Different?

### Bars, Islands & Screen Wrap

Nythera currently includes or experiments with:

- **Floating Islands** — Separate sections of the bar into independent panels rather than one continuous strip.
- **Dynamic Island** — A changing area for things like media playback, notifications and system events.
- **Screen Wrap** — Extends the bar around the display edges, creating a frame that connects with the rest of the interface.
- **Configurable Bar Layouts** — More freedom over positioning, margins, spacing and how bar sections behave.
- **Traditional Bars** — Still available. Not everything needs to be an island!

I'm still refining how these elements work together. Some of it has been an absolute pain in the arse 🤣

### Application Launcher

The launcher has a redesigned header, custom branding, subtler selection states and clearer keyboard navigation feedback.

The aim isn't to reinvent launching apps, just to make it feel like part of Nythera.

### Window Inspector

The Window Inspector shows window and workspace information, positioning details, supported actions and a live preview.

I didn't originally plan it, but once the idea came up, I couldn't resist.

### Notifications & Transfer Integration

Nythera supports KDE-compatible file-job progress reporting, with persistent transfer notifications, progress details and controls where supported. Active jobs are also accessible through the Notifications Control Centre.

The goal is to make file operations feel integrated into the desktop.

### Nythera Files

And then there's [Nythera Files](https://github.com/Nythera-Dev/Nythera-Files).

I wanted COSMIC Files' visual structure, Dolphin's functionality, Thunar's practical touches and Nythera's styling in one place. Rather than fork another file manager, I ended up building my own.

It's now a standalone application with its own repository, using [NytheraLib](https://github.com/Nythera-Dev/NytheraLib) for shared rendering and UI infrastructure. It already supports tabs, list and grid views, file operations, Places, mounts and background transfers.

I never intended to build an entire file manager when I started this fork, but here we are 🤣

## Appearance & Theming

Nythera leans into dark surfaces, transparency, restrained lighting, crimson accents and slightly unusual geometry, with a sci-fi/fantasy influence.

That doesn't mean everyone should be stuck with my colour scheme. Styling should use the theme system wherever possible so Nythera stays customisable.

I've also been experimenting with custom branding, typography and a matching Discord theme.

## What I'm Working Towards



- **More Flexible Islands** — Greater freedom over positioning, connecting and merging bar sections.
- **Screen Wrap Refinements** — Better edge geometry, configurable spacing and more natural integration with windows.
- **Dynamic Island Improvements** — Making its content and interactions feel more like a proper dynamic island rather than a conventional popup.
- **Control Centre Redesign** — A more cohesive interface that shares design elements with the launcher and the rest of Nythera.
- **More Animation Options** — Greater control over motion and transitions throughout the shell.
- **Media Integration** — A more capable music player and media experience.
- **Further Customisation** — More flexibility without turning the settings into an incomprehensible mess.

These are ideas, not promises. I tend to come up with them faster than I finish them.

## Existing Noctalia Features

Since Nythera is based on Noctalia, it retains much of the functionality provided by the original project, including:

- Multi-monitor bars, widgets, taskbar and workspaces.
- Application launcher and dock.
- Notifications, control centre and on-screen displays.
- Media, audio, network and brightness controls.
- Wallpaper management and lock screen.
- Clipboard history and desktop widgets.
- TOML configuration, hot reload and graphical settings.
- Native Wayland integration and IPC controls.

Some are heavily customised; others remain close to upstream.

## Compositor Support

I'm developing and testing Nythera primarily on **Umbriel**.

Much of Noctalia's support for Niri, Hyprland, Sway, Mango and Labwc remains, but I can't guarantee every Nythera-specific feature behaves identically on those compositors.

## Building & Installation

Nythera is still experimental, without a stable packaged release. See [BUILDING.md](BUILDING.md) for source-build instructions and dependencies.

Shell builds independently of [Nythera Files](https://github.com/Nythera-Dev/Nythera-Files). Both use the independently installed, dynamically linked [NytheraLib](https://github.com/Nythera-Dev/NytheraLib).

## Credits

Nythera wouldn't exist without [Noctalia](https://github.com/noctalia-dev/noctalia) and its developers and contributors. The original shell architecture, rendering infrastructure and much of its functionality come from their work.

Nythera is an independent fork, not affiliated with or endorsed by the Noctalia developers.

See [CREDITS.md](CREDITS.md) for additional acknowledgements and third-party dependencies.

## License

Nythera is licensed under the [MIT License](LICENSE), the same as Noctalia.

The original copyright and licence notices are retained.

---

It's still a personal project, and I'm having fun seeing where it goes.

At some point I should probably finish a few ideas before coming up with more. No promises 🤣
