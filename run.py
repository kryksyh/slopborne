#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows and macOS app counterpart of run.sh: prepares the game image and starts bb-probe.

Same environment variables as run.sh (BB_GAME_DIR, BB_DATA_DIR, BB_FPS, BB_RENDER_RES,
BB_LIVE_RES, BB_PATCHES, BB_MODS_*, BB_USER_DIR, ...). Extra arguments go to bb-probe.
The in-game "Apply and restart" runs this script again through BB_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import ctypes
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parent
EXE = '.exe' if sys.platform == 'win32' else ''


# Co-op: the community service that answers Bloodborne's own game-server requests
# (messages, bloodstains, sign-in); summon requests go to the shadNet server instead.
COMMUNITY_SERVER = 'https://thehuntersdream.com'
GAME_SERVER = 'https://ss4.scej-network.jp:20443'
# The server's ss.info sends the game to this port for its API (summons among them).
COMMUNITY_API_PORT = 18671


def summon_service(webapi):
    """True when a shadNet WebAPI answers Bloodborne's summon calls: the route exists there
    (an empty request is refused with 400), where other servers answer 404."""
    import urllib.error
    import urllib.request
    request = urllib.request.Request(webapi.rstrip('/') + '/summon_messenger/create', data=b'',
                                     method='POST')
    try:
        urllib.request.urlopen(request, timeout=4).close()
        return True
    except urllib.error.HTTPError as error:
        return error.code != 404
    except (OSError, ValueError):
        return False


def menu_online(env, config):
    """The in-game menu's Online settings (bbport.ini), unless the environment sets online play."""
    if 'BB_ONLINE' in env or ini_value(config, 'online') != '1':
        return
    npid = (ini_value(config, 'online_npid') or '').strip()
    if not npid:
        print('Online: no shadNet account in the menu; playing offline')
        return
    server = (ini_value(config, 'online_server') or '').strip() or 'srv.shadps4.net:31313'
    host = server.rsplit(':', 1)[0] if server.count(':') == 1 else server
    env.update(BB_ONLINE='1', BB_SHADNET_SERVER=server, BB_SHADNET_NPID=npid,
               BB_SHADNET_PASSWORD=ini_value(config, 'online_password') or '')
    env.setdefault('BB_SHADNET_WEBAPI', f'http://{host}:31315')
    env.setdefault('BB_UPNP', '1')
    env.setdefault('BB_COMMUNITY_SERVER', COMMUNITY_SERVER)
    print(f'Online: shadNet {server} as {npid}')


def online_overrides(env, user_dir):
    """Writes user/host_overrides.json for this launch. A player who needs other addresses
    puts them in user/host_overrides.custom.json, which is then used as it is."""
    custom = user_dir / 'host_overrides.custom.json'
    path = custom if custom.exists() else user_dir / 'host_overrides.json'
    if path != custom:
        community = (env.get('BB_COMMUNITY_SERVER') or COMMUNITY_SERVER).rstrip('/')
        if '://' not in community:
            community = 'https://' + community
        host = community.split('://', 1)[1]
        webapi = env.get('BB_SHADNET_WEBAPI') or 'http://127.0.0.1:31315'
        # Game server -> community server (same port, 20443). Summon signs -> the shadNet
        # WebAPI when that server has the Bloodborne summon service (the shadp2p server),
        # otherwise they stay with the community server.
        overrides = {GAME_SERVER: community}
        if summon_service(webapi):
            overrides.update({f'{scheme}://{host}:{COMMUNITY_API_PORT}/summon_messenger': webapi
                              for scheme in ('https', 'http')})
            print(f'Co-op: summon signs through {webapi}')
        else:
            print(f'Co-op: {webapi} has no summon service; summon signs through {community}')
        path.write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(path)


# The 1.09 executable with Lance McDonald's 60 fps patch applied to it, as some dumps ship it
# (seen on US CUSA00900): one of its edits inverts a branch in the menu code (+0xf6d90d) and the
# gestures menu crashes the game. The port has its own 60 fps; the clean 1.09 eboot.bin is needed.
LANCE_60FPS_IMAGE = 'bbc91f4dff6bc3118b039464289e39878a8fdfc802a07fab9d5fe84d935a676f'


def image_sha256(game):
    """The SHA-256 of eboot.bin's loaded image (its loadable segments at their addresses): the
    same whatever tool dumped it, unlike the SELF around it."""
    import hashlib
    sys.path.insert(0, str(PORT / 'scripts'))
    from prepare import parse_self, span
    elf, _header, ph, _segments, _missing = parse_self((Path(game) / 'eboot.bin').read_bytes())
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return hashlib.sha256(image).hexdigest()


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    if sys.platform != 'win32':
        for _ in range(300):
            try:
                os.kill(pid, 0)
            except OSError:
                return
            time.sleep(0.1)
        return
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    if sys.platform != 'win32':
        return 0
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def run_script(name, *args, capture=False, optional=False):
    # Inside the packaged Bloodborne.exe (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    if capture:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                creationflags=no_console())
        if result.returncode:
            if optional:
                raise subprocess.CalledProcessError(result.returncode, command)
            sys.exit(result.returncode)
        return result.stdout
    # stdin given: the output handles are passed explicitly (a windowed Bloodborne.exe child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return None


def ini_value(path, key):
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.fullmatch(rf'{re.escape(key)}=(.*)', line.strip())
            if match:
                return match.group(1)
    except OSError:
        pass
    return None


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def install_pipeline_list(game, gpu_user_dir, out, env):
    """Pipelines the port ships (pipelines/: descriptions and fetch shaders, no SPIR-V) go into the
    game's pipeline cache, where nothing is overwritten; the game builds their shaders at start from
    the game's own shader programs, found here once (scripts/game_shaders.py)."""
    shipped = PORT / 'pipelines'
    if not shipped.is_dir():
        return
    programs = out / 'shader_programs'
    stamp = programs / 'source.txt'
    source = f'{(game / "eboot.bin").stat().st_mtime_ns} {len(list((game / "dvdroot_ps4/shader").glob("*.dcx")))}'
    if not stamp.is_file() or stamp.read_text() != source:
        run_script('game_shaders.py', game, '--out', programs)
        stamp.write_text(source)
    env['BB_SHADER_PROGRAMS_DIR'] = str(programs)
    sys.path.insert(0, str(PORT / 'scripts'))
    from prepare import sfo
    title = sfo((game / 'sce_sys/param.sfo').read_bytes()).get('TITLE_ID', 'CUSA03173')
    cache = gpu_user_dir / 'cache' / title
    cache.mkdir(parents=True, exist_ok=True)
    for file in shipped.iterdir():
        if file.suffix in ('.meta', '.key', '.fetch') and not (cache / file.name).exists():
            shutil.copy2(file, cache / file.name)


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    # As run.sh: relative paths (BB_GAME_DIR, BB_DATA_DIR, fsr4_shaders/) are from the port.
    os.chdir(PORT)
    env = os.environ
    data = Path(env.get('BB_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = env['BB_CONFIG']
    if not env.get('BB_FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['BB_FSR411_DIR'] = str(data / 'fsr4_411')

    probe = Path(env['BB_PROBE']) if env.get('BB_PROBE') else find_executable('bb-probe' + EXE)
    if not probe or not probe.is_file():
        fail(f'bb-probe{EXE} not found: build it with build.sh (MSYS2 CLANG64) or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'

    game = Path(env.get('BB_GAME_DIR', PORT.parent / 'CUSA03173'))
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set BB_GAME_DIR).')
    original_game = game.resolve()
    if env.get('BB_SKIP_GAME_CHECK') != '1':
        try:
            prepatched = image_sha256(game) == LANCE_60FPS_IMAGE
        except (OSError, ValueError, KeyError, IndexError):
            prepatched = False  # prepare.py reports unreadable executables itself
        if prepatched:
            fail("This eboot.bin has Lance McDonald's 60 fps patch applied to it, which crashes the "
                 'game when the gestures menu opens (the port has its own 60 fps): copy eboot.bin '
                 'from a clean dump of the 1.09 update into the game folder, replacing this one. '
                 'BB_SKIP_GAME_CHECK=1 starts it anyway.')
    game = Path(run_script('mods.py', game, '--out', out,
                           '--mods-dir', env.get('BB_MODS_DIR', data / 'mods'),
                           '--config', env.get('BB_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('BB_MODS_ENABLED', '1'), capture=True).strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        # The helper uses the same SDL identity/calibration as the overlay.
        # Generate a retained view after user mods, without editing their files.
        helper = probe.parent / ('bb-controller-info' + EXE)
        if helper.is_file():
            try:
                game = Path(run_script('button_glyphs.py', game, '--out', out,
                                       '--helper', helper, capture=True, optional=True).strip())
            except (OSError, ValueError, StopIteration, subprocess.SubprocessError) as error:
                print(f'Controller glyphs: could not prepare replacements: {error}; using original icons')
        else:
            print('Controller glyphs: bb-controller-info is missing; using original icons')
        run_script('prepare.py', game, '--out', out)
        run_script('link_libc.py', game, '--out', out)
        run_script('link_modules.py', game, '--out', out)
        run_script('content_profile.py', game, '--out', out, '--sku', env.get('BB_CONTENT_SKU', 'full'))

        # Patches exist for game version 01.09 only (patches.py applies none to others): other
        # versions keep the game's 30 FPS timing and change resolutions live.
        sys.path.insert(0, str(PORT / 'scripts'))
        from patches import eboot_matches_patches, game_app_version
        version = game_app_version(game)
        forced = bool(env.get('BB_FORCE_PATCHES'))
        patched = version in (None, '01.09') or forced
        if not patched:
            print(f'Game version {version}: community patches need 01.09; 30 FPS, no effect patches')
        elif not forced and not eboot_matches_patches(out / 'eboot.elf'):
            # param.sfo says 01.09, the code is another build's: the patches would break it.
            patched = False
            print('Game files: eboot.bin is not the 01.09 build the community patches were made for; '
                  '30 FPS, no effect patches')
        # Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if env.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                env.pop(key, None)
        fps = env.get('BB_FPS', 'uncap') if patched else '30'
        scaled_render = scaled_output = None
        fixed = False
        if not env.get('BB_RENDER_RES'):
            printed = run_script('patches.py', '--print-scaled', '--settings', config, capture=True).split()
            if len(printed) in (2, 3):
                scaled_render, scaled_output = printed[:2]
                fixed = len(printed) == 3
        live = '0'
        if scaled_output and not (fixed and patched):
            live = env.get('BB_LIVE_RES') or ini_value(config, 'live_resolution') or '0'
            if live == 'auto':
                caps = probe.parent / ('bb-gpu-capabilities' + EXE)
                result = subprocess.run([str(caps), '--live-resolution'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        text=True, creationflags=no_console())
                live = result.stdout.strip() if result.returncode == 0 else '0'
            live = '1' if live == '1' or not patched else '0'
        if live == '1':
            print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
        elif scaled_output:
            env.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
            # Only outputs above 1080p need more direct memory: on Macs the extra reserve swapped
            # (PabloVSouza 2933ee8: 9152 MiB ran the clinic at 7 FPS instead of 36 on 18 GB).
            if int(scaled_output.split('x')[0]) > 1920:
                env.setdefault('BB_DMEM_MB', '9152')
            print(f'Output {scaled_output}: scene {scaled_render}, direct memory {env.get("BB_DMEM_MB", "default")} MiB '
                  '(live_resolution=1: live changes)')
        run_script('patches.py', '--out', out, '--fps', fps, '--extra', env.get('BB_PATCHES', ''),
                   '--settings', config, '--game-dir', game, '--render-res', env.get('BB_RENDER_RES', ''),
                   '--output-res', env.get('BB_OUTPUT_RES', ''),
                   '--patches-dir', env.get('BB_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'))
        if not env.get('BB_VBLANK_HZ'):
            env['BB_VBLANK_HZ'] = {'uncap': '0', '90': '90'}.get(fps, '60')

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = Path(env.get('BB_USER_DIR', data / 'user')).resolve()
        user_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault('BB_GPU_USER_DIR', str(user_dir))
        install_pipeline_list(game, Path(env['BB_GPU_USER_DIR']), out, env)
        menu_online(env, config)
        if env.get('BB_ONLINE') == '1':
            online_overrides(env, user_dir)
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        restart = [sys.executable, *this, '--after', str(os.getpid()), *args]
        env['BB_RESTART_COMMAND'] = (subprocess.list2cmdline(restart) if sys.platform == 'win32'
                                     else shlex.join(restart))
        command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', env.get('BB_TIMEOUT', '0'), *args]
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
    finally:
        # macOS glyph views may reference a user-mod view. Retain both, along
        # with all original files, so an existing view always stays usable.
        if mod_view and sys.platform != 'darwin':
            shutil.rmtree(mod_view, ignore_errors=True)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
