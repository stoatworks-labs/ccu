# Attributions

CCU is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Harness, verify script, --pipe contract and the negative-control pattern — Stoatworks toner, slope and clamp

<https://github.com/stoatworks-labs/toner>  
Licence: MIT  
Copyright: Stoatworks Labs

tools/cctest's CGL plumbing, PNG writer, --list/--names/--out/--pipe with the cue-sheet parser, --offline and the GL-less CI, tools/verify.sh's two-raster shape and tools/sweep.py, adapted from toner's, which came by way of slope from clamp. Nothing of the copier, coder or clamp models is here.

### Host clock — Stoatworks clamp

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Clock.{h,cpp}: unit voting on the host's SetTime and an origin plus offset kept in double, unchanged, so the drift only ever sees a dt.

### Plugin shape, Diag logger and the About block — Stoatworks plumbicon

<https://github.com/stoatworks-labs/plumbicon>  
Licence: MIT  
Copyright: Stoatworks Labs

The two-pass effect's shape (source/Ccu.{h,cpp}, PluginEntry.cpp), source/Diag.{h,cpp} (the log-file writer, from orrery by way of plumbicon) and the About parameter headers. Plumbicon is the tube in front of this chain; none of its tube model is here.

### PassBuffer and the fleet's trap list — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

source/PassBuffer.* (tinsel's FFGLFBO with the colour-texture leak fixed) and the inherited GL traps applied throughout.

### Release notes shape and the provisional About — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

The shape of the release notes and the provisional hand copy of StoatworksAbout.h used before registration.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

### OpenFX image effect plug-in API

<https://github.com/AcademySoftwareFoundation/openfx>  
Licence: BSD-3-Clause  
Copyright: OpenFX and contributors to the OpenFX project

Vendored at external/openfx — a git submodule in resolume-ofx-bridge, a copy of the headers and Support library elsewhere.

The plugin ABI for the DaVinci Resolve and Nuke side of the same effects, so one core renders through both hosts.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Broadcast camera processing chains, as a genre

The stages and their order (linear light, white balance, matrix, aperture correction with coring and skin detail, knee, gamma and black gamma, pedestal, white clip) and the names a camera control unit gives its knobs are the trade's public convention, and that is all that was taken. The matrix presets are derivations with generic names: the Standard matrix is SMPTE 170M primaries to BT.709 primaries from the chromaticities, checked against the matrix Charles Poynton publishes; High Saturation and Film-like are a saturation and a hue rotation about the grey axis with judged constants. The OETF is BT.709's, as a family in its exponent. No manufacturer's circuit, firmware, curve, preset, menu or documentation was copied or consulted, and no manufacturer is named.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
