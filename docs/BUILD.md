# Building from source

## macOS

Apple Silicon, macOS 15 or newer, Xcode command line tools and Rosetta. The game and its
libraries are built for x86-64 (the game's own code runs under Rosetta), so the linked
libraries come from an Intel Homebrew in `/usr/local`.

```sh
brew install cmake ninja pkgconf python@3.13 boost vulkan-headers glslang spirv-tools
arch -x86_64 /usr/local/bin/brew install molten-vk fmt robin-map zydis nlohmann-json
git submodule update --init --recursive
bash packaging/macos/build.sh --bootstrap --performance   # SDL, FFmpeg, OpenSSL, ... into out/macos/prefix, then the game
bash packaging/macos/package.sh                          # out/macos/package-*/Bloodborne-macOS-*.zip
```

Later builds skip `--bootstrap`. `--test` adds the regression tests. Details, options and
diagnostics: [packaging/macos/README.md](../packaging/macos/README.md).

## Windows

MSYS2 CLANG64 shell, then `bash build.sh` and `bash packaging/windows/package.sh`. DLSS is
built separately with `packaging/windows/build_dlss.sh`. Details:
[packaging/windows/README.md](../packaging/windows/README.md).
