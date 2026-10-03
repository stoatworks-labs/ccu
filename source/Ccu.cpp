#include "Ccu.h"

#include "Chain.h"
#include "Diag.h"
#include "Model.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace ccu;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Ccu >,// Create method
	"CC01",              // Plugin unique ID of maximum length 4.
	"SW CCU",            // Plugin name
	2,                   // API major version number
	1,                   // API minor version number
	0,                   // Plugin major version number
	1,                   // Plugin minor version number
	FF_EFFECT,           // Plugin type
	"A broadcast camera's processing chain with every knob out.\n\nNot a filter: the stages of a studio or OB camera in their fixed order, in linear light - white balance, matrix, detail (aperture correction), knee, gamma and black gamma, pedestal, white clip - each labelled the way a CCU labels it. The badly set-up camera looks fall out: halos the width of the detail delay, grain sharpened by a low coring, faces softened by skin detail, skies that keep colour through the knee, a warm white that clips in one channel first, and a white balance that drifts as the camera warms up.\n\nStart with Detail Level and Coring, then the Knee.",// Plugin description
	"CCU FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

GLint loc( const FFGLShader& shader, const char* name )
{
	return glGetUniformLocation( shader.GetGLID(), name );
}
} // namespace

//---------------------------------------------------------------------------
Ccu::Ccu()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The drift is the one thing here that moves with time.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. Filled BEFORE any declaration: SetParamInfof reads its
	// default out of GetFloatParameter. They live in chain::HostValues, which
	// the OpenFX build declares its defaults from too, and say there what
	// each one is.
	//---------------------------------------------------------------------
	const chain::HostValues defaults;
	params[ PT_MASTER_GAIN ]  = defaults.masterGain;
	params[ PT_MASTER_BLACK ] = defaults.masterBlack;
	params[ PT_WHITE_CLIP ]   = defaults.whiteClip;
	params[ PT_R_GAIN ]       = defaults.rGain;
	params[ PT_B_GAIN ]       = defaults.bGain;
	params[ PT_DRIFT ]        = defaults.drift;
	params[ PT_MATRIX ]       = defaults.matrix;
	params[ PT_SATURATION ]   = defaults.saturation;
	params[ PT_DETAIL_LEVEL ] = defaults.detailLevel;
	params[ PT_DETAIL_FREQ ]  = defaults.detailFreq;
	params[ PT_HV_RATIO ]     = defaults.hvRatio;
	params[ PT_CORING ]       = defaults.coring;
	params[ PT_LEVEL_DEP ]    = defaults.levelDep;
	params[ PT_SKIN_DETAIL ]  = defaults.skinDetail;
	params[ PT_SKIN_HUE ]     = defaults.skinHue;
	params[ PT_SKIN_WIDTH ]   = defaults.skinWidth;
	params[ PT_KNEE_ON ]      = defaults.kneeOn;
	params[ PT_KNEE_POINT ]   = defaults.kneePoint;
	params[ PT_KNEE_SLOPE ]   = defaults.kneeSlope;
	params[ PT_GAMMA ]        = defaults.gamma;
	params[ PT_BLACK_GAMMA ]  = defaults.blackGamma;
	params[ PT_MIX ]          = defaults.mix;
	params[ PT_SHOW_DETAIL ]  = defaults.showDetail;

	SetParamInfof( PT_MASTER_GAIN, "Master Gain", FF_TYPE_STANDARD );
	SetParamInfof( PT_MASTER_BLACK, "Master Black", FF_TYPE_STANDARD );
	SetParamInfof( PT_WHITE_CLIP, "White Clip", FF_TYPE_STANDARD );

	SetParamInfof( PT_R_GAIN, "R Gain", FF_TYPE_STANDARD );
	SetParamInfof( PT_B_GAIN, "B Gain", FF_TYPE_STANDARD );
	SetParamInfof( PT_DRIFT, "Drift", FF_TYPE_STANDARD );

	//Enum order, not alphabetical: Identity, Standard, High Saturation,
	//Film-like reads as a progression from "off".
	SetOptionParamInfo( PT_MATRIX, "Matrix", model::kMatrixCount, params[ PT_MATRIX ] );
	for( int i = 0; i < model::kMatrixCount; ++i )
		SetParamElementInfo( PT_MATRIX, static_cast< unsigned int >( i ), model::kMatrixNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_SATURATION, "Saturation", FF_TYPE_STANDARD );

	SetParamInfof( PT_DETAIL_LEVEL, "Detail Level", FF_TYPE_STANDARD );
	SetParamInfof( PT_DETAIL_FREQ, "Crispening Freq", FF_TYPE_STANDARD );
	SetParamInfof( PT_HV_RATIO, "H/V Ratio", FF_TYPE_STANDARD );
	SetParamInfof( PT_CORING, "Coring", FF_TYPE_STANDARD );
	SetParamInfof( PT_LEVEL_DEP, "Level Dependence", FF_TYPE_STANDARD );
	SetParamInfof( PT_SKIN_DETAIL, "Skin Detail", FF_TYPE_STANDARD );
	SetParamInfof( PT_SKIN_HUE, "Skin Hue", FF_TYPE_STANDARD );
	SetParamInfof( PT_SKIN_WIDTH, "Skin Width", FF_TYPE_STANDARD );

	SetParamInfo( PT_KNEE_ON, "Knee On", FF_TYPE_BOOLEAN, params[ PT_KNEE_ON ] >= 0.5f );
	SetParamInfof( PT_KNEE_POINT, "Knee Point", FF_TYPE_STANDARD );
	SetParamInfof( PT_KNEE_SLOPE, "Knee Slope", FF_TYPE_STANDARD );

	SetParamInfof( PT_GAMMA, "Gamma", FF_TYPE_STANDARD );
	SetParamInfof( PT_BLACK_GAMMA, "Black Gamma", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );
	SetParamInfo( PT_SHOW_DETAIL, "Show Detail", FF_TYPE_BOOLEAN, false );

	for( FFUInt32 i = PT_MASTER_GAIN; i <= PT_WHITE_CLIP; ++i )
		SetParamGroup( i, "Exposure" );
	for( FFUInt32 i = PT_R_GAIN; i <= PT_DRIFT; ++i )
		SetParamGroup( i, "White" );
	for( FFUInt32 i = PT_MATRIX; i <= PT_SATURATION; ++i )
		SetParamGroup( i, "Matrix" );
	for( FFUInt32 i = PT_DETAIL_LEVEL; i <= PT_SKIN_WIDTH; ++i )
		SetParamGroup( i, "Detail" );
	for( FFUInt32 i = PT_KNEE_ON; i <= PT_KNEE_SLOPE; ++i )
		SetParamGroup( i, "Knee" );
	for( FFUInt32 i = PT_GAMMA; i <= PT_BLACK_GAMMA; ++i )
		SetParamGroup( i, "Gamma" );
	for( FFUInt32 i = PT_MIX; i <= PT_SHOW_DETAIL; ++i )
		SetParamGroup( i, "Output" );

	// The About block. Inline, because SetParamInfo is protected on
	// CFFGLPlugin and nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created CCU effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Ccu::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct Stage
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	};
	const Stage stages[] = {
		{ &linearShader, shaders::kLinear, "linear" },
		{ &processShader, shaders::kProcess, "process" },
	};
	for( const Stage& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertex, stage.fragment ) )
			continue;
		//Invisible to the operator otherwise: the effect does nothing in
		//Resolume, with no message anywhere.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "CCU: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	clock.Reset();
	lastNow    = 0.0;
	driftWalk  = 0.0;
	frameIndex = 0;

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Ccu::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	const int W = static_cast< int >( picture.Width );
	const int H = static_cast< int >( picture.Height );

	//---------------------------------------------------------------------
	// The clock and the drift, in double.
	//---------------------------------------------------------------------
	clock.Update( hostTime );
	const double now = clock.Now();
	const double dt  = std::max( 0.0, now - lastNow );
	lastNow          = now;
	driftWalk        = model::DriftStep( driftWalk, dt, frameIndex );
	++frameIndex;

	//---------------------------------------------------------------------
	// The settings, as the uniforms the two passes read. Chain.cpp works
	// them out in double and rounds each to float once; the OpenFX build
	// calls the same function.
	//---------------------------------------------------------------------
	const chain::Uniforms u = chain::Resolve( hostValues(), driftWalk, perturb );

	//---------------------------------------------------------------------
	// The buffer. Allocated before anything binds a texture: every ffglex
	// Scoped* binding CLEARS to 0 on exit, and FFGLFBO::Initialise sizes
	// its colour texture under one.
	//---------------------------------------------------------------------
	if( !linear.Ensure( W, H, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the linear buffer: " + std::to_string( W ) + " x " + std::to_string( H ) );
		return FF_FAIL;
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );

	//---------------------------------------------------------------------
	// 1. linear
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( linear.GetGLID(), ScopedFBOBinding::RB_REVERT );
		linear.ResizeViewPort();
		ScopedShaderBinding shader( linearShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding input( picture.Handle );

		linearShader.Set( "InputTexture", 0 );
		linearShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		linearShader.Set( "MasterGain", u.MasterGain );
		linearShader.Set( "GainR", u.GainR );
		linearShader.Set( "GainB", u.GainB );
		//Row-major on the CPU, so transposed on the way in.
		glUniformMatrix3fv( loc( linearShader, "Matrix" ), 1, GL_TRUE, u.Matrix );
		linearShader.Set( "InvA", u.InvA );
		linearShader.Set( "InvC", u.InvC );
		linearShader.Set( "InvK", u.InvK );
		linearShader.Set( "InvKnee", u.InvKnee );
		linearShader.Set( "InvGamma", u.InvGamma );
		linearShader.Set( "Perturb", u.Perturb );
		linearShader.Set( "KneeOn", u.KneeOn );
		linearShader.Set( "KneePoint", u.KneePoint );
		linearShader.Set( "KneeSlope", u.KneeSlope );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. process, straight into the host's framebuffer.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( processShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding lin( linear.TextureID() );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding input( picture.Handle );

		processShader.Set( "LinearTexture", 0 );
		processShader.Set( "InputTexture", 1 );
		processShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		processShader.Set( "PictureW", W );
		processShader.Set( "PictureH", H );

		processShader.Set( "DetailLevel", u.DetailLevel );
		processShader.Set( "SpacingInt", u.SpacingInt );
		processShader.Set( "SpacingFrac", u.SpacingFrac );
		processShader.Set( "HWeight", u.HWeight );
		processShader.Set( "VWeight", u.VWeight );
		processShader.Set( "CoringDead", u.CoringDead );
		processShader.Set( "LevelDep", u.LevelDep );
		processShader.Set( "LevelRef", u.LevelRef );
		processShader.Set( "SkinSuppress", u.SkinSuppress );
		processShader.Set( "SkinHue", u.SkinHue );
		processShader.Set( "SkinWidth", u.SkinWidth );
		processShader.Set( "SkinInner", u.SkinInner );
		processShader.Set( "SkinChromaLo", u.SkinChromaLo );
		processShader.Set( "SkinChromaHi", u.SkinChromaHi );

		processShader.Set( "KneeOn", u.KneeOn );
		processShader.Set( "KneePoint", u.KneePoint );
		processShader.Set( "KneeSlope", u.KneeSlope );

		processShader.Set( "OetfA", u.OetfA );
		processShader.Set( "OetfC", u.OetfC );
		processShader.Set( "OetfK", u.OetfK );
		processShader.Set( "OetfBreak", u.OetfBreak );
		processShader.Set( "OetfGamma", u.OetfGamma );

		processShader.Set( "BlackGamma", u.BlackGamma );
		processShader.Set( "BlackLevel", u.BlackLevel );
		processShader.Set( "BlackLift", u.BlackLift );
		processShader.Set( "Pedestal", u.Pedestal );
		processShader.Set( "WhiteClip", u.WhiteClip );

		processShader.Set( "MixAmount", u.MixAmount );
		processShader.Set( "ShowDetail", u.ShowDetail );
		processShader.Set( "Perturb", u.Perturb );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
chain::HostValues Ccu::hostValues() const
{
	chain::HostValues h;
	h.masterGain  = params[ PT_MASTER_GAIN ];
	h.masterBlack = params[ PT_MASTER_BLACK ];
	h.whiteClip   = params[ PT_WHITE_CLIP ];
	h.rGain       = params[ PT_R_GAIN ];
	h.bGain       = params[ PT_B_GAIN ];
	h.drift       = params[ PT_DRIFT ];
	h.matrix      = params[ PT_MATRIX ];
	h.saturation  = params[ PT_SATURATION ];
	h.detailLevel = params[ PT_DETAIL_LEVEL ];
	h.detailFreq  = params[ PT_DETAIL_FREQ ];
	h.hvRatio     = params[ PT_HV_RATIO ];
	h.coring      = params[ PT_CORING ];
	h.levelDep    = params[ PT_LEVEL_DEP ];
	h.skinDetail  = params[ PT_SKIN_DETAIL ];
	h.skinHue     = params[ PT_SKIN_HUE ];
	h.skinWidth   = params[ PT_SKIN_WIDTH ];
	h.kneeOn      = params[ PT_KNEE_ON ];
	h.kneePoint   = params[ PT_KNEE_POINT ];
	h.kneeSlope   = params[ PT_KNEE_SLOPE ];
	h.gamma       = params[ PT_GAMMA ];
	h.blackGamma  = params[ PT_BLACK_GAMMA ];
	h.mix         = params[ PT_MIX ];
	h.showDetail  = params[ PT_SHOW_DETAIL ];
	return h;
}

//---------------------------------------------------------------------------
FFResult Ccu::DeInitGL()
{
	linearShader.FreeGLResources();
	processShader.FreeGLResources();
	quad.Release();
	linear.Destroy();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Ccu::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Ccu::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Ccu::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Ccu::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Ccu::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}
