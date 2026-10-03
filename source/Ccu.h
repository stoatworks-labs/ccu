#pragma once

#include "Chain.h"
#include "Clock.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include <cstdint>
#include <string>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	CCU -- a broadcast camera's processing chain with every knob out, as an
	FFGL effect.

	**The one idea.** The "video" look of a studio or OB camera is not a
	filter. It is a processing chain in a fixed order, each stage a known
	circuit with the knob a shader on the CCU panel turned: linear light,
	white balance, matrix, detail, knee, gamma and black gamma, pedestal,
	white clip. Put the stages in the right order in linear light, label the
	knobs the way a CCU labels them, and the badly set-up camera looks fall
	out rather than being drawn: halos the width of the detail delay, grain
	sharpened by a low coring, faces softened by skin detail while the
	jacket stays sharp, skies that keep their colour through the knee, a
	warm white that clips in one channel first, and a white balance that
	wanders as the camera warms up.

	Two passes: the linear picture (through the matrix) into an RGBA32F
	buffer with its luma in alpha, then everything else straight into the
	host's framebuffer. The only state across frames is the drift, a double
	on the CPU. See Model.h for the chain and AGENTS.md for the rest.
*/
class Ccu : public CFFGLPlugin
{
public:
	Ccu();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// The host's clock, for the drift. Resolume sends milliseconds and
	/// overflows a float; the Clock keeps an origin and an offset in double
	/// and the drift only ever sees a dt.
	FFResult SetTime( double time ) override;

	//--- test hooks. Read by cctest; the plugin's own operation never uses
	//--- them, and the perturbation is always 0 outside the harness.

	/// Negative-control hooks, a bitmask of `model::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}

	/// The harness renders as fast as the GPU allows, so the clock's unit
	/// voting has nothing to measure: the harness declares seconds.
	void SetClockSecondsForTest()
	{
		clock.SetScaleForTest( 1.0 );
	}

	/// The drift walk's normalised value, for the harness to print.
	double DriftStateForTest() const
	{
		return driftWalk;
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	/// `SetParamGroup` collapses RUNS of consecutive same-group ids, so the
	/// order is load-bearing: append only.
	enum ParamID : FFUInt32
	{
		//Exposure
		PT_MASTER_GAIN,
		PT_MASTER_BLACK,
		PT_WHITE_CLIP,

		//White
		PT_R_GAIN,
		PT_B_GAIN,
		PT_DRIFT,

		//Matrix
		PT_MATRIX,
		PT_SATURATION,

		//Detail
		PT_DETAIL_LEVEL,
		PT_DETAIL_FREQ,
		PT_HV_RATIO,
		PT_CORING,
		PT_LEVEL_DEP,
		PT_SKIN_DETAIL,
		PT_SKIN_HUE,
		PT_SKIN_WIDTH,

		//Knee
		PT_KNEE_ON,
		PT_KNEE_POINT,
		PT_KNEE_SLOPE,

		//Gamma
		PT_GAMMA,
		PT_BLACK_GAMMA,

		//Output
		PT_MIX,
		PT_SHOW_DETAIL,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// params[] as the shared settings code reads them (Chain.h).
	ccu::chain::HostValues hostValues() const;

	ffglex::FFGLShader linearShader;
	ffglex::FFGLShader processShader;
	ffglex::FFGLScreenQuad quad;

	ccu::PassBuffer linear;///< picture size, RGBA32F: linear rgb after the matrix, luma in alpha

	ccu::Clock clock;
	double hostTime   = -1.0;
	double lastNow    = 0.0;
	double driftWalk  = 0.0;///< the normalised Ornstein-Uhlenbeck state
	uint32_t frameIndex = 0;

	int perturb = 0;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
