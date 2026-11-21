# Scope

Nythera is an experimental personal fork of [Noctalia](https://github.com/noctalia-dev/noctalia). It's a native Wayland desktop shell, not a compositor or a full desktop environment.

The goal is to keep the functionality of Nythera while experimenting with how the shell looks, feels and behaves. I'm mainly developing and testing it on Umbriel.

## In scope

- Improving and maintaining inherited desktop shell functionality: bars, launcher, dock, notifications, lock screen, widgets and settings.
- Dynamic bars, floating islands and contextual interface elements.
- Screen-wrap geometry and visual integration between bars, islands and screen edges.
- Freeform island placement, snapping and connected layouts as future development goals.
- Configuration and compositor integration needed to support these features.
- Fixing bugs and regressions introduced by Nythera changes.

## Out of scope

- Replacing the Wayland compositor or taking over general window management.
- Rebuilding applications and system services already handled by dedicated tools.
- Claiming tested support for every compositor or distribution supported by upstream Noctalia.
- Promising particular features, release schedules or long-term maintenance while the project is experimental.

## How changes are approached

This is a personal, heavily AI-assisted project. Changes should preserve working functionality where practical, be tested against the current code, and avoid unnecessary rewrites of stable upstream systems. Planned features in the README are goals, not implemented guarantees.

## Issues and contributions

Use [Nythera's repository](https://github.com/Nythera-Dev/Nythera/issues) for fork-specific issues. Please don't direct Nythera bugs to Noctalia's maintainers unless the issue is independently reproducible upstream.
