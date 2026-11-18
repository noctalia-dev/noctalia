#!/usr/bin/env bash
set -euo pipefail

config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
rm -f -- "$config_home/luciole/colors.toml"
