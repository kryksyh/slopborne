# Building from source

## macOS

Apple Silicon, macOS 15 or newer, Xcode command line tools and Rosetta. The game and its
libraries are built for x86-64 (the game's own code runs under Rosetta): `--bootstrap` builds
the linked libraries for x86-64 into `out/macos/prefix` and fetches MoltenVK's universal build.

```sh
brew install cmake ninja pkgconf python@3.13 boost vulkan-headers glslang spirv-tools robin-map nlohmann-json
git submodule update --init --recursive
bash packaging/macos/build.sh --bootstrap --performance   # SDL, FFmpeg, OpenSSL, fmt, Zydis, ... then the game
bash packaging/macos/package.sh                          # out/macos/package-*/Bloodborne-macOS-*.zip
```

Later builds skip `--bootstrap`. `--test` adds the regression tests. Details, options and
diagnostics: [packaging/macos/README.md](../packaging/macos/README.md).

## Windows

MSYS2 CLANG64 shell, then `bash build.sh` and `bash packaging/windows/package.sh`. DLSS is
built separately with `packaging/windows/build_dlss.sh`. Details:
[packaging/windows/README.md](../packaging/windows/README.md).
