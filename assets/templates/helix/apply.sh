#!/usr/bin/env bash
set -euo pipefail

# Reload config in all running Helix instances.
# https://docs.helix-editor.com/configuration.html?highlight=pkill%20-USR1%20hx#configuration
pkill -USR1 hx >/dev/null 2>&1 || true
