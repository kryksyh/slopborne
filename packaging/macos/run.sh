#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
export BB_PROBE=${BB_PROBE:-$PWD/out/macos/gpu/bb-probe}
[[ -x $BB_PROBE ]] || { echo 'Build first: bash build.sh --test' >&2; exit 1; }
export BB_PREBUILT=1
export BB_DATA_DIR=${BB_DATA_DIR:-$PWD/out/macos/run-data}
# Tested MoltenVK path: direct Metal bindings and retained command resources.
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=${MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS:-0}
export MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=${MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES:-1}
game=${BB_GAME_DIR:-../CUSA03173}
[[ -d $game ]] || { echo "Game directory not found: $game" >&2; exit 1; }
game=$(realpath "$game")
if [[ ${BB_FILE_SANDBOX:-0} == 1 ]]; then
    exec /usr/bin/sandbox-exec -f "$PWD/packaging/macos/protect-files.sb" \
        -D "GAME_DIR=$game" -D "DOWNLOAD_DIR=$HOME/Downloads" bash run.sh "$@"
fi
exec bash run.sh "$@"
