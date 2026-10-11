#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Copies the non-system libraries of the given executables (and theirs) into an app's
Contents/Frameworks/game/<architecture> and points every reference at @rpath, so the app runs on Macs without
Homebrew. A folder of their own: the front end's Python libraries (arm64) in Contents/Frameworks
use some of the same names (libssl, libcrypto). Usage: bundle_dylibs.py <App.app> <executable>..."""
from pathlib import Path
import argparse
import hashlib
import shutil
import subprocess
import sys

SYSTEM = ('/System/', '/usr/lib/')


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def dependencies(binary):
    lines = run('otool', '-L', str(binary)).splitlines()[1:]
    deps = [line.strip().split(' (compatibility')[0] for line in lines]
    own_id = run('otool', '-D', str(binary)).splitlines()[1:]
    return [d for d in deps if d not in own_id]


def rpaths(binary):
    lines = run('otool', '-l', str(binary)).splitlines()
    return [lines[i + 2].split()[1] for i, line in enumerate(lines) if 'LC_RPATH' in line]


def architectures(binary):
    return tuple(sorted(run('lipo', '-archs', str(binary)).split()))


def expanded(path, loader, executable):
    return path.replace('@loader_path', str(loader.parent)).replace('@executable_path', str(executable.parent))


def resolve(dep, search, loader=None, executable=None):
    loader = loader or Path('.')
    executable = executable or loader
    if dep.startswith('@rpath/'):
        for directory in search:
            candidate = Path(expanded(directory, loader, executable)) / dep.removeprefix('@rpath/')
            if candidate.is_file():
                return candidate.resolve()
        raise ValueError(f'{dep}: not found in {search}')
    candidate = Path(expanded(dep, loader, executable)).resolve()
    if not candidate.is_file():
        raise ValueError(f'{dep}: not found at {candidate}')
    return candidate


def bundle(app, executables):
    """Each CPU architecture gets its own dependency directory.

    Only binaries copied into this app are modified. Original dependency paths are
    kept separately for resolving loader-relative references during traversal.
    """
    app = Path(app).resolve()
    frameworks = app / 'Contents' / 'Frameworks' / 'game'
    frameworks.mkdir(parents=True, exist_ok=True)
    copied = {}
    done = set()
    for name in executables:
        executable = app / 'Contents' / 'MacOS' / name
        if Path(name).name != name or not executable.is_file():
            raise ValueError(f'Expected an executable name in Contents/MacOS: {name}')
        required = architectures(executable)
        if not required:
            raise ValueError(f'No CPU architectures in {executable}')
        group = '-'.join(required)
        folder = frameworks / group
        folder.mkdir(exist_ok=True)
        pending = [(executable, executable, [], True)]
        while pending:
            binary, original, inherited, is_executable = pending.pop()
            if binary in done:
                continue
            done.add(binary)
            own_search = [expanded(p, original, executable) for p in rpaths(original)]
            search = own_search + inherited
            for dep in dependencies(original):
                if dep.startswith(SYSTEM):
                    continue
                source = resolve(dep, search, original, executable)
                available = architectures(source)
                if not set(required).issubset(available):
                    raise ValueError(f'{name} needs {required}, but {source} contains {available}')
                libname = source.name
                target = folder / libname
                if target in copied:
                    prior = copied[target]
                    if prior != source and hashlib.sha256(prior.read_bytes()).digest() != hashlib.sha256(source.read_bytes()).digest():
                        raise ValueError(f'Different libraries share {group}/{libname}: {prior} and {source}')
                else:
                    # Package staging directories must be fresh; never overwrite a
                    # previous bundle or silently reuse an unverified dependency.
                    if target.exists():
                        raise ValueError(f'Existing bundle dependency: {target}; use a new staging directory')
                    shutil.copy2(source, target)
                    target.chmod(0o755)
                    copied[target] = source
                    run('install_name_tool', '-id', f'@rpath/{libname}', str(target))
                    pending.append((target, source, search, False))
                replacement = f'@rpath/{libname}'
                if dep != replacement:
                    run('install_name_tool', '-change', dep, replacement, str(binary))
            for old in dict.fromkeys(rpaths(binary)):
                run('install_name_tool', '-delete_rpath', old, str(binary))
            new_path = f'@executable_path/../Frameworks/game/{group}' if is_executable else '@loader_path'
            run('install_name_tool', '-add_rpath', new_path, str(binary))
    for binary in sorted(done):
        leftover = [d for d in dependencies(binary)
                    if not d.startswith(SYSTEM) and not d.startswith('@rpath/')]
        if leftover:
            raise ValueError(f'{binary.name}: unresolved {leftover}')
    print(f'Bundled {len(copied)} libraries into {frameworks}')
    return frameworks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('executables', nargs='+')
    args = parser.parse_args()
    try:
        bundle(args.app, args.executables)
    except ValueError as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
