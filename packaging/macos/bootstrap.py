#!/usr/bin/env python3
"""Build pinned x86-64 dependencies locally; never mixes ARM and Intel libraries."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    ('SDL3', 'SDL3-3.4.2', 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.2/SDL3-3.4.2.tar.gz', 'ef39a2e3f9a8a78296c40da701967dd1b0d0d6e267e483863ce70f8a03b4050c'),
    ('xxhash', 'xxHash-0.8.3', 'https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz', 'aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80'),
    ('protobuf', 'protobuf-21.12', 'https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.tar.gz', '22fdaf641b31655d4b2297f9981fa5203b2866f8332d3c6333f6b0107bb320de'),
    ('ffmpeg', 'ffmpeg-8.0', 'https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz', 'b2751fccb6cc4c77708113cd78b561059b6fa904b24162fa0be2d60273d27b8e'),
    ('openssl', 'openssl-3.5.0', 'https://github.com/openssl/openssl/releases/download/openssl-3.5.0/openssl-3.5.0.tar.gz', '344d0a79f1a9b08029b0744e2cc401a43f9c90acd1044d09a530b4885a8e9fc0'),
    ('epoll-shim', 'epoll-shim-0.0.20240608', 'https://github.com/jiixyj/epoll-shim/archive/refs/tags/v0.0.20240608.tar.gz', '8f5125217e4a0eeb96ab01f9dfd56c38f85ac3e8f26ef2578e538e72e87862cb'),
    ('miniupnpc', 'miniupnpc-2.3.3', 'https://miniupnp.tuxfamily.org/files/miniupnpc-2.3.3.tar.gz', 'd52a0afa614ad6c088cc9ddff1ae7d29c8c595ac5fdd321170a05f41e634bd1a'),
    ('fmt', 'fmt-12.2.0', 'https://github.com/fmtlib/fmt/archive/refs/tags/12.2.0.tar.gz', '8b852bb5aa6e7d8564f9e81394055395dd1d1936d38dfd3a17792a02bebd7af0'),
    ('zycore', 'zycore-c-1.5.2', 'https://github.com/zyantific/zycore-c/archive/refs/tags/v1.5.2.tar.gz', '943f91eb9ab2a8cc01ab9f8b785e769a273502071e0ee8011cdfcaad93947cec'),
    ('zydis', 'zydis-4.1.1', 'https://github.com/zyantific/zydis/archive/refs/tags/v4.1.1.tar.gz', '45c6d4d499a1cc80780f7834747c637509777c01dca1e98c5e7c0bfaccdb1514'),
]
# Homebrew no longer installs an Intel copy on Apple Silicon: MoltenVK's own universal build.
MOLTENVK = ('https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar',
            'f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e',
            'MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib')


def download(url, path, digest):
    if not path.exists():
        temporary = path.with_suffix(path.suffix + '.part')
        with urllib.request.urlopen(url, timeout=120) as response, temporary.open('wb') as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
        temporary.replace(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise RuntimeError(f'Checksum mismatch: {path}; remove it and retry')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-only', action='store_true', help='Only build SDL for CPU tests')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 6))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    base = ROOT / 'out/macos'
    deps, prefix = base / 'deps', base / 'prefix'
    deps.mkdir(parents=True, exist_ok=True)
    prefix.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, MACOSX_DEPLOYMENT_TARGET='15.0')
    for name, dirname, url, digest in SOURCES:
        if args.runtime_only and name != 'SDL3':
            continue
        stamp = prefix / f'.{name}-{digest[:12]}'
        if stamp.exists():
            continue
        archive = deps / (name + ('.tar.xz' if name == 'ffmpeg' else '.tar.gz'))
        print(f'Building {name} for x86_64...', flush=True)
        download(url, archive, digest)
        source = deps / dirname
        if not source.is_dir():
            with tarfile.open(archive) as package:
                package.extractall(deps, filter='data')
        log_path = base / f'{name}-bootstrap.log'
        with log_path.open('w') as log:
            def run(command, cwd=ROOT):
                subprocess.run(list(map(str, command)), cwd=cwd, env=env, stdout=log,
                               stderr=subprocess.STDOUT, check=True)
            try:
                if name == 'ffmpeg':
                    run(['./configure', f'--prefix={prefix}', '--arch=x86_64',
                         '--cc=clang -arch x86_64', '--cxx=clang++ -arch x86_64',
                         '--disable-x86asm', '--disable-programs', '--disable-doc',
                         '--disable-autodetect', '--enable-shared', '--disable-static',
                         '--enable-pic'], source)
                    run(['make', f'-j{args.jobs}'], source)
                    run(['make', 'install'], source)
                elif name == 'openssl':
                    run(['./Configure', 'darwin64-x86_64-cc', f'--prefix={prefix}', 'no-tests'], source)
                    run(['make', f'-j{args.jobs}'], source)
                    run(['make', 'install_sw'], source)
                else:
                    subdir, flags = {
                        'SDL3': ('', ['-DSDL_TESTS=OFF', '-DSDL_TEST_LIBRARY=OFF']),
                        'xxhash': ('cmake_unofficial', ['-DXXHASH_BUILD_XXHSUM=OFF']),
                        'protobuf': ('', ['-Dprotobuf_BUILD_TESTS=OFF', '-Dprotobuf_WITH_ZLIB=OFF']),
                        'epoll-shim': ('', ['-DBUILD_TESTING=OFF']),
                        'miniupnpc': ('', ['-DUPNPC_BUILD_TESTS=OFF', '-DUPNPC_BUILD_SAMPLE=OFF']),
                        'fmt': ('', ['-DFMT_TEST=OFF', '-DFMT_DOC=OFF']),
                        'zycore': ('', []),
                        # Zydis links Zycore by its plain name, which carries no include path.
                        'zydis': ('', ['-DZYAN_SYSTEM_ZYCORE=ON', '-DZYDIS_BUILD_TOOLS=OFF',
                                       '-DZYDIS_BUILD_EXAMPLES=OFF', f'-DCMAKE_PREFIX_PATH={prefix}',
                                       f'-DCMAKE_C_FLAGS=-I{prefix}/include']),
                    }[name]
                    build_name = {'SDL3': 'sdl', 'epoll-shim': 'epoll'}.get(name, name)
                    build = deps / f'{build_name}-build'
                    run(['cmake', '-S', source / subdir, '-B', build, '-G', 'Ninja',
                         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_OSX_ARCHITECTURES=x86_64',
                         f'-DCMAKE_INSTALL_PREFIX={prefix}', *flags])
                    run(['cmake', '--build', build, '-j', args.jobs])
                    run(['cmake', '--install', build])
                    if name in ('fmt', 'zycore', 'zydis'):
                        licenses = prefix / 'share/licenses' / name
                        licenses.mkdir(parents=True, exist_ok=True)
                        for license in source.glob('LICENSE*'):
                            (licenses / 'LICENSE').write_bytes(license.read_bytes())
            except subprocess.CalledProcessError as error:
                print(''.join(log_path.read_text(errors='replace').splitlines(True)[-40:]), flush=True)
                raise RuntimeError(f'{name} failed; see {log_path}') from error
        stamp.write_text(digest + '\n')
    if not args.runtime_only:
        header = prefix / 'include/vma/vk_mem_alloc.h'
        header.parent.mkdir(parents=True, exist_ok=True)
        download('https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.3.0/include/vk_mem_alloc.h',
                 header, '90ce12fc4a2466235a09ae02905dd0c13aee80c1bbf11b331ab61230c2ceb112')
        url, digest, member = MOLTENVK
        library = prefix / 'lib/libMoltenVK.dylib'
        if not library.exists():
            archive = deps / 'MoltenVK-macos.tar'
            download(url, archive, digest)
            with tarfile.open(archive) as package:
                library.write_bytes(package.extractfile(member).read())
                licenses = prefix / 'share/licenses/molten-vk'
                licenses.mkdir(parents=True, exist_ok=True)
                (licenses / 'LICENSE').write_bytes(package.extractfile('MoltenVK/LICENSE').read())
    print(f'Dependencies ready: {prefix}')


if __name__ == '__main__':
    main()
