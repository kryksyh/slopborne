#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne.app: finds the game folder (asked once), keeps saves and settings in
~/Library/Application Support/Bloodborne, backs up saves, then starts the game through run.py.

Frozen with PyInstaller (packaging/macos/package.sh): `--run` runs run.py and `--script <file>`
a preparation script inside this executable, so players need no Python. A save folder
(SPRJ0005) dropped on the app is imported.
"""
import datetime
import json
import locale
import os
from pathlib import Path
import platform
import runpy
import shutil
import subprocess
import sys
import zipfile

FROZEN = getattr(sys, 'frozen', False)
# The port (run.py, scripts/, patches/) is in Contents/Resources/port; the game executable
# and its libraries are in Contents/MacOS and Contents/Frameworks.
if FROZEN:
    CONTENTS = Path(sys.executable).resolve().parent.parent
    PORT = CONTENTS / 'Resources' / 'port'
    PROBE = CONTENTS / 'MacOS' / 'bb-probe'
    DEFAULT_INI = CONTENTS / 'Resources' / 'bbport.ini'
else:
    PORT = Path(__file__).resolve().parents[3]
    PROBE = Path(os.environ.get('BB_PROBE', PORT / 'out/macos/gpu/bb-probe'))
    DEFAULT_INI = Path(__file__).resolve().parent / 'bbport.ini'
DATA = Path.home() / 'Library' / 'Application Support' / 'Bloodborne'
CONFIG = DATA / 'config.json'
USER = DATA / 'user'
BACKUPS = DATA / 'backups'
LOGS = DATA / 'logs'
DEFAULTS = {'game_dir': '', 'fps_limit': 30, 'fullscreen': False}

RU = {
    'Choose your Bloodborne game folder (the one with eboot.bin).':
        'Выберите папку с игрой Bloodborne (в ней есть eboot.bin).',
    'This folder has no eboot.bin. Choose the folder of your Bloodborne dump.':
        'В этой папке нет eboot.bin. Выберите папку с дампом Bloodborne.',
    'Game version {version}: the frame rate and effect patches need version 01.09, so the game '
    'runs at its original 30 FPS settings.':
        'Версия игры {version}: патчи частоты кадров и эффектов требуют версию 01.09, игра '
        'запустится с исходными настройками 30 FPS.',
    'Bloodborne needs Rosetta to run on this Mac. Install it now? (Administrator password needed.)':
        'Для Bloodborne на этом Mac нужна Rosetta. Установить сейчас? (Нужен пароль администратора.)',
    'Only {free:.1f} GB of memory is free. Bloodborne needs about 8 GB for smooth play: close other '
    'apps (browsers, virtual machines, editors) to avoid stutter.':
        'Свободно только {free:.1f} ГБ памяти. Для плавной игры нужно около 8 ГБ: закройте другие '
        'программы (браузеры, виртуальные машины, редакторы), чтобы не было рывков.',
    'Import the save from {path}? The current saves are backed up first.':
        'Импортировать сохранение из {path}? Текущие сохранения сначала будут сохранены в резервную копию.',
    'No Bloodborne save (an SPRJ0005 folder with userdata files) was found in {path}.':
        'В {path} не найдено сохранение Bloodborne (папка SPRJ0005 с файлами userdata).',
    'Save imported.': 'Сохранение импортировано.',
    'Bloodborne closed unexpectedly (code {code}). Save a crash report to your Desktop?':
        'Bloodborne неожиданно закрылась (код {code}). Сохранить отчёт об ошибке на рабочий стол?',
    'Crash report saved: {path}': 'Отчёт сохранён: {path}',
    'Bloodborne is already running.': 'Bloodborne уже запущена.',
    'Continue': 'Продолжить', 'Quit': 'Выйти', 'Install': 'Установить', 'Import': 'Импортировать',
    'Cancel': 'Отмена', 'Save report': 'Сохранить отчёт', 'Close': 'Закрыть', 'OK': 'OK',
}


def russian():
    language = os.environ.get('BB_UI_LANGUAGE')
    if language:
        return language.startswith('ru')
    try:
        out = subprocess.run(['defaults', 'read', '-g', 'AppleLanguages'], capture_output=True,
                             text=True).stdout
        first = next((line.strip(' ",()') for line in out.splitlines() if line.strip(' ",()')), '')
        return first.startswith('ru')
    except OSError:
        return (locale.getlocale()[0] or '').startswith('ru')


RUSSIAN = russian()


def tr(text, **values):
    return (RU.get(text, text) if RUSSIAN else text).format(**values)


def applescript_text(text):
    return '"' + str(text).replace('\\', '\\\\').replace('"', '\\"') + '"'


def dialog(message, buttons=('OK',), default=None, caution=False):
    """A native alert; returns the pressed button's English text (None when dismissed)."""
    labels = [tr(b) for b in buttons]
    script = (f'display dialog {applescript_text(message)} with title "Bloodborne" '
              f'buttons {{{", ".join(map(applescript_text, labels))}}} '
              f'default button {applescript_text(tr(default or buttons[-1]))}'
              f'{" with icon caution" if caution else ""}')
    result = subprocess.run(['osascript', '-e', script], capture_output=True, text=True)
    pressed = result.stdout.strip().removeprefix('button returned:')
    return next((b for b, label in zip(buttons, labels) if label == pressed), None)


def choose_folder(prompt):
    script = f'POSIX path of (choose folder with prompt {applescript_text(prompt)})'
    result = subprocess.run(['osascript', '-e', script], capture_output=True, text=True)
    return Path(result.stdout.strip()) if result.returncode == 0 and result.stdout.strip() else None


def load_config():
    try:
        return {**DEFAULTS, **json.loads(CONFIG.read_text(encoding='utf-8'))}
    except (OSError, ValueError):
        return dict(DEFAULTS)


def save_config(config):
    DATA.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text(json.dumps(config, indent=2) + '\n', encoding='utf-8')


def game_info(game):
    """param.sfo of the game folder: title id and version (empty when unreadable)."""
    sys.path.insert(0, str(PORT / 'scripts'))
    try:
        from prepare import sfo
        values = sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes())
        return values.get('TITLE_ID', ''), values.get('APP_VER', '')
    except (OSError, ValueError, ImportError):
        return '', ''
    finally:
        sys.path.remove(str(PORT / 'scripts'))


def find_game(config):
    game = Path(config['game_dir']) if config['game_dir'] else None
    while not game or not (game / 'eboot.bin').is_file():
        if game:
            dialog(tr('This folder has no eboot.bin. Choose the folder of your Bloodborne dump.'),
                   caution=True)
        game = choose_folder(tr('Choose your Bloodborne game folder (the one with eboot.bin).'))
        if not game:
            return None
    if str(game) != config['game_dir']:
        config['game_dir'] = str(game)
        save_config(config)
        _, version = game_info(game)
        if version and version != '01.09':
            dialog(tr('Game version {version}: the frame rate and effect patches need version 01.09, '
                      'so the game runs at its original 30 FPS settings.', version=version))
    return game


def rosetta_ready():
    if platform.machine() != 'arm64':
        return True
    if subprocess.run(['/usr/bin/arch', '-x86_64', '/usr/bin/true'], capture_output=True).returncode == 0:
        return True
    if dialog(tr('Bloodborne needs Rosetta to run on this Mac. Install it now? (Administrator '
                 'password needed.)'), buttons=('Quit', 'Install')) != 'Install':
        return False
    script = ('do shell script "/usr/sbin/softwareupdate --install-rosetta --agree-to-license" '
              'with administrator privileges')
    subprocess.run(['osascript', '-e', script])
    return subprocess.run(['/usr/bin/arch', '-x86_64', '/usr/bin/true'], capture_output=True).returncode == 0


def free_memory_gb():
    """Free plus reclaimable (inactive, purgeable, speculative) memory, as vm_stat reports it."""
    try:
        out = subprocess.run(['vm_stat'], capture_output=True, text=True).stdout
    except OSError:
        return None
    page = 16384
    pages = 0
    for line in out.splitlines():
        if 'page size of' in line:
            page = int(line.split('page size of')[1].split()[0])
        name, _, value = line.partition(':')
        if name in ('Pages free', 'Pages inactive', 'Pages purgeable', 'Pages speculative'):
            pages += int(value.strip().rstrip('.'))
    return pages * page / 2**30


def memory_ok():
    free = free_memory_gb()
    if free is None or free >= 8:
        return True
    return dialog(tr('Only {free:.1f} GB of memory is free. Bloodborne needs about 8 GB for smooth '
                     'play: close other apps (browsers, virtual machines, editors) to avoid '
                     'stutter.', free=free), buttons=('Quit', 'Continue'), caution=True) == 'Continue'


def save_signature(root):
    return sorted((str(p.relative_to(root)), p.stat().st_size, int(p.stat().st_mtime))
                  for p in root.rglob('*') if p.is_file())


def backup_saves():
    """Copies user/savedata to backups/<time> when it changed since the last backup."""
    saves = USER / 'savedata'
    if not saves.is_dir() or not any(saves.rglob('userdata*')):
        return
    signature = save_signature(saves)
    previous = sorted(p for p in BACKUPS.glob('*') if p.is_dir()) if BACKUPS.is_dir() else []
    if previous and save_signature(previous[-1] / 'savedata') == signature:
        return
    target = BACKUPS / datetime.datetime.now().strftime('%Y-%m-%d_%H-%M-%S')
    shutil.copytree(saves, target / 'savedata', copy_function=shutil.copy2)
    # Retain every snapshot: launching the game must never delete an older save backup.


def find_save_folder(path):
    path = Path(path)
    candidates = [path] + sorted(p for p in path.rglob('SPRJ0005') if p.is_dir()) if path.is_dir() else []
    return next((c for c in candidates if any(c.glob('userdata*'))), None)


def import_save(path, game):
    source = find_save_folder(path)
    if not source:
        dialog(tr('No Bloodborne save (an SPRJ0005 folder with userdata files) was found in {path}.',
                  path=path), caution=True)
        return
    if dialog(tr('Import the save from {path}? The current saves are backed up first.', path=source),
              buttons=('Cancel', 'Import')) != 'Import':
        return
    backup_saves()
    existing = sorted((USER / 'savedata' / '1').glob('*/SPRJ0005'))
    title = existing[0].parent.name if existing else (game_info(game)[0] or 'CUSA03173')
    target = USER / 'savedata' / '1' / title / 'SPRJ0005'
    target.mkdir(parents=True, exist_ok=True)
    for file in source.iterdir():
        if file.is_file() and file.name.startswith(('userdata', 'backup')):
            shutil.copy2(file, target / file.name)
    dialog(tr('Save imported.'))


def crash_report(code):
    if dialog(tr('Bloodborne closed unexpectedly (code {code}). Save a crash report to your Desktop?',
                 code=code), buttons=('Close', 'Save report'), caution=True) != 'Save report':
        return
    stamp = datetime.datetime.now().strftime('%Y-%m-%d_%H-%M-%S')
    path = Path.home() / 'Desktop' / f'Bloodborne-crash-{stamp}.zip'
    system = []
    for command in (['sw_vers'], ['sysctl', '-n', 'machdep.cpu.brand_string', 'hw.memsize'], ['vm_stat']):
        system.append(subprocess.run(command, capture_output=True, text=True).stdout)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as report:
        report.writestr('system.txt', '\n'.join(system))
        for name in ('last_run.log', 'previous_run.log'):
            if (LOGS / name).is_file():
                report.write(LOGS / name, name)
        if (DATA / 'bbport.ini').is_file():
            report.write(DATA / 'bbport.ini', 'bbport.ini')
    dialog(tr('Crash report saved: {path}', path=path))


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT / 'run.py')
        sys.argv = [path, *argv[1:]]
    # As `python script.py`: the script's folder first on the import path (its sibling modules).
    sys.path.insert(0, str(Path(path).resolve().parent))
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def game_environment(config, game):
    env = dict(os.environ)
    env.update(BB_PROBE=str(PROBE), BB_DATA_DIR=str(DATA), BB_GAME_DIR=str(game),
               BB_USER_DIR=str(USER), BB_UI_LANGUAGE='ru' if RUSSIAN else 'en')
    # Tested MoltenVK path: direct Metal bindings and retained command resources.
    env.setdefault('MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS', '0')
    env.setdefault('MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES', '1')
    # Frame-time game timing with an even cap: 30 FPS keeps 33.3 ms pacing on Apple Silicon.
    env.setdefault('BB_FPS', 'uncap')
    env.setdefault('BB_FPS_LIMIT', str(config['fps_limit']))
    env.setdefault('BB_FULLSCREEN', '1' if config['fullscreen'] else '0')
    return env


def play(config, game):
    DATA.mkdir(parents=True, exist_ok=True)
    LOGS.mkdir(parents=True, exist_ok=True)
    if not (DATA / 'bbport.ini').is_file() and DEFAULT_INI.is_file():
        shutil.copy2(DEFAULT_INI, DATA / 'bbport.ini')
    backup_saves()
    if (LOGS / 'last_run.log').is_file():
        shutil.copy2(LOGS / 'last_run.log', LOGS / 'previous_run.log')
    command = [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT / 'run.py')]
    with open(LOGS / 'last_run.log', 'w', encoding='utf-8') as log:
        result = subprocess.run(command, env=game_environment(config, game), stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT)
    if result.returncode not in (0, 130):
        crash_report(result.returncode)
    return result.returncode


def main():
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    if subprocess.run(['pgrep', '-x', 'bb-probe'], capture_output=True).returncode == 0:
        dialog(tr('Bloodborne is already running.'))
        return 1
    if not rosetta_ready():
        return 1
    config = load_config()
    game = find_game(config)
    if not game:
        return 1
    dropped = [a for a in args if not a.startswith('-') and Path(a).exists()]
    for path in dropped:
        import_save(path, game)
    if not memory_ok():
        return 1
    return play(config, game)


if __name__ == '__main__':
    sys.exit(main())
