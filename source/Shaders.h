#pragma once

/**
	The two passes, as GLSL 4.10 source, plus the vertex shader.

	1. **linear**   picture size, RGBA32F. The host's picture linearised,
	                gained, white-balanced and matrixed; the BT.709 luma of
	                the result in alpha. This is where the chain's front half
	                lives, and the buffer exists so the detail taps can read
	                neighbours that have already been through it.
	2. **process**  the host's framebuffer. Detail from the linear buffer's
	                luma at +-spacing (two texelFetches and a lerp per tap),
	                then knee, gamma, black gamma, pedestal, white clip, and
	                the mix against the source. Show Detail replaces the
	                picture with the detail signal about mid grey.

	The shaders ARE the chain: each stage lives once, here, and the C++
	only converts sliders to uniforms and computes the constants in double
	(Chain.cpp). The one exception is the OpenFX build, which has no GPU to
	run these on: CpuChain.cpp restates both passes statement for statement
	in float, and `cctest --cpu` holds that copy to these.
	`cctest --dump-shaders` writes these exact strings and
	`tools/check-shaders.sh` compiles them, so what is checked is what the
	driver gets.
*/

namespace ccu::shaders
{

extern const char* const kVertex;
extern const char* const kLinear;
extern const char* const kProcess;

inline constexpr int kFragmentCount = 2;

} // namespace ccu::shaders
