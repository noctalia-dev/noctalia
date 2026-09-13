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

tmp_file="$(mktemp "$config_file.tmp.XXXXXX")"
trap 'rm -f "$tmp_file"' EXIT

awk '
    function add_files() {
        print "files = [\"noctalia.toml\"]"
        added = 1
    }

    # Offset of the first ch at or after start that is real array syntax: not
    # inside a quoted string and not inside a # comment. Strings and comments
    # end at a newline. Returns 0 when the buffer has no such character.
    # Entries are TOML basic strings, matching the rest of this script.
    function find_syntax(s, start, ch,   i, c, in_str, in_comment) {
        for (i = start; i <= length(s); i++) {
            c = substr(s, i, 1)
            if (c == "\n") { in_str = 0; in_comment = 0; continue }
            if (in_comment) continue
            if (in_str) {
                if (c == "\\") { i++; continue }
                if (c == "\"") in_str = 0
                continue
            }
            if (c == "\"") { in_str = 1; continue }
            if (c == "#") { in_comment = 1; continue }
            if (c == ch) return i
        }
        return 0
    }

    function has_array_close(s) {
        return find_syntax(s, 1, "]") > 0
    }

    # Rebuild a complete "files = [ ... ]" statement (buf may span lines),
    # dropping any existing noctalia.toml entry and appending it last so it
    # overrides earlier includes. Handles single-line and multi-line arrays.
    function build(buf,   open, endp, head, inner, tail, test, multiline, indent) {
        open = find_syntax(buf, 1, "[")
        endp = find_syntax(buf, open + 1, "]")
        if (open == 0 || endp == 0 || endp < open) {
            print "error: include.files must be an array" > "/dev/stderr"
            exit 2
        }
        head  = substr(buf, 1, open)
        inner = substr(buf, open + 1, endp - open - 1)
        tail  = substr(buf, endp)

        gsub(/"noctalia\.toml"[[:space:]]*,[[:space:]]*/, "", inner)
        gsub(/,[[:space:]]*"noctalia\.toml"/, "", inner)
        gsub(/"noctalia\.toml"/, "", inner)

        test = inner
        gsub(/[[:space:]]/, "", test)
        multiline = (index(inner, "\n") > 0)

        if (multiline) {
            indent = "  "
            if (match(inner, /\n[ \t]*"/))
                indent = substr(inner, RSTART + 1, RLENGTH - 2)
            if (test == "")
                return head "\n" indent "\"noctalia.toml\",\n" tail
            sub(/[[:space:]]+$/, "", inner)
            if (inner !~ /,$/)
                inner = inner ","
            return head inner "\n" indent "\"noctalia.toml\",\n" tail
        }

        if (test == "")
            return head "\"noctalia.toml\"" tail
        sub(/[[:space:]]+$/, "", inner)
        return head inner ", \"noctalia.toml\"" tail
    }

    collecting {
        buf = buf "\n" $0
        if (has_array_close($0)) {
            print build(buf)
            collecting = 0
            added = 1
        }
        next
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
        if (find_syntax($0, 1, "[") == 0) {
            print "error: include.files must be an array" > "/dev/stderr"
            exit 2
        }
        buf = $0
        if (has_array_close($0)) {
            print build(buf)
            added = 1
        } else {
            collecting = 1
        }
        next
    }

    { print }

    END {
        if (collecting)
            print buf
        if (in_include && !saw_files)
            add_files()
        if (!saw_include) {
            print ""
            print "[include]"
            add_files()
        }
    }
' "$config_file" >"$tmp_file"

if ! cmp -s "$config_file" "$tmp_file"; then
    cp "$tmp_file" "$config_file"
fi
