<p align="center">
  <img src="assets/Nythera%20Neon%20Nebula%20Emblem.png" alt="Nythera emblem" width="360">
</p>

# Nythera

Nythera is my personal fork of [Noctalia](https://github.com/noctalia-dev/noctalia), a native Wayland desktop shell written in C++.

I started this project because I wanted something a bit different from the usual desktop bars and panels. Noctalia already had a solid foundation, so rather than starting from scratch, I decided to build on what was there and see how far I could take it.

What started as experimenting with floating islands and screen wrapping has grown into something a fair bit bigger.

The idea is still the same, though: a flexible, customisable desktop shell with a darker sci-fi/fantasy aesthetic, more interesting layouts and a few features I haven't quite found elsewhere.

I'm not trying to replace everything Noctalia does. A lot of its existing functionality is exactly what I need. I just want more freedom over how it looks, behaves and fits together.

## A Note on Development

I'll get this out of the way early: **Nythera is largely vibe coded.** 🤣

I'm learning to code, but C++ is completely outside my wheelhouse. I couldn't write this project myself, so I'm using AI tools to help turn my ideas into something that actually works.

The ideas, design direction and decisions are mine, while the implementation is largely AI-generated. I'm testing things, breaking things, figuring out what works and probably giving the AI a headache in the process.

I'm not pretending to be a C++ developer. I'm just someone who enjoys messing around with Linux, has far too many ideas, and now has the tools to actually try building them.

That said, I'm still trying to do things properly. Testing, handling errors, avoiding unnecessary dependencies and keeping the code maintainable are important, even if I'm not the one writing most of it.

AI-generated code isn't automatically good code, after all.

## What's Different?

### Bars, Islands & Screen Wrap

One of the main reasons I started Nythera was wanting something more flexible than a traditional single desktop bar.

Nythera currently includes or experiments with:

- **Floating Islands** — Separate sections of the bar into independent panels rather than one continuous strip.
- **Dynamic Island** — A changing area for things like media playback, notifications and system events.
- **Screen Wrap** — Extends the bar around the display edges, creating a frame that connects with the rest of the interface.
- **Configurable Bar Layouts** — More freedom over positioning, margins, spacing and how bar sections behave.
- **Traditional Bars** — Still available. Not everything needs to be an island!

The focus now is refining how these elements work together, particularly how islands connect, how screen wrapping interacts with windows and how everything responds to different layouts.

Some of it works nicely. Some of it has been an absolute pain in the arse. 🤣

### Application Launcher

The launcher is getting its own Nythera identity rather than simply inheriting Noctalia's original appearance.

I've been working on a redesigned header, custom branding, subtler selection states, keyboard navigation feedback and more consistent styling throughout.

The aim isn't to completely reinvent an application launcher. It's to make something familiar feel like it actually belongs to Nythera.

### Window Inspector

Nythera includes a Window Inspector for viewing information about the currently selected window.

It provides window and workspace information, positioning details, supported window actions and a live preview.

It's one of those features I didn't originally plan, but once the idea came up, I couldn't resist giving it a shot.

### Notifications & Transfer Integration

I've also been extending the notification system beyond ordinary desktop notifications.

Nythera supports KDE-compatible file-job progress reporting, including persistent transfer notifications with progress information and controls where supported.

Active jobs can also be accessed through the Notifications Control Centre.

The intention is to make file operations feel properly integrated into the desktop rather than relying on a separate progress window for everything.

### Nythera Files

And then there's [Nythera Files](https://github.com/Nythera-Dev/Nythera-Files).

This started because I couldn't quite find a file manager that combined everything I liked about the existing options.

**COSMIC Files' visual structure, Dolphin's functionality, Thunar's practical touches, and Nythera's own styling.**

Rather than maintaining another file-manager fork, I decided to experiment with building a native one using Nythera's C++ rendering and UI infrastructure.

That experiment now has its own repository. Files is a standalone application, built independently of Nythera Shell, with shared rendering and UI components provided by [NytheraLib](https://github.com/Nythera-Dev/NytheraLib).

Current development includes:

- List and grid browsing, tabs, navigation and sorting.
- MIME-aware icons and configurable icon themes.
- Copy, move, rename, creation and deletion.
- Native conflict resolution and directory merging.
- Wastebin browsing.
- Device discovery and filesystem capacity information.
- Mount management and saved Places.
- File selection, drag-and-drop and adjustable view scaling.
- Background transfers with shell-integrated progress reporting.

It's still under active development, particularly around interactions, filesystem support and general usability.

I never intended to build an entire file manager when I started this fork, but here we are. 🤣

## Appearance & Theming

Nythera leans towards a darker, futuristic style with sci-fi and fantasy influences.

Dark surfaces, transparency, restrained lighting, crimson accents and slightly unusual geometry are generally what I'm going for.

That doesn't mean everyone should be forced into my particular colour scheme, though.

Where possible, styling should follow the existing theme system rather than hardcoded values. The intention is to keep Nythera configurable while giving its own components a consistent visual identity.

I've also been experimenting with custom branding, typography and a matching Discord theme.

## What I'm Working Towards

There's still plenty I'd like to do.

- **More Flexible Islands** — Greater freedom over positioning, connecting and merging bar sections.
- **Screen Wrap Refinements** — Better edge geometry, configurable spacing and more natural integration with windows.
- **Dynamic Island Improvements** — Making its content and interactions feel more like a proper dynamic island rather than a conventional popup.
- **Control Centre Redesign** — A more cohesive interface that shares design elements with the launcher and the rest of Nythera.
- **More Animation Options** — Greater control over motion and transitions throughout the shell.
- **Media Integration** — A more capable music player and media experience.
- **Nythera Files** — Continuing towards a practical, fully featured native file manager.
- **Further Customisation** — More flexibility without turning the settings into an incomprehensible mess.

These are things I'm exploring, not promises that every single one will make it in.

I have a habit of coming up with new ideas faster than I can finish the existing ones.

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

Some of these components have received substantial Nythera-specific changes, while others remain largely inherited from upstream.

## Compositor Support

I'm developing Nythera primarily on **Umbriel**, since that's what I currently use.

Noctalia supports several Wayland compositors, including Niri, Hyprland, Sway, Mango and Labwc. Much of that support is retained in Nythera.

However, most of my testing happens on Umbriel, so I can't guarantee every Nythera-specific feature behaves identically elsewhere.

## Building & Installation

Nythera is still experimental and doesn't currently have a proper packaged release.

See [BUILDING.md](BUILDING.md) for the current source-build instructions and dependencies.

The project is changing fairly regularly, so expect the occasional rough edge.

Nythera Shell now builds independently of [Nythera Files](https://github.com/Nythera-Dev/Nythera-Files). Both use the separately installed, dynamically linked [NytheraLib](https://github.com/Nythera-Dev/NytheraLib).

## Credits

Nythera wouldn't exist without [Noctalia](https://github.com/noctalia-dev/noctalia) and the work of its developers and contributors.

The original shell architecture, rendering infrastructure and much of the underlying functionality come from their project. Their work is what made this whole experiment possible in the first place.

Nythera is an independent fork and isn't affiliated with or endorsed by the Noctalia developers.

See [CREDITS.md](CREDITS.md) for additional acknowledgements and third-party dependencies.

## License

Nythera is licensed under the [MIT License](LICENSE), the same as Noctalia.

The original copyright and licence notices are retained.

---

Ultimately, this is still a personal project, and I'm having a lot of fun seeing where it goes.

I've got plenty more ideas, although at some point I should probably finish a few of the ones I've already started.

No promises. 🤣
