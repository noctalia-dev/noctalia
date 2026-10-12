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
    printf '[include.optional]\n%s\n' "$include_line" >"$config_file"
    exit 0
fi

tmp_file="$(mktemp "$config_file.tmp.XXXXXX")"
trap 'rm -f "$tmp_file"' EXIT

awk '
    function add_optional_files() {
        print "files = [\"noctalia.toml\"]"
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
    function build_optional(buf,   open, endp, head, inner, tail, test, multiline, indent) {
        open = find_syntax(buf, 1, "[")
        endp = find_syntax(buf, open + 1, "]")
        if (open == 0 || endp == 0 || endp < open) {
            print "error: include.optional.files must be an array" > "/dev/stderr"
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

    # Remove the generated include from the mandatory section written by older
    # Noctalia versions while preserving every user-owned include.
    function build_required(buf,   open, endp, head, inner, tail) {
        if (index(buf, "\"noctalia.toml\"") == 0)
            return buf

        open = find_syntax(buf, 1, "[")
        endp = find_syntax(buf, open + 1, "]")
        if (open == 0 || endp == 0 || endp < open)
            return buf

        head  = substr(buf, 1, open)
        inner = substr(buf, open + 1, endp - open - 1)
        tail  = substr(buf, endp)

        gsub(/"noctalia\.toml"[[:space:]]*,[[:space:]]*/, "", inner)
        gsub(/,[[:space:]]*"noctalia\.toml"/, "", inner)
        gsub(/"noctalia\.toml"/, "", inner)
        return head inner tail
    }

    collecting {
        buf = buf "\n" $0
        if (has_array_close($0)) {
            if (collecting_optional)
                print build_optional(buf)
            else
                print build_required(buf)
            collecting = 0
            collecting_optional = 0
        }
        next
    }

    /^[[:space:]]*\[/ {
        if (in_optional && !saw_optional_files)
            add_optional_files()
        in_optional = 0
        in_required = 0

        if ($0 ~ /^[[:space:]]*\[include\.optional\][[:space:]]*(#.*)?$/) {
            saw_optional = 1
            in_optional = 1
        } else if ($0 ~ /^[[:space:]]*\[include\][[:space:]]*(#.*)?$/) {
            in_required = 1
        }

        print
        next
    }

    in_optional && /^[[:space:]]*files[[:space:]]*=/ {
        saw_optional_files = 1
        if (find_syntax($0, 1, "[") == 0) {
            print "error: include.optional.files must be an array" > "/dev/stderr"
            exit 2
        }
        buf = $0
        if (has_array_close($0)) {
            print build_optional(buf)
        } else {
            collecting = 1
            collecting_optional = 1
        }
        next
    }

    in_required && /^[[:space:]]*files[[:space:]]*=/ {
        if (find_syntax($0, 1, "[") == 0) {
            print
            next
        }
        buf = $0
        if (has_array_close($0)) {
            print build_required(buf)
        } else {
            collecting = 1
        }
        next
    }

    { print }

    END {
        if (collecting)
            print buf
        if (in_optional && !saw_optional_files)
            add_optional_files()
        if (!saw_optional) {
            print ""
            print "[include.optional]"
            add_optional_files()
        }
    }
' "$config_file" >"$tmp_file"

if ! cmp -s "$config_file" "$tmp_file"; then
    cp "$tmp_file" "$config_file"
fi
