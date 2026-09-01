---
title: Plugins
description: IPC commands for plugin event dispatch and plugin/source management.
sidebar:
  order: 4
---

import IpcCommand from '../../../../components/IpcCommand.astro';

Plugin IPC has two command families: dispatching events to plugin entries, and managing enabled plugins/sources.

## Send an Event

<IpcCommand command="plugin <author/plugin:entry> <target[:bar-name]> <event> [payload]" />

Dispatches an event to a plugin entry's `onIpc(event, payload)` handler.

Targets select which live entry instances receive the event:

- `focused` - the entry instance on the focused output
- a connector such as `DP-1`
- a bar-qualified target such as `focused:default` or `DP-1:top`
- `all` - every matching instance; required for singleton entries such as `[[service]]` and `[[panel]]`

Anything after `<event>` is passed through as `[payload]` unchanged.

Examples:

```sh
noctalia msg plugin noctalia/screen_recorder:service all toggle
noctalia msg plugin noctalia/screen_recorder:service all start focused
noctalia msg plugin noctalia/example:hello focused set "New label"
```

## Manage Plugins

<IpcCommand command="plugins <subcommand> ..." />

Use this family to list, enable, disable, update plugins, and manage plugin sources.

Common commands:

```sh
noctalia msg plugins list
noctalia msg plugins enable noctalia/screen_recorder
noctalia msg plugins disable noctalia/screen_recorder
noctalia msg plugins update official
```

Source management:

```sh
noctalia msg plugins source list
noctalia msg plugins source add my-repo git https://github.com/me/my-plugins
noctalia msg plugins source add my-dev path ~/dev/my-plugins
noctalia msg plugins source remove my-repo
```

See [Using Plugins](/noctalia/plugins/) for installing, enabling, and configuring plugins.
