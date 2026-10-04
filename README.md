# ccu

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The chain is not
> asserted but measured: an offline harness drives the real plugin class in a
> headless GL context and reads each claim back out of the picture it made —
> every stage at its null returns the input within a bound derived from the
> OETF round trip in float, alpha bitwise, and again after a resize; a step's
> overshoot is Detail Level × h / 4 for exactly the delay, whole and fractional,
> horizontal and vertical; an edge below Coring gets no detail and one above it
> gets the overshoot less the dead zone; above the knee point a linear ramp's
> slope is Knee Slope, continuous at the point, per channel; a ramp follows
> the OETF at three exponents and Black Gamma lifts only below its level; a
> warm white through white balance and then the knee compresses in red where
> the other order would clip it; the detail gain inside the skin window is
> 1 − Skin Detail and outside it is 1 — with a negative control per check that
> proves each can fail, and one recorded mutation of the shipped GLSL. It has
> **never been loaded into Resolume**. It is loaded by
> [oxbow](https://github.com/stoatworks-labs/oxbow), which is a real FFGL host
> and is not Resolume. The OpenFX build renders the same pictures as the FFGL
> build byte for byte through the fleet's OFX test host, and in DaVinci
> Resolve Studio 21.1 on macOS it renders as a Fusion tool and matches that
> test host to within 1/255. It has **never been run in Vegas, Nuke or
> Natron**. See [Status](#status).

A broadcast camera's processing chain with every knob out, as an FFGL effect
for [Resolume](https://resolume.com) Arena and Avenue — and, from the same
source, as an OpenFX effect for DaVinci Resolve, Vegas, Nuke and Natron.

![The test card through the chain at its defaults: halos on the bars, the top of the grey ramp gone milky through the knee, the skin disc softened while the jacket's weave stays sharp](docs/hero.png)

<sub>One frame, rendered by `cctest`, the offline harness — not captured from
Resolume. The defaults. The halos on the bars are the width of the detail
delay, not of the edge; the top of the ramp and the warm sky have gone through
the knee; the hair on the skin disc is softer than the weave on the jacket
because of the skin window; the floor's fine texture is left alone by the
coring.</sub>

[![CCU — a broadcast camera's processing chain with every knob out, for Resolume](docs/video-thumb.png)](https://www.youtube.com/watch?v=w5i91PWg-c4)

*[Watch it](https://www.youtube.com/watch?v=w5i91PWg-c4) — 62 seconds:
Detail Level from 0 to 3 and the delay from 2 to 9 pixels, the Show Detail
view with Coring eating the texture, Skin Detail dropping bone and orange out
of the view and the window moved to cyan, the knee on, off, gained into a hard
clip and back on, a warm white compressed and then clipped in red first, Gamma
and Black Gamma, the four matrix presets, pedestal and white clip. Every frame
is the real plugin's output: an FFGL plugin has no window, so the footage is
rendered by this repository's own offline harness (`cctest --pipe`, driven by
a cue sheet) over Resolume's bundled demo clips, not captured from Resolume.*

<!-- downloads:start -->

## Download

**[v0.1.0](https://github.com/stoatworks-labs/ccu/releases/tag/v0.1.0)** — prebuilt for macOS and Windows. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`ccu-0.1.0-macos-universal.dmg`](https://github.com/stoatworks-labs/ccu/releases/download/v0.1.0/ccu-0.1.0-macos-universal.dmg) | 204 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`ccu-macos-universal.zip`](https://github.com/stoatworks-labs/ccu/releases/latest/download/ccu-macos-universal.zip) | 167 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`ccu-0.1.0-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/ccu/releases/download/v0.1.0/ccu-0.1.0-windows-x86_64-setup.exe) | 218 KB |
| x64 · .zip archive | [`ccu-windows-x86_64.zip`](https://github.com/stoatworks-labs/ccu/releases/latest/download/ccu-windows-x86_64.zip) | 111 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/ccu/releases](https://github.com/stoatworks-labs/ccu/releases).

macOS builds are signed and notarised and open normally. The Windows builds are unsigned, so SmartScreen warns once.

<!-- downloads:end -->

## The one idea

The "video" look of a studio or OB camera is not a filter. It is a
**processing chain in a fixed order**, each stage a known circuit with the
knob a shader on the CCU panel turned:

    linear light → white balance → matrix → detail → knee → gamma and black gamma → pedestal → white clip

Put the stages in the right order, in linear light, and label the knobs the
way a CCU labels them, and the badly set-up camera looks fall out rather
than being drawn:

- **Halos.** Detail boosts edges by adding a high-passed copy made from pixel
  and line delays, so a hard edge overshoots on one side and undershoots on
  the other, **for the width of the delay, not of the edge**.
- **Coring** keeps detail off the noise: small detail is not boosted at all,
  so a low coring sharpens the grain and a high one plastic-coats the picture.
- **Skin detail** suppresses detail inside a hue window, and faces go soft
  while the jacket stays sharp.
- **Knee.** Above the knee point highlights are compressed, so skies keep
  their colour. With the knee off they clip — and because the knee follows
  white balance, **a warm white clips in one channel first**.
- **Black gamma** lifts the shadows on a curve below its level and nowhere
  else.
- **Drifting white balance.** The R and B gains wander slowly, as a
  warming-up camera did.

Nothing carries across frames except the drift, a double on the CPU. The
chain is two GLSL passes and the constants — the OETF's, the matrix, the
drift's step — are computed in double once a frame. The OpenFX build runs the
same two passes on the CPU; see [OpenFX](#openfx--resolve-vegas-nuke-natron).

## Controls

| Group | | |
| --- | --- | --- |
| **Exposure** | Master Gain | head-end gain, −6 to +18 dB, in linear light; 0 dB at a quarter |
| | Master Black | pedestal on video, −0.05 to +0.15; 0 at a quarter |
| | White Clip | 0.85 to 1.15; exactly 1 at the middle |
| **White** | R Gain, B Gain | ±6 dB about green; 0 dB at the middle |
| | Drift | a slow random walk in mireds, 20 s time constant, up to 60 mireds RMS |
| **Matrix** | Matrix | Identity, Standard (SMPTE-C to BT.709), High Saturation, Film-like |
| | Saturation | 0 to 2 about BT.709 luma; exactly 1 at the middle |
| **Detail** | Detail Level | the gain on the detail signal, 0 to 3; a step of h overshoots by Level × h / 4 |
| | Crispening Freq | the delay, 1 to 9 pixels; whole at every eighth |
| | H/V Ratio | vertical only at 0, horizontal only at 1, both at the middle |
| | Coring | the edge height below which there is no detail at all, 0 to 0.25 linear |
| | Level Dependence | how far detail is reduced in the shadows |
| | Skin Detail | the detail gain inside the skin window is 1 − this |
| | Skin Hue, Skin Width | the window's centre (0 to 360°) and half-width (5 to 60°) |
| **Knee** | Knee On | |
| | Knee Point | 0.4 to 1.0 in linear light |
| | Knee Slope | 0.05 to 1 above it; continuous at the point; per channel |
| **Gamma** | Gamma | the OETF's exponent, 0.35 to 0.55; BT.709's 0.45 at the middle |
| | Black Gamma | a lift below 0.25 video |
| **Output** | Mix | |
| | Show Detail | the detail signal about mid grey, for setting up |

Every numeric control is 0..1 to the host; the conversions above live in
`Controls.cpp`, written so each null is exact in binary. The parameter names
are at most 16 characters, which is why it is `Crispening Freq`.

## OpenFX — Resolve, Vegas, Nuke, Natron

The same chain also builds as an OpenFX plugin, **CCU** in the **Stoatworks**
group (`com.stoatworks.ccu`), for DaVinci Resolve (Edit, Color and Fusion),
Vegas Pro, Nuke and Natron. It has the same 23 controls in the same seven
groups, with the same 0..1 ranges and the same defaults, and it renders the
same picture: everything between a slider and a uniform — the control laws,
the OETF's constants, the matrix, the drift's pull on the gains, the rounding
of each to float — is one function both builds call (`Chain.cpp`), and the
two GLSL passes are restated statement for statement on the CPU
(`CpuChain.cpp`) and held to the GPU per pixel by the harness.

The OpenFX build ships from v0.2.0: each release carries
`ccu-ofx-macos-universal.zip`, `ccu-ofx-windows-x86_64.zip` and
`ccu-ofx-linux-x86_64.zip`. Copy `CCU.ofx.bundle` from the zip into the
standard OpenFX folder and restart the host:

```
macOS    /Library/OFX/Plugins/
Windows  C:\Program Files\Common Files\OFX\Plugins\
Linux    /usr/OFX/Plugins/
```

To build it yourself it comes out of the normal build as
`build/CCU.ofx.bundle`; `-DCCU_BUILD_FFGL=OFF` builds it alone with nothing
but a compiler (no FFGL SDK, no GLEW), which is how the Linux release job
builds it on AlmaLinux 8 for Rocky 8's glibc.

**What differs from the Resolume build**, all of it:

- **Drift is a function of the frame number.** Resolume steps the white
  balance's random walk once per frame it is handed, from the moment the effect
  starts. An OpenFX host renders frames in any order, alone and on several
  threads at once, so here the walk is replayed — the same steps, the same
  seeded noise, the frame's duration from the clip's frame rate — up to the
  frame being rendered. A frame renders the same alone, in sequence, or twice;
  scrubbing shows the camera as it was at that frame. It starts from no drift
  at frame 0 and settles over the first minute, as the Resolume build does when
  the effect is added. The replay starts at most 800 s (40 time constants)
  back, where what it forgets weighs less than a double's rounding: it is
  bit-identical to the walk stepped from frame 0 at every frame the harness
  probed, up to an hour in, and costs 0.5 ms a frame at 60 fps wherever the
  timeline starts (a replay from frame 0 would cost 2.4 ms an hour in, and
  grow). **Fusion reports no frame rate; there, Drift assumes 24 fps** — still
  one fixed picture per frame number, stepped as if the timeline ran at 24.
- **Alpha.** The chain works on straight colour in both builds. A
  premultiplied clip is divided by its alpha on the way in and multiplied back
  on the way out; at alpha 1 — every camera clip — both are exact.
- **It runs on the CPU**, threaded through the host: about 15 ms a 1080p frame
  in a host lending it 8 threads of an M4 Max, against 0.17 ms for the GPU
  build. See Status.
- **The detail delay is in pixels of the image the host renders**, as it is in
  pixels of Resolume's composition. A host rendering a half-resolution proxy
  gets halos twice as wide relative to the frame.

Nothing is FFGL-only: the effect has no audio input, no host-beat control and
no event button, so every control carries over. There are no presets in
either build.

## Status

**v0.2.0, and honestly early.** v0.1.0 (released 2026-09-24) was the
Resolume build alone; v0.2.0 adds the OpenFX build and leaves the Resolume
build's pictures unchanged. Verified by
measurement on an M4 Max, macOS 26.4, at 320×180 and 1280×720, on a fresh
universal build. Never loaded into Resolume on macOS. The OpenFX build came
after v0.1.0 (2026-10-03); its rows below are `tools/verify.sh` on the same
machine, and every FFGL row was re-run unchanged after the settings code
moved into `Chain.cpp` for both builds to share.

| Check | Result |
| --- | --- |
| `--identity` | every stage at its null returns a three-ramp picture to **1.19e-7**, 0.07 of a tolerance derived from the OETF round trip in float (3.57e-6 at white); alpha bitwise; the same after a resize mid-run |
| `--detail` | a step of 0.3872 linear overshoots by **0.096795** against Level × h / 4 = 0.096795 and undershoots by the same, for exactly ceil( delay ) pixels at 2, 2.5, 3 and 1.5 px; the fractional pixel carries f × the peak; the view matches the kernel to 1.5e-8 and the output the chain to 6.6e-8 |
| `--coring` | an edge of 0.02 below Coring 0.04: **exactly zero** both sides (uncored, 0.005); an edge of 0.20: **0.040000** against Level × ( h − c ) / 4 |
| `--knee` | a linear ramp above the point at **0.24000** against 0.2400, below it at 1.00000, no column stepping more than the ramp, per channel (R compressed, G and B untouched to 6e-8), and the identity with the knee off |
| `--gamma` | OETF( gL ) at +6 dB per column to 1.7e-7 with the linear segment exercised; exponents 0.35 and 0.55 to 1.7e-7; Black Gamma to 1.2e-7, lifting up to the last column its bump can be seen on and not from 0.25 |
| `--order` | red at +6 dB through white balance then the knee is **0.831815** as predicted; the knee-first order would give 0.997004, **34 605 tolerances** away; with the knee off red clips to **exactly 1.0** while green and blue sit at 0.9000 |
| `--skin` | detail gain **0.25** inside the window, **1.00** with the window 90° away, **1.00** on a neutral, to 2.7e-8 |
| `--negative` | eight perturbed chains — gamma space, the knee before white balance, no coring, a doubled kernel, a steeper knee, the skin window ignored, the exponent high — each **fails** its check at both rasters |
| mutation | one character of the shipped GLSL (the encode's `- OetfC` → `+ OetfC`) was caught by five checks at both rasters; the two that read the Show Detail view did not, as they should not; reverted |
| `tools/sweep.py` | all **23** controls measurably change the picture |
| shaders | all 3, as the plugin compiles them, through `glslc` |
| `--pipe` | 2.5 frames in, exactly 2 out; an unknown cue refused (2); a failed render and a closed stdout (`\| head -c 1`) each exit 1; a boolean cue steps and a slider cue ramps |
| the bundle | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs; `oxbow` reports `SW CCU` / `CC01` / `effect` and renders 120 frames through `plugMain` |
| `--cpu` | the OpenFX build's CPU copy of the two passes against the GPU, in float, on the test card at both rasters: worst **1.6e-6** (1/2 500 of an 8-bit step) across the defaults, the null chain, every stage moved, the skin window and detail view, hard clips, a picture 1.4× past white and frame 300 of a drifting run; the seven perturbed chains agree with their perturbed copies to 9.5e-7; alpha bitwise; and three controls that must differ do, by 1.1e-2 to 2.0e-2 |
| `--laws` (drift) | the walk replayed from the frame number is **bit-identical** to the walk stepped from frame 0, at 22 frames probed at 24 and 60 fps up to an hour in; a replay from one time constant back is 0.33 out |
| `tools/ofx_agree.py` | the FFGL bundle's render (`cctest --pipe`, GPU) against the OpenFX bundle's (`ofxprobe`, CPU), 8-bit both sides, on the same picture: **0 of 57 600 pixels differ** at the defaults and at four settings that move every stage; the control (Detail Level 0.30 against 0.40) differs at 9 208 pixels. Also 0 differing at 1280×720 |
| the OpenFX bundle | universal, exports `OfxGetPlugin`, `CFBundleExecutable` names the binary, ad-hoc signs; `ofxprobe` resolves `com.stoatworks.ccu` to this build, sees 23 controls in seven groups plus the About block, and renders |
| Fusion's missing frame rate | the test host's `--quirks fusion` withholds the frame rate (and the frame range, the unmapped pair and the render-status props) as Resolve's Fusion page does: the build before the guard fails every render under it, as it failed in Fusion; the guarded build renders and is **byte-identical to the normal host at 24 fps** at eight cases (Drift 1 at frames 0, 1, 300 and 86 400, float, the General context among them), deterministic alone and in batches; in the normal host its output is unchanged |
| the OpenFX build in a host | an extended `ofxprobe` (any input, any time, float depth, batches in one instance; built for this round, not yet on resolume-ofx-bridge `main`): the CCU test card through both builds at the defaults and four settings, **0 of 57 600 pixels differ** in 8-bit and in float, the control differing at 15 464; **Drift 1 at frame 300, 60 fps, rendered alone: 0 pixels differ** from the FFGL plugin's frame 300 after 300 stepped frames, while frame 0 differs from it at 52 411 and 24 fps from 60 at 17 752; frame 300 alone is byte-identical to frame 300 after 0..299 in one instance and after 299, 5, 1000, 300, 0, 300; the General context renders what the Filter context does; keyframed controls are read at the render time; a premultiplied clip with alpha 255 → 0 keeps its alpha bitwise and lands within 2/255 of the FFGL build's straight-colour picture premultiplied |
| DaVinci Resolve | Resolve Studio 21.1 on macOS, 2026-10-04: the guarded build, added as a Fusion tool on the Fusion page, renders and **matches the test host to within 1/255** (the build before the guard failed every frame there) |

Render cost, best of three runs of 60 frames after a warm-up, `glFinish`
both sides, on a GPU shared with other builds: **0.06 ms** at 720p, **0.17
ms** at 1080p, **0.68–0.70 ms** at 4K — 4% of a 60 fps frame at 4K. Two
passes and eight texel fetches a pixel. macOS figures only.

The OpenFX build on the CPU (`cctest --bench-cpu`, both passes, best of three
runs of 20 frames): **66–71 ms** a 1080p frame on one thread, **10 ms** on 8
threads (what `ofxprobe`'s thread suite gives it) and **8.5–8.9 ms** on all 16
of the M4 Max's cores; 4K is 33–39 ms on 8–16 threads. The drift's replay adds
0.54 ms a frame at 60 fps, 0.22 ms at 24. In a host (the extended `ofxprobe`,
which lends 8 threads) the whole render action at 1080p — marshalling, the
drift's replay an hour into a 60 fps timeline, both passes — is **14.8 ms** in
8-bit and 14.4 ms in float, best of five. Not timed in any commercial host.

Seen on footage: nine of Resolume's bundled demo clips through `--pipe` at
the defaults, judged by eye beside the source — halos of the delay's width on
the bright edges, hot spots and white backgrounds compressed to a milky 90%,
the fine dark texture left alone by the coring. A hot broadcast camera, not a
broken one. Not measured.

### Not established

It has **never been loaded into Resolume on macOS**. Everything above was
compiled, rendered and measured offline against the real plugin class in a
headless CGL context, plus an `oxbow` load. How twenty-three controls in eight
groups read in Arena's inspector on macOS is untested. The drift has not been
watched over a minute in a host. On Windows, a CI build of this source loads, registers and renders in Resolume Arena 7.27.1 on software rendering (win-lab, Mesa llvmpipe, no GPU): all 29 host controls match the declaration and all 24 that take a value move the picture, 9 of the fleet gate's 9 checks (`plugin-bench/arena/expect/ccu.json`). Software rendering says nothing about a GPU or about speed.

The OpenFX build has **never been run in Vegas, Nuke or Natron**. In DaVinci
Resolve its first build was loaded once into Resolve Studio 21.1 as a Fusion
tool and failed every frame: Fusion reports no frame rate, and the read threw
out of the render. That read is guarded now (24 fps when there is none), and the
test host's Fusion mode, which withholds the same properties, renders it; on
2026-10-04 the guarded build rendered in Resolve Studio 21.1 on macOS as a
Fusion tool and matched the test host to within 1/255. That is the only real
host it has rendered in, and the Fusion page the only place in Resolve it has
been tried. It has run in `ofxprobe`, the fleet's
OFX test host, stock and extended: Filter and General contexts, 8-bit and float
RGBA, any frame, full frames at render scale 1. Nothing has handed it a 16-bit clip, a tile, a proxy
render scale or an RGB-only clip. The Windows `.ofx` is built by CI and has
never been run; the Linux `.ofx` is built on AlmaLinux 8 and only dlopened on
Rocky 8 in CI, never rendered. How the controls read in a real host's
inspector, and what a real host's colour management does to the clip before
the chain sees it, are untested. There is a
[user guide](https://stoatworks-labs.com/software/ccu/guide/) and a browser
demo at [ccu-demo.stoatworks-labs.com](https://ccu-demo.stoatworks-labs.com/),
which is a port of the shaders rather than the plugin.

### Found filming the release video

The video was rendered by `cctest --pipe` over Resolume's bundled demo clips,
after every one of the 33 was put through the defaults: nothing floods or
blanks, the dark clips stay dark, and the defaults stood. One claim was
corrected by the footage: a warm white with the knee off does not give
cyan-edged highlights. Red reaches the ceiling first, so each highlight's core
goes white inside a warm surround; with the knee on it stays warm all the way
up.

## Build

Needs CMake 3.15+, a C++17 compiler, and the FFGL SDK submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/ccu
cd ccu
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

macOS builds are universal (Apple Silicon + Intel) by default; add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster dev build. Windows needs GLEW via vcpkg.

The same build produces the OpenFX plugin as `build/CCU.ofx.bundle`
(`-DBUILD_OFX=OFF` to skip it). `-DCCU_BUILD_FFGL=OFF` builds the OpenFX plugin
alone, with no FFGL SDK and no GLEW — any C++17 compiler on macOS, Windows or
Linux. Nothing installs it: copy the bundle into the OpenFX folder above.

## Building and testing

The offline harness renders the real plugin class headlessly:

```bash
./build/cctest --out /tmp/frame.png --size 1920x1080   # the test card
./build/cctest --list                                  # every control, kind and default
./build/cctest --identity --detail --coring --knee --gamma --order --skin   # each claim, measured
./build/cctest --negative                              # and the checks can fail
./build/cctest --cpu                                   # the OpenFX build's CPU passes against the GPU
./build/cctest --offline                               # what needs no GL (CI)
./build/cctest --bench                                 # 720p, 1080p and 4K
./build/cctest --bench-cpu                             # the OpenFX build's CPU cost, no GL
python3 tools/ofx_agree.py --build build               # FFGL bundle against OFX bundle, byte for byte (needs ofxprobe)
python3 tools/sweep.py                                 # no control is silently dead
tools/verify.sh                                        # all of it, on a fresh universal build
```

Every check takes `--size`; run it at 320×180 as well as the raster you care about.
Footage goes through the real shaders with `--pipe`, in the fleet's frame format:

```bash
ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
  | ./build/cctest --pipe --size 1920x1080 --fps 50 --script cues.txt \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 50 -i - out.mov
```

See [`CLAUDE.md`](CLAUDE.md) for the full command reference and
[`AGENTS.md`](AGENTS.md) for the model, the traps, and where every tolerance
comes from.

## Browser demo

[ccu-demo.stoatworks-labs.com](https://ccu-demo.stoatworks-labs.com/) runs
the plugin's own linear and process shaders in WebGL2, on generated clips, with
every control the plugin declares in its own order, groups and defaults.
`demo/tools/check_shaders.py` fails `tools/verify.sh` if the page's copy of any
shader drifts from `source/Shaders.cpp`. Everything the C++ computes on the way
to a uniform — the control laws in `Controls.cpp`, the OETF's constants from the
exponent, the matrices and the white-balance drift in `Model.h` — is a hand port
to JavaScript in `demo/plugin.js`, and nothing checks that but a reader. It is
not the plugin: no Resolume, no FFGL, GLSL ES 3.00 rather than GL 4.1, the
kit's clock rather than the plugin's, and a float render target is required.
The page says all of this itself. Source in [`demo/`](demo/).

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).
