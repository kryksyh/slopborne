#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds Bloodborne.app and a zip for players: the front end (packaging/macos/app, frozen with
# PyInstaller so players need no Python), bb-probe with every non-system library it uses,
# run.py with the preparation scripts and patches, and default settings. No game files.
# Each run writes a new out/macos/package-<time>/; earlier packages are kept.
# BB_PROBE selects the game executable (default: the Release build of build.sh --performance).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
probe=${BB_PROBE:-out/macos/gpu-performance/bb-probe}
caps=$(dirname -- "$probe")/bb-gpu-capabilities
[[ -x $probe ]] || { echo "No $probe: run bash build.sh --performance (or set BB_PROBE)" >&2; exit 1; }
[[ -x $caps ]] || caps=out/macos/gpu/bb-gpu-capabilities
version=${BB_VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}
stage=$PWD/out/macos/package-$(date +%Y%m%d-%H%M%S)
mkdir -p "$stage"

# A private venv with PyInstaller (created once).
if [[ ! -x out/macos/pyenv/bin/python ]]; then
    python3 -m venv out/macos/pyenv
    out/macos/pyenv/bin/python -m pip install -q --disable-pip-version-check pyinstaller
fi

# Icon from the launcher's PNG.
iconset=$stage/Bloodborne.iconset
mkdir -p "$iconset"
for size in 16 32 64 128 256 512; do
    sips -z $size $size launcher/bloodborne.png --out "$iconset/icon_${size}x${size}.png" >/dev/null
    sips -z $((size * 2)) $((size * 2)) launcher/bloodborne.png --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$stage/Bloodborne.icns"

# The preparation scripts run inside the frozen executable (--script): their standard modules.
hidden=()
for module in argparse base64 collections hashlib json re shutil struct tempfile xml.etree.ElementTree \
              urllib.request zipfile ctypes shlex time; do
    hidden+=(--hidden-import "$module")
done
out/macos/pyenv/bin/python -m PyInstaller --noconfirm --log-level WARN --windowed --argv-emulation \
    --name Bloodborne --icon "$stage/Bloodborne.icns" --osx-bundle-identifier io.github.bbport.bloodborne \
    --distpath "$stage/dist" --workpath "$stage/work" --specpath "$stage/work" \
    --paths "$PWD/scripts" "${hidden[@]}" "$PWD/packaging/macos/app/bloodborne_mac.py"
app=$stage/Bloodborne.app
mv "$stage/dist/Bloodborne.app" "$app"

contents=$app/Contents
mkdir -p "$contents/Resources/port/scripts" "$contents/Resources/port/patches"
cp run.py "$contents/Resources/port/"
cp scripts/*.py "$contents/Resources/port/scripts/"
cp patches/Bloodborne.xml "$contents/Resources/port/patches/"
# The pipeline list (scripts/export_pipelines.py): built at first start from the player's game files.
# Export it from ONE cache made by the build being packaged (permutation numbers are local to a
# cache, and entries from other builds may not rebuild), e.g. replays into an empty cache.
pipelines=${BB_PIPELINES:-out/macos/pipelines}
if [[ -d $pipelines ]]; then
    mkdir -p "$contents/Resources/port/pipelines"
    cp "$pipelines"/*.key "$pipelines"/*.meta "$pipelines"/*.fetch "$contents/Resources/port/pipelines/"
fi
cp packaging/macos/app/bbport.ini "$contents/Resources/"
cp "$probe" "$caps" "$contents/MacOS/"
python3 packaging/macos/bundle_dylibs.py "$app" bb-probe "$(basename -- "$caps")"

plist=$contents/Info.plist
set_plist() { /usr/libexec/PlistBuddy -c "Delete :$1" "$plist" 2>/dev/null || true
              /usr/libexec/PlistBuddy -c "Add :$1 $2 $3" "$plist"; }
set_plist CFBundleDisplayName string Bloodborne
set_plist CFBundleShortVersionString string "$version"
set_plist LSMinimumSystemVersion string 15.0
set_plist LSApplicationCategoryType string public.app-category.role-playing-games
set_plist LSSupportsGameMode bool true
set_plist NSHighResolutionCapable bool true
# The front end and the preparation scripts it runs (this executable again) stay out of the Dock:
# the game's window (bb-probe, a regular app through SDL) is the app's one Dock icon.
set_plist LSUIElement bool true

# Ad-hoc signature (no Developer ID): Gatekeeper asks once (see README-macOS.txt).
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"

folder=$stage/Bloodborne-macOS
mkdir -p "$folder"
ditto "$app" "$folder/Bloodborne.app"
cp packaging/macos/README-macOS.txt "$folder/"
(cd "$stage" && ditto -c -k --keepParent Bloodborne-macOS "Bloodborne-macOS-$version.zip")
echo "Package: $stage/Bloodborne-macOS-$version.zip"
