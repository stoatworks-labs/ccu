#pragma once

/**
	The two passes of Shaders.cpp, on the CPU, for the OpenFX build.

	**This is a copy, and it is the only one.** The shaders are the chain:
	every stage lives in `kLinear` and `kProcess`, and the C++ the FFGL build
	runs only works out their uniforms (Chain.cpp, which this copy shares).
	OpenFX hosts hand a plugin a CPU buffer and Resolve's Linux build has no GL
	loader to hand it anything else, so here the per-pixel arithmetic is
	written a second time, statement for statement, in float -- the precision
	the GPU works in -- with the uniforms it reads already rounded to float by
	`chain::Resolve` exactly as they are uploaded. Every line below names the
	line of GLSL it mirrors; `//= mirrored` marks each place, and Shaders.cpp
	points back here. Edit one, edit the other.

	`cctest --cpu` renders pictures through both and holds the copy to the
	GPU at the defaults, at settings that move every stage, and at every
	negative control, with a control case that must differ.

	The picture layout is the harness's and the OpenFX build's both: RGBA
	float, rows of `width` pixels, straight (not premultiplied) colour. Row
	order does not matter -- the only taps that look at a neighbour are the
	detail kernel's, which is symmetric, and they clamp at both edges.
*/

#include "Chain.h"

namespace ccu::cpu
{

/// The linear pass (`kLinear`) for rows [ y0, y1 ): linearise, master gain,
/// white balance, matrix; the BT.709 luma of the result in alpha. `source`
/// and `linear` are both width x ( at least y1 ) RGBA.
void LinearRows( const chain::Uniforms& u, const float* source, int width, int y0, int y1, float* linear );

/// The process pass (`kProcess`) for pixels [ x0, x1 ) of row y: detail from
/// the linear picture's luma, knee, gamma, black gamma, pedestal, white
/// clip, the mix against the source and the source's alpha. `linear` must be
/// complete for the whole picture first. `out` receives ( x1 - x0 ) RGBA.
void ProcessRow( const chain::Uniforms& u, const float* linear, const float* source, int width, int height, int y, int x0, int x1,
                 float* out );

} // namespace ccu::cpu
