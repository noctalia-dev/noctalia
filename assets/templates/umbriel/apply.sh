#!/usr/bin/env bash
set -euo pipefail

resolve_config_home() {
    if [ -n "${XDG_CONFIG_HOME:-}" ]; then
        printf '%s\n' "$XDG_CONFIG_HOME"
        return
    fi

    if [ -z "${HOME:-}" ]; then
        echo "error: HOME or XDG_CONFIG_HOME must be set" >&2
        exit 1
    fi

    printf '%s/.config\n' "$HOME"
}

config_dir="$(resolve_config_home)/umbriel"
config_file="$config_dir/config.toml"
include_line='files = ["noctalia.toml"]'

mkdir -p "$config_dir"

if [ ! -f "$config_file" ]; then
    printf '[include]\n%s\n' "$include_line" >"$config_file"
    exit 0
fi

if awk '
    /^[[:space:]]*\[include\][[:space:]]*(#.*)?$/ { in_include = 1; next }
    in_include && /^[[:space:]]*\[/ { exit found ? 0 : 1 }
    in_include && index($0, "noctalia.toml") != 0 { found = 1; exit 0 }
    END { exit found ? 0 : 1 }
' "$config_file"; then
    exit 0
fi

tmp_file="$(mktemp "$config_file.tmp.XXXXXX")"
trap 'rm -f "$tmp_file"' EXIT

awk '
    function add_files() {
        print "files = [\"noctalia.toml\"]"
        added = 1
    }

    /^[[:space:]]*\[include\][[:space:]]*(#.*)?$/ {
        saw_include = 1
        in_include = 1
        print
        next
    }

    in_include && /^[[:space:]]*\[/ {
        if (!saw_files)
            add_files()
        in_include = 0
    }

    in_include && /^[[:space:]]*files[[:space:]]*=/ {
        saw_files = 1
        if (index($0, "[") == 0) {
            print "error: include.files must be an array" > "/dev/stderr"
            exit 2
        }
        if ($0 ~ /\[[[:space:]]*\]/)
            sub(/\[[[:space:]]*\]/, "[\"noctalia.toml\"]")
        else
            sub(/\[/, "[\"noctalia.toml\", ")
        added = 1
    }

    { print }

    END {
        if (in_include && !saw_files)
            add_files()
        if (!saw_include) {
            print ""
            print "[include]"
            add_files()
        }
    }
' "$config_file" >"$tmp_file"

cp "$tmp_file" "$config_file"
