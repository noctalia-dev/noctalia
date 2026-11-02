Luau ![CI](https://github.com/luau-lang/luau/actions/workflows/build.yml/badge.svg) [![codecov](https://codecov.io/gh/luau-lang/luau/branch/master/graph/badge.svg)](https://codecov.io/gh/luau-lang/luau)
====

Luau (lowercase u, /ˈlu.aʊ/) is a fast, small, safe, gradually typed embeddable scripting language derived from [Lua](https://lua.org).

It is designed to be backwards compatible with Lua 5.1, as well as incorporating [some features](https://luau.org/compatibility) from future Lua releases, but also expands the feature set (most notably with type annotations and a state-of-the-art type inference system). Luau is largely implemented from scratch, with the language runtime being a very heavily modified version of Lua 5.1 runtime, with completely rewritten interpreter and other [performance innovations](https://luau.org/performance). The runtime mostly preserves Lua 5.1 API, so existing bindings should be more or less compatible with a few caveats.

Luau is used by Roblox game developers to write game code, and by Roblox engineers to implement large parts of the user-facing application code as well as portions of the editor (Roblox Studio) as plugins. Roblox chose to open-source Luau to foster collaboration within the Roblox community as well as to allow other companies and communities to benefit from the ongoing language and runtime innovation. More recently, Luau has seen adoption in games like Alan Wake 2, Farming Simulator 2025, Second Life, and Warframe.

This repository hosts source code for the language implementation and associated tooling. Documentation for the language is available at https://luau.org/ and accepts contributions via [site repository](https://github.com/luau-lang/site); the language is evolved through RFCs that are located in [rfcs repository](https://github.com/luau-lang/rfcs).

# Usage

Luau is an embeddable programming language, but it also comes with two command-line tools by default, `luau` and `luau-analyze`.

`luau` is a command-line REPL and can also run input files. Note that REPL runs in a sandboxed environment and as such doesn't have access to the underlying file system except for ability to `require` modules.

`luau-analyze` is a command-line type checker and linter; given a set of input files, it produces errors/warnings according to the file configuration, which can be customized by using `--!` comments in the files or [`.luaurc`](https://rfcs.luau.org/config-luaurc) files. For details, please refer to our [type checking](https://luau.org/typecheck) and [linting](https://luau.org/lint) documentation. Our community maintains a language server frontend for `luau-analyze` called [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) for use with text editors.

# Installation

You can install and run Luau by downloading the compiled binaries from [a recent release](https://github.com/luau-lang/luau/releases); note that `luau` and `luau-analyze` binaries from the archives will need to be added to PATH or copied to a directory like `/usr/local/bin` on Linux/macOS.

Alternatively, you can use one of the packaged distributions (note that these are not maintained by Luau development team):

- macOS: [Install Homebrew](https://docs.brew.sh/Installation) and run `brew install luau`
- Arch Linux: Luau has been added to the official Arch Linux packages repository under the extras repository (see [``luau``](https://archlinux.org/packages/extra/x86_64/luau/)), simply install using ``pacman``: ``pacman -Syu luau``
- Alpine Linux: [Enable community repositories](https://wiki.alpinelinux.org/w/index.php?title=Enable_Community_Repository) and run `apk add luau`
- Gentoo Linux: Luau is [officially packaged by Gentoo](https://packages.gentoo.org/packages/dev-lang/luau) and can be installed using `emerge dev-lang/luau`. You may have to unmask the package first before installing it (which can be done by including the `--autounmask=y` option in the `emerge` command).

After installing, you will want to validate the installation was successful by running the test case [here](https://luau.org/getting-started).

## Building

On all platforms, you can use CMake to run the following commands to build Luau binaries from source:

```sh
mkdir cmake && cd cmake
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --target Luau.Repl.CLI --config RelWithDebInfo
cmake --build . --target Luau.Analyze.CLI --config RelWithDebInfo
```

Alternatively, on Linux and macOS, you can also use `make`:

```sh
make config=release luau luau-analyze
```

To integrate Luau into your CMake application projects as a library, at the minimum, you'll need to depend on `Luau.Compiler` and `Luau.VM` projects. From there you need to create a new Luau state (using Lua 5.x API such as `lua_newstate`), compile source to bytecode and load it into the VM like this:

```cpp
// needs lua.h and luacode.h
size_t bytecodeSize = 0;
char* bytecode = luau_compile(source, strlen(source), NULL, &bytecodeSize);
int result = luau_load(L, chunkname, bytecode, bytecodeSize, 0);
free(bytecode);

if (result == 0)
    return 1; /* return chunk main function */
```

For more details about the use of the host API, see the [C API documentation](https://luau.org/api).
Much of it is similar to the [Lua 5.x API](https://www.lua.org/manual/5.1/manual.html#3), but Luau has a few deviations, such as the need to compile source separately (which is important to be able to deploy VM without a compiler), and the lack of `__gc` support (use `lua_newuserdatadtor` instead).

To gain advantage of many performance improvements, it's highly recommended to use the `safeenv` feature, which sandboxes individual scripts' global tables from each other, and protects builtin libraries from monkey-patching. For this to work, you must call `luaL_sandbox` on the global state and `luaL_sandboxthread` for each new script's execution thread.

# Testing

Luau has an internal test suite; in CMake builds, it is split into two targets, `Luau.UnitTest` (for the bytecode compiler and type checker/linter tests) and `Luau.Conformance` (for the VM tests). The unit tests are written in C++, whereas the conformance tests are largely written in Luau (see `tests/conformance`).

Makefile builds combine both into a single target that can be run via `make test`.

# Dependencies

Luau uses C++ as its implementation language. The runtime requires C++11, while the compiler and analysis components require C++17. It should build without issues using Microsoft Visual Studio 2017 or later, or gcc-7 or clang-7 or later.

Other than the STL/CRT, Luau library components don't have external dependencies. The test suite depends on the [doctest](https://github.com/onqtam/doctest) testing framework, and the REPL command-line depends on [isocline](https://github.com/daanx/isocline).

# License

Luau implementation is distributed under the terms of [MIT License](https://github.com/luau-lang/luau/blob/master/LICENSE.txt). It is based on the Lua 5.x implementation, also under the MIT License.

When Luau is integrated into external projects, we ask that you honor the license agreement and include Luau attribution into the user-facing product documentation. Attribution making use of the [Luau logo](https://github.com/luau-lang/site/blob/master/logo.svg) is also encouraged when reasonable.
