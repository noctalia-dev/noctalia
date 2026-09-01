#!/usr/bin/env bash
set -euo pipefail

# Reload a running Ghostty's configuration.
#
# SIGUSR2 keeps its default disposition (terminate) until Ghostty installs its handler
# well into GTK startup, so signalling the single-instance primary while it is starting
# (its systemd user service at login) kills it. That primary owns the com.mitchellh.ghostty
# bus name and exposes the same reload as a GTK action, which can never terminate it:
# activating an action the app has not registered yet is a silent no-op.
#
# Instances launched with gtk-single-instance=false own no bus name and only take the signal.
if command -v gdbus >/dev/null 2>&1 &&
    gdbus call --session \
        --dest com.mitchellh.ghostty \
        --object-path /com/mitchellh/ghostty \
        --method org.gtk.Actions.Activate \
        reload-config '[]' '{}' >/dev/null 2>&1; then
    exit 0
fi

pkill -SIGUSR2 -x ghostty || true
