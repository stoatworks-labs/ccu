# ccu

A broadcast camera's processing chain with every knob out, as an FFGL **effect**
(`CC01`, shown as `SW CCU`) for Resolume Arena/Avenue. C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT. The same chain is also an
**OpenFX** effect (`com.stoatworks.ccu`, shown as `CCU` in `Stoatworks`) for
Resolve/Vegas/Nuke/Natron: `CCU.ofx.bundle`, CPU render, macOS/Windows/Linux.

Read `AGENTS.md` before changing the chain (`Model.h`, the two shaders in
`Shaders.cpp`), the control laws, the defaults or the harness's tolerances.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel 4`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- OpenFX: built by default as `build/CCU.ofx.bundle` (`-DBUILD_OFX=OFF` skips it);
  `-DCCU_BUILD_FFGL=OFF` builds it alone with no FFGL SDK and no GLEW (the Linux job).
  Never copied into `/Library/OFX/Plugins` from a session.
- The OpenFX plugin in a host: `~/Projects/resolume/resolume-ofx-bridge/build/ofxprobe
  --dir build --render com.stoatworks.ccu --size 640x360 --out /tmp/o.bmp --set detailLevel=0.6`
  (OFX script names are camelCase: `masterGain`, `crispeningFreq`, `hvRatio`,
  `levelDependence`, `kneeOn`, `showDetail`, ...; Filter context, time 0, 8-bit only)
- Render a frame offline: `./build/cctest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Detail Level=0.5" --set "Matrix=2" --set "Knee On=0"`
  (0..1 for sliders, the element index for Matrix, 0/1 for the booleans)
- List parameters, kinds, defaults and ranges: `./build/cctest --list`
- The exact GLSL the plugin compiles: `./build/cctest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (the synthetic clock the drift runs on) and an
  optional `--script` of `frame Parameter Name value` cues. Sliders ramp linearly
  between a name's cues; **Matrix, Knee On and Show Detail step** (each cue's value
  holds until the next cue's frame). Both hold before the first cue and after the
  last. A cue naming no parameter exits 2 before any frame; a partial frame at the end
  of stdin ends the stream with exit 0; a failed render or a closed stdout exits 1
  (SIGPIPE is ignored so a closed stdout is a failed write, not a 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/cctest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the offline checks +
  every rendered check at 320x180 AND 1280x720 + the --pipe contract + the sweep + the
  bundle, ~3 min)
- Every stage at its null returns the input, alpha bitwise, and after a resize: `./build/cctest --identity`
- A step's overshoot is Detail Level × h / 4 for exactly the spacing: `./build/cctest --detail`
- Below Coring no detail; above it the overshoot less the dead zone: `./build/cctest --coring`
- The knee's slope, continuity and per-channel action: `./build/cctest --knee`
- The OETF at three exponents; Black Gamma below its level only: `./build/cctest --gamma`
- WB before the knee, the knee before the clip: `./build/cctest --order`
- The skin window's gain inside, outside, and on a neutral: `./build/cctest --skin`
- The checks can fail: `./build/cctest --negative`; one perturbation verbosely:
  `./build/cctest --order --perturb 2` (bits in `Model.h`)
- The OpenFX build's CPU copy of the two passes against the GPU, with controls that
  must differ: `./build/cctest --cpu`; its cost, no GL: `./build/cctest --bench-cpu`
- The FFGL bundle against the OpenFX bundle through their entry points, byte for byte:
  `python3 tools/ofx_agree.py --build build` (needs ofxprobe)
- No GL (what CI runs first): `./build/cctest --offline` = `--laws --names`
- Every rendered check takes `--size WxH`; CI runs them at 320x180 with `--allow-no-gl`
- Shaders through glslc: `tools/check-shaders.sh build/cctest`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/cctest --bench` (720p, 1080p, 4K; best of three; the GPU is
  shared, so run it twice)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/CCU.bundle`

## Notes
- **The shaders ARE the chain.** Each stage lives once, in GLSL (`Shaders.cpp`): the
  linear pass (linearise, master gain, white balance, matrix; luma in alpha) into an
  RGBA32F buffer, then the process pass (detail, knee, gamma, black gamma, pedestal,
  white clip, mix) into the host's framebuffer. The C++ converts sliders to uniforms
  (`Controls.cpp`) and computes the OETF's constants, the matrix and the drift's pull
  on the gains in double, rounding each to float once (`chain::Resolve` in
  `Chain.cpp`, which both builds call; `Model.h`). The harness restates every law and
  holds the shaders to it.
- **The one copy: `CpuChain.cpp`** restates `kLinear` and `kProcess` statement for
  statement in float, for the OpenFX build. **Edit a shader, edit it too** (`//=
  mirrored` marks each place); `cctest --cpu` fails if they drift. The pointer
  comments in `Shaders.cpp` sit OUTSIDE the GLSL strings on purpose: `demo/plugin.js`
  carries a character-for-character copy of each string.
- **OpenFX drift** is `model::DriftWalkAt( frame, 1 / fps )`: DriftStep replayed up
  to the frame from at most 40 τ (800 s) back, bit-identical to the stepped walk
  (`--laws`), 0.54 ms a frame at 60 fps. Frames render alone and out of order there;
  nothing may carry state between renders.
- **Master Gain is head-end gain, in linear light, before white balance** — not a
  video gain after gamma. A CCU's dB gain is sensor gain, and putting it there is what
  lets the knee catch a gained-up highlight. AGENTS.md has the decision.
- **The OETF is a family with the exponent as the control**: a and k follow from it by
  continuity at 0.018, and at 0.45 they are BT.709's 1.099 / 4.5 to the standard's
  printed precision (the harness asserts it). The linearise uses the same family at
  0.45, so the round trip is exact in principle.
- **Every detail tap is `texelFetch`**, two per far tap with a hand lerp for a
  fractional spacing, clamped to the picture. Nothing rests on a texture unit's
  filtering, and a whole spacing is `( 1 - 0 ) a + 0 b`, which is `a` exactly.
- **The mix is written out** as `v * Mix + src * ( 1 - Mix )`, not `mix()`: the
  identity check needs `Mix = 1` to be exactly `v`.
- **Coring is in units of edge height** (an edge lower than Coring gets no detail), so
  the dead zone on the detail signal is Coring / 4.
- **The drift is an Ornstein–Uhlenbeck walk in double on the CPU** (τ 20 s, seeded
  through the PCG hash by frame), scaled by the Drift control at use, so a slider move
  never jumps the state. The clock is clamp's: unit voted, origin + offset in double.
- **Skin Hue's ends are the same hue**, so the sweep compares 0 with 180 degrees.
- **Parameter names must be unique and 16 characters or under** — hence `Crispening
  Freq` and `Level Dependence` (exactly 16).
- `SetParamInfo` clamps a STANDARD default into 0..1; `SetParamInfof` reads its default
  out of `params[]`, so fill `params[]` first. Matrix is mapped by index in
  `Model.h` (an option's range reads back 0..1).
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `ccu_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `ccu_dsp` (Controls, Chain, CpuChain — no GL, no SDK) is the OBJECT library the
  OpenFX build links; every final target names it directly, because an OBJECT
  library's objects do not travel through a second one (`ccu_core`).
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- `FFGLShader::Set` has no mat3 overload: the matrix goes in with `glUniformMatrix3fv`
  transposed (row-major on the CPU).
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `CC01`, display name `SW CCU`.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is measured offline on
  macOS, plus an `oxbow` load. Footage seen through `--pipe` (all 33 of Resolume's demo
  clips at the defaults, and the release video), judged by eye.
- **Windows**: gated in Resolume Arena 7.27.1 on win-lab (Mesa llvmpipe, no GPU): 9/9, all 24 valued controls live. Never run on a Windows GPU.
- **The OpenFX build has never been in Resolve, Vegas, Nuke or Natron**: only
  `ofxprobe`, stock (Filter, time 0, 8-bit) and an extended build (any frame, float,
  General, batches), where it renders the FFGL build's pictures byte for byte, drift
  included. The Windows `.ofx` is CI-built and never run; the Linux one is only
  dlopened on Rocky 8 in CI. Not in a release yet (after v0.1.0).
- No factory presets, no luma knee (the knee is per channel only), no audio input. The
  browser demo (`demo/`) is a port of the shaders, not the plugin.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated by the backend's syncs now the
  project is registered; never edit them by hand.

## Browser demo

`demo/` is the page at **ccu-demo.stoatworks-labs.com**, deployed from
`wrangler.toml` with `cf-run npx wrangler deploy` and by `deploy.yml` on a push to
main — no build step; what is committed is what is served. The host is a Worker
route plus a proxied `AAAA 100::` record (the zone is at its custom-domain limit).
`demo/vendor/` is copied in by
`~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh ccu` and is not
a place to edit. The shaders in `demo/plugin.js` must stay the plugin's:
`python3 demo/tools/check_shaders.py` (run by `tools/verify.sh`). The CPU half
(`Controls.cpp`, `Model.h`'s OETF, matrices and drift, the two draws) is a hand
port in `demo/plugin.js` that nothing checks: change one, change the other. Serve
it locally with `python3 -m http.server` in `demo/`. See AGENTS.md, *The browser
demo*.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/ccu/ccu.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\ccu\logs\ccu.YYYY-MM-DD.log   (Windows)
