# Experimental macOS port

The full loader, HLE runtime, Vulkan renderer, FSR providers, audio/video decoders,
and network libraries compile into an **x86-64 macOS executable**. Apple Silicon
runs the executable and original x86-64 guest instructions through Rosetta.
This is not an ARM64 recompilation of the game.

**Status: builds and reaches saved gameplay. Native 1080p Hunter's Dream sampled
33–34 FPS in the initial view and 31.5–36.6 FPS during a short camera/idle test
after optimizing strict Metal shader arithmetic. This is not a stability guarantee.**
Graphics artifacts and GPU faults remain under investigation. All stock effects
were enabled in that comparison, but unsupported geometry-shader draws are still
skipped; enabled settings do not establish complete rendering compatibility.
The tested M3 Pro with MoltenVK 1.4.2 lacks `robustBufferAccess2` and
`nullDescriptor`. The renderer automatically uses shader bounds checks, shader
vertex pulling, and explicit resource-presence data with valid placeholder
descriptors on devices missing either feature.

MoltenVK uses allocated descriptor sets for graphics and compute. With push
descriptors, the game's gamma lookup compute pass produced an all-zero texture,
turning the final display black even though the offscreen UI rendered correctly.
GPU readback now verifies the populated lookup table and visible title screen.
Host resource-presence metadata is also excluded from the guest-buffer count
used to recognize image-copy and image-clear shaders.

Startup reaches audio, physics, save/load initialization and gameplay without
the previous thread deadlock or sparse-buffer assertion. Some optional guest file
lookups still report missing files. Title-screen FPS is not used as gameplay evidence.

## Build

Tested with macOS 15.7.5, Xcode 16.4, CMake 4.1, and Rosetta on an M3 Pro.
The build targets macOS 15 or newer. Intel Macs have not been tested.

Install host tools and architecture-independent headers with your normal Homebrew:

```sh
brew install cmake ninja pkgconf python@3.13 boost vulkan-headers glslang spirv-tools
```

The linked Homebrew libraries need an **Intel** installation. On Apple Silicon,
use Intel Homebrew in `/usr/local` (separate from `/opt/homebrew`):

```sh
arch -x86_64 /usr/local/bin/brew install molten-vk fmt robin-map zydis nlohmann-json
```

SDL, FFmpeg, Protobuf, OpenSSL, xxHash, epoll-shim and miniupnpc are built from pinned,
checksum-verified source archives into `out/macos/prefix`. This avoids depending
on Intel bottles that are no longer available for several packages. VMA is a
pinned header. Python 3.12+ is required by the bootstrap script.

```sh
git submodule update --init --recursive
bash build.sh --bootstrap --test
```

Subsequent builds use:

```sh
bash build.sh --test
```

Build products stay separate from Windows/Linux output:

- `out/macos/gpu/bb-probe`: full executable.
- `out/macos/gpu/bb-gpu-capabilities`: Vulkan device/scene-scaling diagnostic.
- `out/macos/runtime/bb-probe-cpu`: CPU-only diagnostic with an explicit no-renderer backend.

`BB_JOBS` controls build concurrency (default 6). `BB_MACOS_HOST_PREFIX` and
`BB_MACOS_INTEL_PREFIX` override the host Homebrew and Intel library prefixes.
`BB_LTO=ON` opts into LTO; it is off by default for faster iteration.
`bash build.sh --performance` builds a separate Release (`-O3`) + ThinLTO binary
at `out/macos/gpu-performance/bb-probe`, preserving the development build.
Combine it with `--test` for the same regression checks.
Select that executable when launching with
`BB_PROBE="$PWD/out/macos/gpu-performance/bb-probe" bash packaging/macos/run.sh`.
Build logs are under `out/macos/`. The executable currently depends on this
workspace's dependency prefix and Intel Homebrew; it is not a distributable app bundle.

For just the runtime tests, without renderer dependencies:

```sh
bash build.sh --bootstrap --runtime-only --test
```

## Validation

```sh
out/macos/gpu/bb-probe --vulkan-only
out/macos/gpu/bb-gpu-capabilities --renderer-requirements
out/macos/gpu/bb-probe --renderer-only
```

`--vulkan-only` verifies actual GPU command submission and a 4096-byte readback.
`--renderer-requirements` reports native extension/feature availability and whether
the robustness fallback is needed, without a window or game files. Passing this
check does not validate gameplay.
`--renderer-only` creates a Cocoa/Metal window and initializes the real renderer
without game files. It succeeds on the test machine with the automatic fallback.

`bash build.sh --test` runs nine CTest groups (runtime, synchronization, controller,
files/content, direct executable guest TLS loads, and Python contracts), then
Vulkan readback, production shader-backend GPU tests with hardware robustness
disabled, SPIR-V validation, and 14 synthetic loader tests against the **full** executable.
The shader tests check buffer bounds (8/16/32/64-bit and float), partial vectors,
32-bit atomics, null buffers/images, address overflow, discarded image writes,
null vertex tables, 64-lane shared-memory exchange on a 32-lane GPU, and DMA page
table bounds including 64-bit page-index overflow.
They do not validate every game shader, format, or vertex-fetch program.
The host-thread test reproduces concurrent movie EOF/stop joins, self-joins, and
thread reuse. Movie workers retain their ownership until an external caller joins
them; cancellation remains available while another caller waits for completion.
This fixes the observed intro shutdown deadlock on macOS.
The committed-arena GPU test checks overlapping allocations, repeated binding
relocation, preservation of GPU writes, and resident/nonresident BDA table entries.
The PM4 packet-reuse test overwrites active graphics, indirect-parent and constant-
engine packet headers while executing them. The decoder keeps each packet's original
length and copies write-command data before publishing writes that allow guest reuse.
Metal API validation also exposed mismatched depth/stencil pipeline formats when
tests were disabled, and a transient presentation-view lifetime problem. Pipeline
formats now follow the bound attachment state; display views are cached with their
images. The GPU profiler uses separate 4096-query pools for its in-flight frames
to fit Metal's counter sample buffer limit. The presentation semaphore wait covers
transfer operations as well as color attachment writes. MoltenVK enables the
existing wave64 shared-memory barrier pass to synchronize across Apple subgroups.
On MoltenVK, attachmentless draws without a fragment shader discard rasterization
and use a 1x1 render area. Vertex-stage buffer writes still execute, while Metal
avoids building a maximum-size tile grid for passes with no raster output.

The optional `avplayer-stop-test` target decodes a supplied movie and stops it
five times using the production decoder, without creating a Vulkan device:

```sh
cmake --build out/macos/gpu --target avplayer-stop-test
out/macos/gpu/avplayer-stop-test /path/to/sprj_opening.mp4
```

The CPU-only backend does not emulate missing GPU/network/kernel-event imports;
those remain unresolved traps.

Once your download is complete, the existing image preparation pipeline can be
invoked with your decrypted game folder:

```sh
BB_GAME_DIR="/path/to/game" bash packaging/macos/run.sh
```

`--cpu-only` can be passed to that command for guest startup investigation.
No game files are downloaded by these scripts.
The runtime rejects writes, unlink and rename on the game mounts. Normal launches
allow generated shader caches to publish files with atomic rename. The optional
`BB_FILE_SANDBOX=1` additionally makes the game folder and Downloads read-only at
the OS level and blocks deletion everywhere; it also prevents cache publication.
Generated images, settings and saves default to `out/macos/run-data` (override
with `BB_DATA_DIR`, outside the protected source folder). Mod views are retained.
Incompatible shader caches are retained in sibling `.retained-*` directories on
macOS rather than removed. Operations that require deleting save files fail under
the optional sandbox.
For a non-deleting GPU readback diagnostic, set `BB_FRAME_DUMP_AT=1500` and
`BB_DUMP_DIR` to an existing workspace directory. The numbered frame (counting
from one) is written as raw 32-bit display/output images. Unlike the older
`BB_FRAME_DUMP_TRIGGER` mechanism, this does not consume or remove a trigger file.
`BB_FRAME_DUMP_SEQUENCE=/path/to/file` captures whenever the integer in that file
changes, allowing repeated captures without deletion. `game-control.py` provides
scripted controller presses, camera sweeps, and PNG conversion of these readbacks.
`BB_DUMP_IMAGE_ADDR=0xADDRESS,0xADDRESS` also captures the first layer of selected
32-bit color images from the texture cache at that frame, without refreshing them
from guest memory. `BB_CAPTURE_AT` records a numbered frame's render passes and
resource inputs in `BB_CAPTURE_DIR`, also without a trigger file.

`BB_EMULATE_ROBUSTNESS=1` forces the fallback for comparison on capable drivers;
`=0` requires the native features and therefore fails on the tested MoltenVK.
The fallback uses one additional storage-buffer descriptor per shader stage and
disables parallel texture binding. Its performance cost has not been measured.

## Port details and remaining work

- Darwin pthread TSD stores guest TCBs without overwriting the host GS base. Guest
  GS loads are patched to the allocated key offset, and the layout is checked at runtime.
- Guest thread handles also stay below 1 TiB: the game's lock-owner field stores
  only 40 bits. Ordinary Darwin heap pointers caused a startup self-deadlock.
- Devices without sparse-buffer support use committed 32 MiB buffer arenas.
  Overlapping requests merge whole arenas, preserve GPU contents, relocate BDA
  entries, and repair pending shader, vertex, index, indirect and copy bindings.
  Old allocations remain alive for recorded commands; this costs extra GPU memory.
- Low allocations use collision-safe Mach VM allocation. Guest direct memory uses
  an unlinked POSIX shared-memory object, preserving aliasing and zero-on-reuse.
- Guest mappings start at 448 GiB to avoid Rosetta's reservation, and remain below
  the PS4's 40-bit pointer limit. Fixed mappings cannot overwrite host/Rosetta memory.
- Timed locks, monotonic semaphore waits, clocks, random bytes, Darwin signal
  registers, Mach-O assembly symbols, and embedded font data have macOS paths.
- Cocoa creation and SDL event pumping run on the process main thread; the guest
  loader runs on a worker. MoltenVK is linked and loaded directly.
- Linux-only GPU per-thread scheduling statistics are disabled on macOS. The macOS
  watchdog reports the interrupted thread rather than enumerating `/proc` threads.
- Remaining: real-game shader/format compatibility, CPU/TLS and memory validation,
  video/presentation validation, launcher integration, and relocatable app packaging.

### Automated gameplay benchmarks

`benchmark-run.py` copies a prepared save into a unique run directory, presses
through the title menus to Continue, waits for two gameplay intervals without
shader compilation, sweeps the camera in both directions, and retains frame
captures, a JSON performance report, and the complete startup log. It uses the
runtime's scripted controller input, so desktop focus is not required. The source
save and downloaded game are not modified by the test. The default 250-draw
threshold is specific to the current Bloodborne gameplay workload; captures must
still be checked to distinguish gameplay from unexpected screens.

```sh
python3 packaging/macos/benchmark-run.py \
  --prepared out/macos/launch-20261010-211141/out \
  --profile out/macos/performance-720p \
  --game /path/to/Game --save /path/to/user \
  --seconds 90 --move-seconds 5 \
  --env BB_RENDER_RES=1280x720 --env BB_OUTPUT_RES=1280x720 \
  --env MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0 \
  --env MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=1
```

Use `--env KEY=VALUE` for an A/B run, and `--stop-after` to terminate only the test
process after measurement. `--wait-for-exit` keeps the launcher attached with the
game open after measurement. `--move-seconds 5` adds five seconds of forward and
backward movement using the copied save. Failed runs also retain a JSON report.
All outputs remain under `out/macos/benchmarks`.
On macOS the launcher also samples whole-process resident memory, physical
footprint, wired memory and disk I/O once per second into `process-metrics.jsonl`.
`process-metrics-summary.json` reports peaks and the memory trend in the latter
half of the run. These are process counters, not a separate Metal allocation
counter; growing asset caches during loading are not by themselves proof of a
leak. Compare repeated routes and idle periods after warm-up. Sampling continues
through `--wait-for-exit` and does not synchronize with the GPU.
Full Metal shader validation can make pipeline warm-up take several minutes;
validation runs should not be compared with normal performance runs.

### GPU audit findings

The 2026-10-11 native-1080p comparison identified a large shader translation cost.
SPIRV-Cross translates strict add/multiply/subtract operations into `optnone`
helpers on Metal. A sampled lighting shader had 507 such calls in optimized Metal
IR. The MoltenVK path now emits the same explicit FMA operations as those helpers,
preserving each rounding boundary while allowing native shader optimization.
Safe floating-point mode remains enabled; this does not turn on fast math.
Set `BB_METAL_INLINE_ARITHMETIC=0` to restore the previous path for an A/B test.

Using one executable, the same copied save, native 1920x1080, stock LOD, no
upscaling, all stock effect settings, and a 60 FPS measurement cap:

| Arithmetic path | Initial warm view | Camera/idle result |
| --- | --- | --- |
| Original helpers | About 14 FPS, about 70 ms/frame | Earlier baselines faulted during the camera sweep. |
| Native FMA equivalent | 33.3–34.0 FPS, about 29 ms/frame | 20-second camera sweep plus 20-second idle passed; 31.5–36.6 FPS windows. |

Production-backend GPU readback compares 482 numerical cases (2,410 results),
including signed zero, infinities, NaNs, denormals and separate/fused rounding,
with Metal API and shader validation enabled. NaN payload differences are allowed.
An isolated native arithmetic kernel also matched 262,144 values and measured
0.245 ms versus 0.039 ms; that kernel ratio is not the game's speedup.
Evidence: `out/macos/optimization-20261011/performance-analysis.json` and runs
`20261011-004712-003448` (optimized), `20261011-004906-201545` (repeat baseline).

The native Metal trace also found 209 render encoders and 90 buffer-copy encoders
in one frame. These counts justify investigating pass splits and redundant
transfers, but do not prove any particular barrier or copy can be removed.
`summarize-metal-trace.py` summarizes xctrace XML exports using interval unions
so overlapping GPU channels are not double-counted.
The separate `BB_BOUNDED_BUFFER_READS=1` experiment did not show a clear gain and
remains disabled by default.

| Path | Finding and action |
| --- | --- |
| Attachmentless rendering | Thirteen passes used a 16384×16384 render area despite having no raster output. Preserve vertex work, discard rasterization and use 1×1 on MoltenVK. Measured aggregate cost fell from about 13 ms to under 1 ms/frame. |
| Presentation | Skip the swapchain clear when the following blit covers the whole image. Preserve clearing for letterboxing. Cache display views with their images and wait for producer completion before transfer reads. |
| Descriptor binding | In the tested heavy clinic view, direct Metal bindings gave roughly 28.5 FPS versus 24.8 FPS with argument buffers. `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` plus `MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=1` is the tested performance configuration; the second option retains referenced resources through command completion on this path. |
| Pipeline startup | Create the Vulkan pipeline cache before warm-up so warm-up uses it. Permit atomic cache publication during normal launches. Keep old cache generations when shader code/profile changes. |
| Guest shader compatibility | Guard null vertex tables; bound internal DMA page/fault buffers; enable the existing shared-memory ordering workaround for Apple’s 32-lane subgroups. Hardware readback tests cover these paths. |
| Tiny buffer copies | Already batched into compute dispatches, about 1.6–1.9 ms/frame in the profiled scene. Replacing each range with a native blit would create thousands of commands. Threadgroup-size tuning needs a separate A/B measurement. |
| Render-pass reuse | Identical render state is already memoized. Real memory dependencies still break passes. Further tile-memory savings require proving which attachments can avoid load/store across those dependencies. |
| Upscaling | FSR is disabled in the measured 720p profile; no FSR dispatch should be counted as removable work in this configuration. |

A direct-to-swapchain final composite could remove the remaining intermediate
presentation blit, but requires changing image ownership and overlay ordering.
That rewrite is not included. Geometry-shader compatibility and visible shading
artifacts also remain. Passing a clinic camera test does not validate other areas,
combat, all shader paths, or a native ARM64/Metal backend.

The experimental `geometry-probe` target compiles a captured StandardParticle
geometry shader offline. Adding `--compute` lowers point inputs to a compute
dispatch, preserving output attributes and strip boundaries in a bounded storage
buffer. Both forms of the captured shader pass Vulkan 1.3 SPIR-V validation.
`robustness-shader-test --geometry-only` passed six GPU readback cases on the M3
Pro with Metal API/shader validation enabled: attributes, strip boundaries,
overflow, short input/output buffers, and inactive invocations.
`--geometry-compile-only new-output.spv` generates the test shader without a GPU.
`geometry-probe ... new-output.spv --replay` emits the matching triangle-list
vertex shader. It clips incomplete triangles and triangles crossing strip cuts,
and restarts winding at each new strip. `geometry-replay-test` passed vertex-output
readback on the M3 Pro with Metal validation, including odd-length preceding
strips and short input/output buffers. This validates generated vertex attributes;
it does not replace an in-game image comparison.
`geometry-probe ... new-output.spv --export` lowers the captured particle input
shader to compute too. The draw header supplies indices, base vertex and instance
values; output uses the bounded ES-to-GS layout. It passes SPIR-V validation.
`robustness-shader-test --export-only` passed 14 GPU readback cases with Metal
validation, covering indices, instances, base offsets, overflow and short buffers;

`--export-compile-only new-output.spv` emits its test shader without a GPU.
GS input headers carry points per instance so primitive IDs restart correctly.
Inactive GS records clear their counts before returning, preventing stale replay.
Replay supports either provoking-vertex convention; the caller must match its
pipeline state. `geometry-replay-test --first` and its default last-vertex mode
both passed three GPU readback cases. The six GS cases also passed again with
instance-ID and retired-record checks.
This compiler path is not yet connected to game draws: ES/GS dispatch submission,
replay draw submission and resource lifetime/barrier integration remain.
Unsupported geometry effects therefore remain a correctness gap in the game.

For targeted buffer-coherence investigation, set `BB_CAPTURE_GPU_BUFFER_HASH` to
one shader's hexadecimal hash and `BB_CAPTURE_GPU_BUFFER_DIR` to a new output
directory, then request frames through `BB_CAPTURE_SEQUENCE`. The diagnostic
captures up to 512 read-only bindings, at most 64 KiB each. Each unique directory
contains `gpu.bin`, `guest.bin`, metadata, and `staged.bin` for constant-ring
bindings. The existing text capture for constant-ring bindings shows staged
bytes, not live guest memory. Copies complete asynchronously before CPU readback;
extra copy/barrier work means these runs are not performance evidence. A
GPU/guest mismatch can be legitimate for GPU-generated inputs or guest memory
reused after a draw was prepared, so compare the staged bytes and the bad/good
frames before attributing a coherence fault.

`BB_CAPTURE_DRAW_HASH=<hex VS or PS hash>` adds the exact push-constant bytes,
resolved descriptor handles/ranges/layouts, and direct-draw/index arguments to
matching draws in text frame captures. Unlike the normal first-four-draw buffer
summary, this records every matching draw. Handle values only compare within
one process; constant-ring offsets normally change between frames. With the
GPU-buffer diagnostic selecting the vertex shader, indexed draws also capture
their resolved index range as slot `4294967295` (at most 64 KiB). This readback
adds transfers and barriers; use it for correctness investigation, not timing.

`BB_PM4_FAULT_DIR=/absolute/new/directory` records malformed graphics-command
buffers before the existing fatal check, including unsupported type 0/1 headers
and truncated type 3 packets. Each unique subdirectory contains
`initial.bin`, `current.bin`, and metadata identifying the failing word offset,
fault reason and observed header. Use `analyze-pm4-fault.py` to compare them.
For prepared top-level submissions, the initial bytes come from the immutable
submit-time copy; otherwise they come from the start of processing (up to 16 MiB).
The command processor continues to execute the original guest memory, so this
diagnostic does not hide a packet-lifetime bug by switching to the snapshot.

`robustness-shader-test --specialization-only` runs CPU-only checks that missing
textures retain their shader interface dimensions, scalar types, and descriptor
binding locations. A missing texture receives a real placeholder on Metal, so
it cannot reuse a shader expecting an incompatible texture type. Shader metadata
version 10 rejects older incomplete null-image specialization records; existing
cache files are retained.

`geometry-pipeline-test` runs lowered ES and GS kernels through the production
Vulkan pipeline, descriptor allocation and deferred recorder, checking separate
guest resources, appended input/output buffers and ES-to-GS memory ordering.
`geometry-replay-test --binding-offset` checks replay at a nonzero descriptor
binding, leaving room for fragment resources; add `--first` to check first-vertex
provoking semantics. These are integration regressions, not game activation.

Geometry shaders (MoltenVK has none) run as compute: separate ES/GS compute
specializations write the expanded primitives, and a replay vertex shader draws them
with the guest fragment shader. On by default; `BB_GEOMETRY_COMPUTE=0` skips these
draws as before. Covered: point inputs, triangle-strip output, one GS invocation, no
streamout or user clip planes; direct draws (non-indexed or 32-bit indexed) and
non-indexed single indirect draws. The game's particles (Hunter's Dream mist, the
StandardParticle GS) are GPU-written indirect draws: a one-thread compute turns their
arguments into the ES/GS dispatch and replay draw arguments, up to 1024 points per
draw, with no CPU readback. Per-vertex fragment inputs (MSL has no `PerVertexKHR`)
become three flat corner inputs that the replay writes. Unsupported cases are skipped.
`BB_GEOMETRY_INDIRECT_READBACK=1` adds a diagnostic bridge for indexed/multi-draw
indirect geometry that reads the arguments back (a GPU wait per draw). Tests:
`geometry-replay-test --first-hardware --negative-one-to-one --binding-offset`,
`geometry-pipeline-test`.

Camera/movement evidence (followed by a later GPU hang): `out/macos/benchmarks/20261010-224029-802245`
(log, JSON report and before/after captures). Worst frames in the sweep ranged
from 33.4 to 69.8 ms; the 30 FPS average does not imply every frame met 33.3 ms.

The 2026-10-10 late freeze was a Metal command-buffer GPU hang followed by an
unbounded semaphore retry loop. The retry loop now stops on terminal Vulkan
errors; this improves failure reporting and does not fix the originating GPU hang.
The production-semaphore regression test verifies timeout retries, successful
completion, and termination with a diagnostic on device loss.
Benchmark `--idle-seconds` adds monitoring after the input sequence, and launcher
process failures are recorded even when the input benchmark already completed.
Idle monitoring checks new renderer frame statistics and fails after 30 seconds
without progress, even if other threads continue writing to the log.

The follow-up run `out/macos/benchmarks/20261010-225252-427597` remained responsive
for over 13 minutes, including 540 seconds of automated idle monitoring, and
responded to camera input afterward. Its final five-minute idle interval sampled
30.0 FPS. Metal API validation was enabled, which can change timing; this single
diagnostic run does not establish that the normal-launch GPU hang is fixed.
