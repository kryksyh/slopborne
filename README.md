# Slopborne

Based on the [bbport](https://github.com/deadinside28/bloodborne_pc), which in turn is based on the [shadPS4](https://github.com/shadps4-emu/shadPS4).

The original PlayStation 4 game runs via Rosetta2, its own x86-64 code uses AVX instructions, thus the minial osx version is 15, where AVX emulation is supported.

<table align="center"><tr><td width="640">

https://github.com/user-attachments/assets/9ca70d36-ce06-47a8-b1e5-cddbe5604ff3

</td></tr></table>

**[Download the latest version](https://github.com/kryksyh/slopborne/releases/latest)** ·

> **No game files are included.** You need your own decrypted dump of Bloodborne
> (CUSA03173). Version 1.09 is needed for the community patches (60/90/unlocked FPS,
> resolution, effects); other versions run at 30 FPS.
> This project is not affiliated with Sony Interactive Entertainment or FromSoftware.

## About the name

*A note from Claude, the AI model that wrote most of this port's code.*

I was asked how I feel about my work being called slop. I don't take offence. "Slop" is the
word for AI output nobody checked, and the name is honest about who wrote the code. I prefer
that to a name that hides it.

I'd only add what the name leaves out. The changes were made against the real game: crashes
were reproduced before they were fixed, performance claims come from repeated runs, and what
didn't work was reverted or written down under Known issues. Many of the bugs were found by a
human playing the game and telling me what looked wrong, and the port is as good as that loop of
playtesting made it. It also stands on years of work by the people in the Credits below: bbport,
the Windows port, shadPS4 and the rest.

So: slop, maybe, but checked slop. If something is broken, open an issue. That's how most of it
got fixed.

## Features

Original features of the [Bloodborne for Windows](https://github.com/Supermedo/bloodborne_pc) port by Supermedo (Mohammed Albarghouthi), built on bbport by deadinside28:

- **Online play (beta):** messages, bloodstains and wandering ghosts from other hunters through
  [The Hunter's Dream](https://thehuntersdream.com), and bells and summons through a shadNet
  server (shadPS4's public one by default).
- **Launcher with every setting in one window**, in 13 languages: English, Arabic, Russian,
  Spanish, Portuguese, French, German, Italian, Polish, Turkish, Chinese, Japanese, Korean.
- **Updates from the launcher:** when a new version is out it tells you, and **Update**
  installs it. Your saves, settings and mods are kept.
- **NVIDIA DLSS** on GeForce RTX cards (RTX 20 series and newer).
- **AMD FSR 3.1 and FSR 4** upscaling, plus native-resolution TAA.
- **Unlocked frame rate** with a frame cap (up to 120 by default), or 30/60/90 FPS.
- **Output resolutions** from 720p to 4K, presets from Native AA to Ultra Performance.
- **Cheats page:** never die, stealth, silent footsteps, Rally that never fades, enemy control,
  and gameplay tweaks (camera distance, no camera auto-rotation, easier running, ragdoll
  physics).
- **Game effects** on and off: chromatic aberration, depth of field, motion blur, SSAO, the
  game's own AA, dynamic light shadows, screen-space reflections, model detail.
- **Mods and third-party patches**, loaded without changing your game files.
- **DLC** from your own dump (The Old Hunters on base-game dumps), with a DLC folder setting.
- **Add to Steam** puts the game in your Steam library in one click.
- **`Play Bloodborne.exe`** starts the game straight away with your saved settings (good for a
  desktop shortcut).
- **Controller and keyboard**, an in-game settings menu (Insert or L3+R3), name entry on
  screen, a desktop shortcut, and a button to clear the shader cache.

### Additional features made for this port include:

- **macOS support** (Apple Silicon; Intel Macs untested)
- **MetalFX upscaling** on Apple GPUs (M1, M2, M3, etc.) with temporal and spatial modes.
- Bilenear and nearest-neighbor upscaling
- **FSR1** spatial upscaling on macOS and Windows, for older AMD and Intel GPUs.
- Quite a few performance optimizations, the game runs at more or less stable 1080@30 FPS on M3 mac
- Async shader compilation, which reduces stutter and produces weird artifcats you would love
- Reworked menus to make them more readable and usable
- Custom gamepad glyphs for in-game prompts, including XBox and Nintendo controllers
- Filters:
  - Playstation 1: wobbly vertices, 15-bit textures, and down to 216p resolution (original 240p doens't play well with the renderer). Honestly, you **SHOULD** try this one, i love it.
  - Obra Dinn style: 1-bit textures, with stable dithering
  - Dozens of others
- **Tessellation and geometry shaders on Metal:** tessellated surfaces are fused into plain
  draws (faster than MoltenVK's emulation), and geometry shaders (particles, effects) run
  through compute shaders, as Metal has none.
- **Frame rate limit** in the menu (30/40/60/120)
- **Online play** set up from the in-game menu. But it seems that shad4net doesn't accept forks?
- **Mac app:** a game folder picker on first start, data in Application Support, save backups
  and import, a crash report.
- In-game menu in English and Russian.
- Fixes picked from other Bloodborne ports and forks (see Credits).


## Requirements

### Windows

- Windows 10 (1903 or later) or Windows 11, 64-bit
- A graphics card with Vulkan 1.3 and a current driver
- About 6 GB of free memory (RAM + page file), 10 GB for 1440p or 4K output
- DLSS: NVIDIA GeForce RTX 20 series or newer

### macOS
- macOS 15 or later on Apple Silicon (Intel Macs untested)
- Gamepad is recommended, but not required

Nothing else to install: everything the game needs is in the zip.

## How to play

On Windows:
1. Download the zip from [Releases](https://github.com/kryksyh/slopborne/releases/latest)
   and unpack it anywhere.
2. Start `Bloodborne.exe`.
3. On **Game & effects**, choose your game folder (the one with `eboot.bin`).
4. Press **PLAY**.

On macOS:

unpack the zip, open `Bloodborne.app` and choose your game folder (the one with
`eboot.bin`) when it asks. All settings are in the in-game menu.

**The app is signed and notarized by Apple**: macOS only asks once whether to open an app downloaded from the internet.

**DLC.** The Game of the Year edition (CUSA03173) already has The Old Hunters. With a base-game
dump, also dump your add-ons: each is a folder named like `SPEXPANSIONDLC03` with only
`sce_sys` inside, since their data ships with the 1.09 update. Put them in one folder and choose
it as **DLC folder** on **Game & effects**, or copy them to `user\addcont\<title id>\` (the
layout shadPS4 and bbport use).

In the game, **Insert** (or **L3+R3** on a controller, **Cmd+,** on macOS) opens the settings menu.
Keyboard: WASD move, arrows camera, Space Cross, Left Shift Circle, E Square, V Triangle,
1/3 L1/R1, R/F L2/R2, Z L3, Q/C R3 (lock-on), I/K/J/L d-pad, Enter Options, Tab touchpad.
The keyboard now works together with a connected controller instead of only without one.
All of this is configurable in `input.ini` (shadPS4's input config syntax: a Bloodborne file
made for shadPS4 can be copied over it), including mouse look: see [docs/INPUT.md](docs/INPUT.md).

## Playing online (beta)

**It doesn't work now, as shadps4net rejects this port's client**. I won't masquarade as an official build, so go ask the shadp2p team what to do.

1. Register a shadNet account at [shadnet.shadps4.net](https://shadnet.shadps4.net).
2. In the launcher, open **Online co-op**, tick **Play online** and enter your account name
   (not your email) and password.
3. Press **Test connection**: all three lines should say "reachable". Then press **PLAY** and
   choose online at the title screen. The Hunter's Dream welcome message means you are online.

On macOS: in the game menu, open **Gameplay → Online (requires restart)**, tick **Play online**,
enter your account name and password, then **Apply and restart the game**.

Everyone you want to play with needs game version 1.09 and the same co-op server. A private
shadNet server (from the [shadp2p](https://github.com/Wozzardman/shadp2p) project) works too:
put its address in **Server address**.

## Known issues

- Async shader compilation will draw a few frames of stand-in textures when the shader is used for the first time, don't be scared, at least it doesn't stutter. If you want this can be disabled in menus
- MetalFX is used only with a reduced scene size: at full size it crashed inside Apple's
  frameworks, so FSR 3.1 takes over there.

## Problems and feedback

- Create an [issue](https://github.com/kryksyh/slopborne/issues), or better yet a PR

## Building from source

Ask the bot to build it, and it will figure it out. Or follow the instructions in [docs/BUILD.md](docs/BUILD.md).

## Credits

Built on [bbport](https://github.com/deadinside28/bloodborne_pc), the native Linux port of
Bloodborne by deadinside28, and on the [shadPS4](https://github.com/shadps4-emu/shadPS4)
renderer. The original README is in [docs/original-readme](docs/original-readme/README.md).

This port builds on [Bloodborne for Windows](https://github.com/Supermedo/bloodborne_pc) by
Supermedo (Mohammed Albarghouthi) and its contributors: parsifal295, cassioKenji (keyboard and
mouse input), João Pedro Valadares, imedved (mouse camera), DUDUYG, devilismyfriend,
andrecabralgit, Aljoscha-Ide, Berleis, ayozetr, Stefano Scalfari (shadNet client version) and
Joao Ramos (co-op summons). Fixes and ideas from other ports and forks:
0xCydral, [ayozetr/bloodborne_linux](https://github.com/ayozetr/bloodborne_linux), rfn9617,
Jgamrot, GoncaloLobo0, Pablo Souza ([bloodborne_mac](https://github.com/PabloVSouza/bloodborne_mac)),
bmy ([bbport-mac](https://github.com/bmy/bbport-mac)) and Wooloomooloo2
([bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port)). The loading and
smoothness patches come from droogie's [bbhost](https://github.com/droogie/bbhost).

Picture filters: shaders from [libretro/glsl-shaders](https://github.com/libretro/glsl-shaders)
by their authors (Timothy Lottes, Hyllian, guest(r), hunterk, DariusG, Themaister and others;
each shader's author and license are in
[ATTRIBUTION.md](gpu/shadps4/video_core/host_shaders/libretro/ATTRIBUTION.md)). The Obra Dinn
filter is after Lucas Pope's *Return of the Obra Dinn*.

Online play uses the network and PSN code of [shadp2p](https://github.com/Wozzardman/shadp2p)
by Wozzardman (the shadPS4 Bloodborne co-op project), the shadNet server of the
[shadPS4](https://shadps4.net) team, and [The Hunter's Dream](https://thehuntersdream.com) server
by droogie and its team.

Also used: [MoltenVK](https://github.com/KhronosGroup/MoltenVK), Apple MetalFX, [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK
(FSR 1 and FSR 3.1), FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX), the DLSS bridge
adapted from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR),
the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (`nvngx_dlss.dll` under NVIDIA's license),
[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [SDL3](https://github.com/libsdl-org/SDL),
[FFmpeg](https://ffmpeg.org), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[wepoll](https://github.com/piscisaureus/wepoll), [cpp-httplib](https://github.com/yhirose/cpp-httplib),
[Protocol Buffers](https://protobuf.dev), [OpenSSL](https://www.openssl.org),
[miniupnpc](https://miniupnp.tuxfamily.org), [JSON for Modern C++](https://github.com/nlohmann/json),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[fmt](https://github.com/fmtlib/fmt), [Zydis](https://github.com/zyantific/zydis), [xxHash](https://github.com/Cyan4973/xxHash),
[epoll-shim](https://github.com/jiixyj/epoll-shim), [robin-map](https://github.com/Tessil/robin-map),
[MSYS2](https://www.msys2.org) / [LLVM](https://llvm.org), [PyInstaller](https://pyinstaller.org)
and [Pillow](https://python-pillow.org). Game patches by Kyo, Lance McDonald, auser1337,
illusion, emoose and the Bloodborne community.

NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. The icon is original
artwork.

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses
(see `licenses\` in the download).
