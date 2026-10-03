#pragma once

/**
	One frame's settings for the chain, from the host's sliders to the
	uniforms the two passes read -- with no GL anywhere in it.

	Both builds call `Resolve`. The FFGL plugin uploads what it returns; the
	OpenFX plugin hands it to the CPU copy of the two shaders (CpuChain.h).
	So the control laws, the OETF's constants, the matrix, the drift's pull on
	the R and B gains and the rounding of each to the float a shader sees all
	live once, and the two builds cannot read a slider differently. The only
	thing written twice is the per-pixel arithmetic of the two passes, and
	`cctest --cpu` holds the copy to the GPU.

	The uniforms are stored by the names the GLSL gives them, so the mirror
	reads `u.KneePoint` where the shader reads `KneePoint`.
*/

#include "Model.h"

namespace ccu::chain
{

/// The controls as the host holds them: 0..1 for a slider, the element index
/// for Matrix, 0 or 1 for the booleans. The member initialisers ARE the
/// defaults, for both builds.
///
/// They add up to a camera somebody set up in a hurry: a touch of detail at
/// a two-pixel delay with the coring low enough to show it, the knee on with
/// a soft slope so the top of the picture goes milky, a little black gamma,
/// the Standard matrix, and a slow drift. The null is Mix at zero.
struct HostValues
{
	//Exposure
	float masterGain  = 0.25f;//exactly 0 dB
	float masterBlack = 0.25f;//exactly 0
	float whiteClip   = 0.5f; //exactly 1.0

	//White
	float rGain = 0.5f; //exactly 0 dB
	float bGain = 0.5f;
	float drift = 0.15f;//9 mireds RMS, tau 20 s

	//Matrix
	float matrix     = static_cast< float >( model::kMatrixStandard );
	float saturation = 0.5f;//exactly 1

	//Detail
	float detailLevel = 0.3f;   //0.9: a step of h overshoots by 0.225 h
	float detailFreq  = 0.125f; //exactly 2 px
	float hvRatio     = 0.5f;   //both exactly 1
	float coring      = 0.3f;   //an edge below 0.0225 linear gets no detail
	float levelDep    = 0.5f;
	float skinDetail  = 0.4f;
	float skinHue     = 0.0556f;//20 degrees, a skin tone in linear RGB
	float skinWidth   = 0.2727f;//+-20 degrees

	//Knee
	float kneeOn    = 1.0f;
	float kneePoint = 0.5f; //0.7 linear
	float kneeSlope = 0.25f;//0.29

	//Gamma
	float gamma      = 0.5f;//exactly 0.45
	float blackGamma = 0.2f;

	//Output
	float mix        = 1.0f;
	float showDetail = 0.0f;
};

/// Every uniform the linear and process passes read except the picture's
/// size, by the GLSL's own names, rounded to float exactly as they are
/// uploaded.
struct Uniforms
{
	//--- linear pass ---------------------------------------------------------
	float MasterGain;
	float GainR;
	float GainB;
	float Matrix[ 9 ];///< row-major, as Model.h holds it (transposed on upload)
	float InvA;
	float InvC;
	float InvK;
	float InvKnee;
	float InvGamma;

	//--- both passes ---------------------------------------------------------
	int Perturb;
	int KneeOn;
	float KneePoint;
	float KneeSlope;

	//--- process pass --------------------------------------------------------
	float DetailLevel;
	int SpacingInt;
	float SpacingFrac;
	float HWeight;
	float VWeight;
	float CoringDead;
	float LevelDep;
	float LevelRef;
	float SkinSuppress;
	float SkinHue;
	float SkinWidth;
	float SkinInner;
	float SkinChromaLo;
	float SkinChromaHi;

	float OetfA;
	float OetfC;
	float OetfK;
	float OetfBreak;
	float OetfGamma;

	float BlackGamma;
	float BlackLevel;
	float BlackLift;
	float Pedestal;
	float WhiteClip;

	float MixAmount;
	int ShowDetail;
};

/**
	The settings for one frame. `driftWalk` is the white-balance drift's
	normalised Ornstein-Uhlenbeck state at this frame (Model.h): the FFGL
	build steps it frame by frame, the OpenFX build replays it from the frame
	number with `model::DriftWalkAt`. `perturb` is the harness's
	negative-control mask and is 0 in every shipped build.
*/
Uniforms Resolve( const HostValues& host, double driftWalk, int perturb = 0 );

} // namespace ccu::chain
