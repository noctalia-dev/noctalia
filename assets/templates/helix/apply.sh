#!/usr/bin/env bash
set -euo pipefail

# Reload config in all running Helix instances. Match names exactly: SIGUSR1 kills
# processes that do not handle it. Some distros (e.g. Arch) install the binary as helix.
# https://docs.helix-editor.com/configuration.html#configuration
pkill -USR1 -x 'hx|helix' || true
