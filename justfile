set positional-arguments

mode := "debug"
build-dir := "build-" + mode
prefix := "/usr/local"
cpp-std := "c++23"

default:
    @just --list

configure m=mode install_prefix=prefix:
    #!/usr/bin/env bash
    set -euo pipefail
    args=(--buildtype={{ if m == "release" { "release" } else { "debug" } }} -Dcpp_std={{cpp-std}})
    [[ "{{m}}" == "release" ]] && args+=(-Db_lto=true)
    [[ "{{m}}" == "asan"    ]] && args+=(-Db_sanitize=address,undefined)
    if [[ -d "build-{{m}}" ]]; then
        meson setup "build-{{m}}" "${args[@]}" --prefix "{{install_prefix}}" --reconfigure
    else
        meson setup "build-{{m}}" "${args[@]}" --prefix "{{install_prefix}}"
    fi
    ln -sfn "build-{{m}}/compile_commands.json" compile_commands.json

build m=mode: (_ensure-configured m)
    meson compile -C build-{{m}}

_ensure-configured m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        just configure {{m}}
        exit 0
    fi
    current_cpp_std="$(meson configure "build-{{m}}" | awk '$1 == "cpp_std" { print $2; exit }')"
    if [[ "$current_cpp_std" != "{{cpp-std}}" ]]; then
        meson configure "build-{{m}}" -Dcpp_std={{cpp-std}}
    fi

run m=mode: (build m)
    ./build-{{m}}/noctalia

# Build (forcing tests on, even for release) and run the unit tests.
test m=mode *args: (_ensure-configured m)
    #!/usr/bin/env bash
    set -euo pipefail
    # Plain reconfigure first so build dirs predating the 'tests' option learn it,
    # then force it on (covers release, where it defaults off).
    meson setup "build-{{m}}" --reconfigure >/dev/null
    meson setup "build-{{m}}" -Dtests=enabled --reconfigure >/dev/null
    meson test -C build-{{m}} {{args}}

install m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -x "build-{{m}}/noctalia" ]]; then
        echo "error: build-{{m}}/noctalia is missing; run 'just build {{m}}' before installing" >&2
        exit 1
    fi
    meson install --no-rebuild -C build-{{m}}

uninstall m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        echo "error: build-{{m}} is missing or was not configured with the Ninja backend; nothing to uninstall" >&2
        exit 1
    fi
    ninja -C build-{{m}} uninstall

format:
    find src \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
    find src \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 grep -ZlP '\s+$' | xargs -0 -r sed -i 's/[[:space:]]*$//'

_clang_tidy m=mode *args:
    #!/usr/bin/env bash
    set -euo pipefail
    src_root="$(realpath src)"
    run-clang-tidy -quiet -p build-{{m}} -j "$(nproc)" -header-filter="^${src_root}/.*" {{args}} "^${src_root}/.*"

lint m=mode: (configure m)
    just _clang_tidy {{m}} '-warnings-as-errors=*'

fix m=mode: (configure m)
    just _clang_tidy {{m}} -fix
    just format

clean m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)
