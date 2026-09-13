#!/usr/bin/env bash
set -euo pipefail

# Reload the service-owned primary through systemd. Ghostty documents this as
# the supported way to signal that process, and it avoids SIGUSR2 while it is
# still installing its handler during startup.
primary_pid=""
if command -v systemctl >/dev/null 2>&1; then
    candidate_pid="$(systemctl --user show --property=MainPID --value app-com.mitchellh.ghostty.service 2>/dev/null || true)"
    if [[ "$candidate_pid" =~ ^[1-9][0-9]*$ ]] &&
        systemctl --user reload app-com.mitchellh.ghostty.service >/dev/null 2>&1; then
        primary_pid="$candidate_pid"
    fi
fi

# Directly launched single-instance primaries have no systemd unit. Their GTK
# reload action is safe, unlike SIGUSR2 during startup.
if [ -z "$primary_pid" ] && command -v gdbus >/dev/null 2>&1; then
    candidate_pid="$(
        {
            gdbus call --session \
                --dest org.freedesktop.DBus \
                --object-path /org/freedesktop/DBus \
                --method org.freedesktop.DBus.GetConnectionUnixProcessID \
                com.mitchellh.ghostty 2>/dev/null || true
        } | sed -nE 's/^\(uint32 ([0-9]+),\)$/\1/p'
    )"
    if [[ "$candidate_pid" =~ ^[1-9][0-9]*$ ]] &&
        gdbus call --session \
            --dest com.mitchellh.ghostty \
            --object-path /com/mitchellh/ghostty \
            --method org.gtk.Actions.Activate \
            reload-config '[]' '{}' >/dev/null 2>&1; then
        primary_pid="$candidate_pid"
    fi
fi

# Instances launched with gtk-single-instance=false own no D-Bus name, so
# reload their independent processes separately. Excluding the primary keeps
# its startup-safe reload path intact.
for pid in $(pgrep -x ghostty 2>/dev/null || true); do
    [ "$pid" = "$primary_pid" ] && continue
    kill -SIGUSR2 "$pid" 2>/dev/null || true
done
