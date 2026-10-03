/// The OpenFX build of CCU, for DaVinci Resolve, Nuke, Natron, Vegas and other
/// OFX hosts.
///
/// ------------------------------------------------------- what is shared
///
/// **Everything up to the pixel.** `chain::Resolve` (Chain.cpp) is the same
/// function the FFGL plugin calls between reading its sliders and setting its
/// uniforms: the control laws, the OETF's constants for the chosen exponent,
/// the matrix preset composed with Saturation, the drift's pull on the R and B
/// gains, and the rounding of each one to the float a shader sees. So a slider
/// cannot mean one thing in Resolume and another here.
///
/// **What is written twice** is the per-pixel arithmetic of the two GLSL
/// passes, which has nowhere to run in a CPU host: CpuChain.cpp restates
/// `kLinear` and `kProcess` statement for statement, in float, and
/// `cctest --cpu` holds it to the GPU. This file only marshals: OFX's pixel
/// formats in, straight float RGBA through the chain, and back.
///
/// ------------------------------------------------- the one real difference
///
/// **The drift is a function of the frame number here.** The FFGL build steps
/// its Ornstein-Uhlenbeck walk once per frame it is handed, from the moment
/// the effect starts; OFX renders frames in any order, alone and on several
/// threads at once, and a walk carried from render to render would give a
/// different picture every export. `model::DriftWalkAt` replays the SAME steps
/// -- DriftStep, the same hash, the frame's dt from the clip's frame rate --
/// for the frames before this one, starting at most 40 time constants (800 s)
/// back, where the forgotten history weighs less than the rounding of a double.
/// So frame N rendered alone is frame N rendered in sequence, scrubbing shows
/// the camera as it was at that frame, and the walk is the one Resolume would
/// have walked had it played the timeline from frame 0 at this frame rate.
/// What it gives up: it starts from no drift at frame 0, as the FFGL build
/// does when the effect is added, so the first minute of a timeline that
/// starts at 0 is the camera warming up.
///
/// Nothing else carries across frames, so nothing else differs. There is no
/// audio in this effect and no host-beat control, in either build.
///
/// ------------------------------------------------------------- and tiles
///
/// The detail kernel reads the luma up to ten pixels away in each direction,
/// so a tile would need a margin; `setSupportsTiles( false )` asks for the
/// whole frame instead. The delay is in pixels of the image the host renders:
/// a host rendering a half-resolution proxy gets halos twice as wide relative
/// to the frame, exactly as a camera of half the resolution would.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Chain.h"
#include "../CpuChain.h"
#include "../Model.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.ccu";
constexpr const char* kPluginName       = "CCU";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A broadcast camera's processing chain with every knob out.\n\n"
	"Not a filter: the stages of a studio or OB camera in their fixed order, in "
	"linear light - white balance, matrix, detail (aperture correction), knee, "
	"gamma and black gamma, pedestal, white clip - each labelled the way a CCU "
	"labels it. The badly set-up camera looks fall out: halos the width of the "
	"detail delay, grain sharpened by a low coring, faces softened by skin "
	"detail, skies that keep colour through the knee, a warm white that clips "
	"in one channel first, and a white balance that drifts as the camera warms "
	"up.\n\n"
	"Start with Detail Level and Coring, then the Knee.\n\n"
	"Every control is the Resolume build's. One behaves differently: Drift here "
	"is a function of the frame number rather than of how long the effect has "
	"been running, so a frame renders the same alone, in order or out of order. "
	"It starts from no drift at frame 0 and settles over the first minute. The "
	"detail delay is in pixels of the image the host renders.\n\n"
	"https://stoatworks-labs.com";

//The script names. A saved project refers to these, so they are permanent.
constexpr const char* kParamMasterGain  = "masterGain";
constexpr const char* kParamMasterBlack = "masterBlack";
constexpr const char* kParamWhiteClip   = "whiteClip";
constexpr const char* kParamRGain       = "rGain";
constexpr const char* kParamBGain       = "bGain";
constexpr const char* kParamDrift       = "drift";
constexpr const char* kParamMatrix      = "matrix";
constexpr const char* kParamSaturation  = "saturation";
constexpr const char* kParamDetailLevel = "detailLevel";
constexpr const char* kParamDetailFreq  = "crispeningFreq";
constexpr const char* kParamHVRatio     = "hvRatio";
constexpr const char* kParamCoring      = "coring";
constexpr const char* kParamLevelDep    = "levelDependence";
constexpr const char* kParamSkinDetail  = "skinDetail";
constexpr const char* kParamSkinHue     = "skinHue";
constexpr const char* kParamSkinWidth   = "skinWidth";
constexpr const char* kParamKneeOn      = "kneeOn";
constexpr const char* kParamKneePoint   = "kneePoint";
constexpr const char* kParamKneeSlope   = "kneeSlope";
constexpr const char* kParamGamma       = "gamma";
constexpr const char* kParamBlackGamma  = "blackGamma";
constexpr const char* kParamMix         = "mix";
constexpr const char* kParamShowDetail  = "showDetail";

using namespace ccu;

//---------------------------------------------------------------------------
// Marshalling, one row at a time so it runs inside the passes' threads.
//
// The chain works on straight colour, as the FFGL build's shaders do: a
// pedestal or a knee applied to premultiplied values would act on the alpha
// as well as the picture. So a premultiplied clip is divided out on the way
// in and multiplied back on the way out. At alpha 1 -- every clip a camera
// ever made -- both are exact, and this is the FFGL build's arithmetic.
//---------------------------------------------------------------------------
template< typename Pixel, int Components, int Maximum >
void gatherRow( const OFX::Image* src, int x1, int x2, int y, bool premultiplied, float* out )
{
	for( int x = x1; x < x2; ++x, out += 4 )
	{
		const Pixel* px = static_cast< const Pixel* >( src->getPixelAddress( x, y ) );
		if( px == nullptr )
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0f;
			continue;
		}

		//Divided, not multiplied by a reciprocal: a GL unorm texture reads
		//c / ( 2^n - 1 ), and that is the value the FFGL build starts from.
		const float a = Components == 4 ? static_cast< float >( px[ 3 ] ) / static_cast< float >( Maximum ) : 1.0f;
		for( int c = 0; c < 3; ++c )
		{
			const float v = static_cast< float >( px[ c ] ) / static_cast< float >( Maximum );
			out[ c ]      = ( premultiplied && Components == 4 ) ? ( a > 0.0f ? v / a : 0.0f ) : v;
		}
		out[ 3 ] = a;
	}
}

template< typename Pixel, int Components, int Maximum >
void scatterPixel( const float* in, bool premultiplied, Pixel* px )
{
	const float a = in[ 3 ];
	for( int c = 0; c < 3; ++c )
	{
		float v = in[ c ];
		if( premultiplied && Components == 4 )
			v *= a;

		//Integer formats clamp; float ones are left alone, because a host
		//working in float may legitimately carry values outside 0..1 and the
		//chain's own white clip has already happened.
		if( Maximum != 1 )
			px[ c ] = static_cast< Pixel >( std::lround( std::clamp( v, 0.0f, 1.0f ) * static_cast< float >( Maximum ) ) );
		else
			px[ c ] = static_cast< Pixel >( v );
	}
	if( Components == 4 )
		px[ 3 ] = Maximum == 1 ? static_cast< Pixel >( a )
		                       : static_cast< Pixel >( std::lround( std::clamp( a, 0.0f, 1.0f ) * static_cast< float >( Maximum ) ) );
}

//---------------------------------------------------------------------------
// One frame's working state, resolved on the calling thread before any pixel.
//---------------------------------------------------------------------------
struct Frame
{
	chain::Uniforms uniforms = {};
	OfxRectI bounds          = { 0, 0, 0, 0 };///< the source's
	int width                = 0;
	int height               = 0;
	bool premultiplied       = false;

	std::vector< float > source;///< straight RGBA, row 0 at the bottom, as OFX holds it
	std::vector< float > linear;///< the linear pass: rgb after the matrix, luma in alpha
};

//---------------------------------------------------------------------------
// The two passes, each across the host's threads. The second reads the
// first's neighbours, so the first finishes -- process() returns -- before
// the second starts.
//---------------------------------------------------------------------------
class PassBase : public OFX::ImageProcessor
{
public:
	PassBase( OFX::ImageEffect& effect, Frame& frameValue, const OFX::Image* srcValue ) :
		OFX::ImageProcessor( effect ),
		frame( frameValue ),
		src( srcValue )
	{
	}

protected:
	Frame& frame;
	const OFX::Image* src;
};

/// Gather the source and run `kLinear`'s copy, over rows in SOURCE index space
/// (0 .. height): there is no destination image in this pass.
template< typename Pixel, int Components, int Maximum >
class LinearPass : public PassBase
{
public:
	using PassBase::PassBase;

	void multiThreadProcessImages( OfxRectI window ) override
	{
		for( int row = window.y1; row < window.y2; ++row )
		{
			if( _effect.abort() )
				break;
			float* line = frame.source.data() + static_cast< size_t >( row ) * frame.width * 4;
			gatherRow< Pixel, Components, Maximum >( src, frame.bounds.x1, frame.bounds.x2, frame.bounds.y1 + row, frame.premultiplied, line );
			cpu::LinearRows( frame.uniforms, frame.source.data(), frame.width, row, row + 1, frame.linear.data() );
		}
	}
};

/// `kProcess`'s copy, over the render window, straight into the host's image.
template< typename Pixel, int Components, int Maximum >
class ProcessPass : public PassBase
{
public:
	using PassBase::PassBase;

	void multiThreadProcessImages( OfxRectI window ) override
	{
		std::vector< float > line( static_cast< size_t >( std::max( 0, window.x2 - window.x1 ) ) * 4 );
		const float clear[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			//The part of this row the source covers, in absolute pixels. The
			//output's region of definition is the source's, so outside it is
			//only ever reached by a host that asks for more than it was told.
			const bool rowInside = y >= frame.bounds.y1 && y < frame.bounds.y2;
			const int from       = rowInside ? std::max( window.x1, frame.bounds.x1 ) : window.x2;
			const int to         = rowInside ? std::min( window.x2, frame.bounds.x2 ) : window.x2;
			if( from < to )
				cpu::ProcessRow( frame.uniforms, frame.linear.data(), frame.source.data(), frame.width, frame.height, y - frame.bounds.y1,
				                 from - frame.bounds.x1, to - frame.bounds.x1, line.data() );

			for( int x = window.x1; x < window.x2; ++x )
			{
				Pixel* px = static_cast< Pixel* >( _dstImg->getPixelAddress( x, y ) );
				if( px == nullptr )
					continue;
				const float* value = ( x >= from && x < to ) ? line.data() + static_cast< size_t >( x - from ) * 4 : clear;
				scatterPixel< Pixel, Components, Maximum >( value, frame.premultiplied, px );
			}
		}
	}
};

class CcuOFXPlugin : public OFX::ImageEffect
{
public:
	explicit CcuOFXPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		masterGain  = fetchDoubleParam( kParamMasterGain );
		masterBlack = fetchDoubleParam( kParamMasterBlack );
		whiteClip   = fetchDoubleParam( kParamWhiteClip );
		rGain       = fetchDoubleParam( kParamRGain );
		bGain       = fetchDoubleParam( kParamBGain );
		drift       = fetchDoubleParam( kParamDrift );
		matrix      = fetchChoiceParam( kParamMatrix );
		saturation  = fetchDoubleParam( kParamSaturation );
		detailLevel = fetchDoubleParam( kParamDetailLevel );
		detailFreq  = fetchDoubleParam( kParamDetailFreq );
		hvRatio     = fetchDoubleParam( kParamHVRatio );
		coring      = fetchDoubleParam( kParamCoring );
		levelDep    = fetchDoubleParam( kParamLevelDep );
		skinDetail  = fetchDoubleParam( kParamSkinDetail );
		skinHue     = fetchDoubleParam( kParamSkinHue );
		skinWidth   = fetchDoubleParam( kParamSkinWidth );
		kneeOn      = fetchBooleanParam( kParamKneeOn );
		kneePoint   = fetchDoubleParam( kParamKneePoint );
		kneeSlope   = fetchDoubleParam( kParamKneeSlope );
		gamma       = fetchDoubleParam( kParamGamma );
		blackGamma  = fetchDoubleParam( kParamBlackGamma );
		mix         = fetchDoubleParam( kParamMix );
		showDetail  = fetchBooleanParam( kParamShowDetail );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );

		if( dst == nullptr || src == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		if( src->getPixelDepth() != depth || src->getPixelComponents() != comps )
			OFX::throwSuiteStatusException( kOfxStatErrImageFormat );

		Frame frame;
		frame.bounds = src->getBounds();
		frame.width  = frame.bounds.x2 - frame.bounds.x1;
		frame.height = frame.bounds.y2 - frame.bounds.y1;
		if( frame.width <= 0 || frame.height <= 0 )
			return;

		frame.uniforms = uniformsAt( args.time );

		//An RGB clip has no alpha to be premultiplied by, and a host that says
		//"unpremultiplied" about one is describing something that does not
		//exist. Opaque counts as premultiplied: alpha is 1, so it is exact.
		frame.premultiplied = comps == OFX::ePixelComponentRGBA && srcClip->getPreMultiplication() != OFX::eImageUnPreMultiplied;

		const size_t floats = static_cast< size_t >( frame.width ) * frame.height * 4;
		frame.source.resize( floats );
		frame.linear.resize( floats );

		switch( depth )
		{
			case OFX::eBitDepthUByte:
				comps == OFX::ePixelComponentRGBA ? run< unsigned char, 4, 255 >( args, frame, src.get(), dst.get() )
				                                  : run< unsigned char, 3, 255 >( args, frame, src.get(), dst.get() );
				break;
			case OFX::eBitDepthUShort:
				comps == OFX::ePixelComponentRGBA ? run< unsigned short, 4, 65535 >( args, frame, src.get(), dst.get() )
				                                  : run< unsigned short, 3, 65535 >( args, frame, src.get(), dst.get() );
				break;
			case OFX::eBitDepthFloat:
				comps == OFX::ePixelComponentRGBA ? run< float, 4, 1 >( args, frame, src.get(), dst.get() )
				                                  : run< float, 3, 1 >( args, frame, src.get(), dst.get() );
				break;
			default:
				OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;
	}

private:
	template< typename Pixel, int Components, int Maximum >
	void run( const OFX::RenderArguments& args, Frame& frame, const OFX::Image* src, OFX::Image* dst )
	{
		{
			//The full width, although the pass works in whole rows: the Support
			//library sizes its thread count from the window's area.
			LinearPass< Pixel, Components, Maximum > pass( *this, frame, src );
			pass.setRenderWindow( { 0, 0, frame.width, frame.height } );
			pass.process();
		}
		if( abort() )
			return;
		{
			ProcessPass< Pixel, Components, Maximum > pass( *this, frame, src );
			pass.setDstImg( dst );
			pass.setRenderWindow( args.renderWindow );
			pass.process();
		}
	}

	/// The frame's uniforms: every parameter at this time, and the drift's
	/// walk replayed to this frame (see the top of this file).
	chain::Uniforms uniformsAt( double time ) const
	{
		const auto at = [ time ]( OFX::DoubleParam* p ) { return static_cast< float >( p->getValueAtTime( time ) ); };

		chain::HostValues host;
		host.masterGain  = at( masterGain );
		host.masterBlack = at( masterBlack );
		host.whiteClip   = at( whiteClip );
		host.rGain       = at( rGain );
		host.bGain       = at( bGain );
		host.drift       = at( drift );
		{
			//ChoiceParam answers through an out parameter rather than a return
			//value, unlike every other param type in the Support library.
			int index = 0;
			matrix->getValueAtTime( time, index );
			host.matrix = static_cast< float >( index );
		}
		host.saturation  = at( saturation );
		host.detailLevel = at( detailLevel );
		host.detailFreq  = at( detailFreq );
		host.hvRatio     = at( hvRatio );
		host.coring      = at( coring );
		host.levelDep    = at( levelDep );
		host.skinDetail  = at( skinDetail );
		host.skinHue     = at( skinHue );
		host.skinWidth   = at( skinWidth );
		host.kneeOn      = kneeOn->getValueAtTime( time ) ? 1.0f : 0.0f;
		host.kneePoint   = at( kneePoint );
		host.kneeSlope   = at( kneeSlope );
		host.gamma       = at( gamma );
		host.blackGamma  = at( blackGamma );
		host.mix         = at( mix );
		host.showDetail  = showDetail->getValueAtTime( time ) ? 1.0f : 0.0f;

		//OFX time is in FRAMES. A sub-frame time (a motion-blur sample, a
		//field) belongs to the nearest frame: the walk has one value a frame,
		//as it does in the FFGL build.
		const int64_t frameNumber = static_cast< int64_t >( std::floor( time + 0.5 ) );
		return chain::Resolve( host, model::DriftWalkAt( frameNumber, 1.0 / framesPerSecond() ) );
	}

	/// The clip's rate, for the drift's dt. A host that reports zero -- some
	/// do for a clip with nothing connected -- would otherwise divide by it.
	double framesPerSecond() const
	{
		double fps = srcClip != nullptr ? srcClip->getFrameRate() : 0.0;
		if( !( fps > 0.0 ) )
			fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 25.0;
		return fps;
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::DoubleParam* masterGain  = nullptr;
	OFX::DoubleParam* masterBlack = nullptr;
	OFX::DoubleParam* whiteClip   = nullptr;
	OFX::DoubleParam* rGain       = nullptr;
	OFX::DoubleParam* bGain       = nullptr;
	OFX::DoubleParam* drift       = nullptr;
	OFX::ChoiceParam* matrix      = nullptr;
	OFX::DoubleParam* saturation  = nullptr;
	OFX::DoubleParam* detailLevel = nullptr;
	OFX::DoubleParam* detailFreq  = nullptr;
	OFX::DoubleParam* hvRatio     = nullptr;
	OFX::DoubleParam* coring      = nullptr;
	OFX::DoubleParam* levelDep    = nullptr;
	OFX::DoubleParam* skinDetail  = nullptr;
	OFX::DoubleParam* skinHue     = nullptr;
	OFX::DoubleParam* skinWidth   = nullptr;
	OFX::BooleanParam* kneeOn     = nullptr;
	OFX::DoubleParam* kneePoint   = nullptr;
	OFX::DoubleParam* kneeSlope   = nullptr;
	OFX::DoubleParam* gamma       = nullptr;
	OFX::DoubleParam* blackGamma  = nullptr;
	OFX::DoubleParam* mix         = nullptr;
	OFX::BooleanParam* showDetail = nullptr;
};

//---------------------------------------------------------------------------
// Parameter declaration helpers.
//---------------------------------------------------------------------------
OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const char* name,
                                        const char* label )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( label, label, label );
	page->addChild( *group );
	return group;
}

void defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const char* label, const char* hint, double value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
}

void defineToggle( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const char* label, const char* hint, bool value )
{
	OFX::BooleanParamDescriptor* param = desc.defineBooleanParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
}

mDeclarePluginFactory( CcuPluginFactory, {}, {} );
} // namespace

void CcuPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The detail kernel reads up to ten pixels away, so no tiles. Frames are
	// independent of each other and of render order: the drift is replayed
	// from the frame number, and nothing else carries across frames.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
	desc.setSupportsMultipleClipPARs( false );
	desc.setSupportsMultipleClipDepths( false );
}

void CcuPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults and the same groups as
	// the FFGL build -- the defaults are literally the same struct -- so the
	// two inspectors read identically and one guide covers both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const chain::HostValues d;

	//--------------------------------------------------------------- Exposure
	OFX::GroupParamDescriptor* exposure = defineGroup( desc, page, "exposureGroup", "Exposure" );
	defineSlider( desc, page, exposure, kParamMasterGain, "Master Gain",
	              "Head-end gain in linear light, before white balance: -6 to +18 dB, 0 dB at a quarter.", d.masterGain );
	defineSlider( desc, page, exposure, kParamMasterBlack, "Master Black",
	              "Pedestal, added on video: -0.05 to +0.15, 0 at a quarter.", d.masterBlack );
	defineSlider( desc, page, exposure, kParamWhiteClip, "White Clip",
	              "The video level nothing exceeds: 0.85 to 1.15, exactly 1 at the middle.", d.whiteClip );

	//------------------------------------------------------------------ White
	OFX::GroupParamDescriptor* white = defineGroup( desc, page, "whiteGroup", "White" );
	defineSlider( desc, page, white, kParamRGain, "R Gain", "Red gain about green: +-6 dB, 0 dB at the middle.", d.rGain );
	defineSlider( desc, page, white, kParamBGain, "B Gain", "Blue gain about green: +-6 dB, 0 dB at the middle.", d.bGain );
	defineSlider( desc, page, white, kParamDrift, "Drift",
	              "A slow random walk of the colour temperature, as a warming-up camera wanders: 20 s time constant, up "
	              "to 60 mireds RMS. A function of the frame number here, so every render of a frame drifts the same.",
	              d.drift );

	//----------------------------------------------------------------- Matrix
	OFX::GroupParamDescriptor* matrixGroup = defineGroup( desc, page, "matrixGroup", "Matrix" );
	{
		OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam( kParamMatrix );
		param->setLabels( "Matrix", "Matrix", "Matrix" );
		param->setHint( "The colour matrix in linear light. Identity; Standard (SMPTE-C primaries to BT.709); High "
		                "Saturation; Film-like. Saturation composes on top." );
		//Enum order, not alphabetical: it reads as a progression from "off".
		for( int i = 0; i < model::kMatrixCount; ++i )
			param->appendOption( model::kMatrixNames[ i ] );
		param->setDefault( model::OptionIndex( d.matrix, model::kMatrixCount ) );
		param->setAnimates( false );
		param->setParent( *matrixGroup );
		page->addChild( *param );
	}
	defineSlider( desc, page, matrixGroup, kParamSaturation, "Saturation",
	              "0 to 2 about BT.709 luma, white-preserving; exactly 1 at the middle.", d.saturation );

	//----------------------------------------------------------------- Detail
	OFX::GroupParamDescriptor* detail = defineGroup( desc, page, "detailGroup", "Detail" );
	defineSlider( desc, page, detail, kParamDetailLevel, "Detail Level",
	              "The gain on the detail signal, 0 to 3. A step of height h overshoots by Level x h / 4.", d.detailLevel );
	defineSlider( desc, page, detail, kParamDetailFreq, "Crispening Freq",
	              "The detail delay, 1 to 9 pixels -- the width of the halo, not of the edge. Whole at every eighth.",
	              d.detailFreq );
	defineSlider( desc, page, detail, kParamHVRatio, "H/V Ratio",
	              "Vertical detail only at 0, horizontal only at 1, both at the middle.", d.hvRatio );
	defineSlider( desc, page, detail, kParamCoring, "Coring",
	              "The edge height below which there is no detail at all, 0 to 0.25 linear. Low sharpens the grain; high "
	              "plastic-coats the picture.",
	              d.coring );
	defineSlider( desc, page, detail, kParamLevelDep, "Level Dependence", "How far detail is reduced in the shadows.",
	              d.levelDep );
	defineSlider( desc, page, detail, kParamSkinDetail, "Skin Detail",
	              "The detail gain inside the skin window is 1 minus this: faces go soft while the jacket stays sharp.",
	              d.skinDetail );
	defineSlider( desc, page, detail, kParamSkinHue, "Skin Hue", "The skin window's centre, 0 to 360 degrees of hue.",
	              d.skinHue );
	defineSlider( desc, page, detail, kParamSkinWidth, "Skin Width", "The skin window's half-width, 5 to 60 degrees.",
	              d.skinWidth );

	//------------------------------------------------------------------- Knee
	OFX::GroupParamDescriptor* kneeGroup = defineGroup( desc, page, "kneeGroup", "Knee" );
	defineToggle( desc, page, kneeGroup, kParamKneeOn, "Knee On",
	              "Compress highlights above the knee point, per channel. Off, they clip -- and a warm white clips in red first.",
	              d.kneeOn >= 0.5f );
	defineSlider( desc, page, kneeGroup, kParamKneePoint, "Knee Point", "Where the knee starts: 0.4 to 1.0 in linear light.",
	              d.kneePoint );
	defineSlider( desc, page, kneeGroup, kParamKneeSlope, "Knee Slope",
	              "The slope above the point, 0.05 to 1; continuous at the point.", d.kneeSlope );

	//------------------------------------------------------------------ Gamma
	OFX::GroupParamDescriptor* gammaGroup = defineGroup( desc, page, "gammaGroup", "Gamma" );
	defineSlider( desc, page, gammaGroup, kParamGamma, "Gamma",
	              "The OETF's exponent, 0.35 to 0.55; BT.709's 0.45 at the middle.", d.gamma );
	defineSlider( desc, page, gammaGroup, kParamBlackGamma, "Black Gamma",
	              "A lift of the shadows below 0.25 video, and nowhere else.", d.blackGamma );

	//----------------------------------------------------------------- Output
	OFX::GroupParamDescriptor* output = defineGroup( desc, page, "outputGroup", "Output" );
	defineSlider( desc, page, output, kParamMix, "Mix", "Wet/dry against the untouched input.", d.mix );
	defineToggle( desc, page, output, kParamShowDetail, "Show Detail",
	              "The detail signal about mid grey, for setting up Coring and the skin window.", d.showDetail >= 0.5f );

	// The Stoatworks About block: a read-only credit line and one push button per
	// link, in a group that starts folded. Last, so it sits under the effect's
	// own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* CcuPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new CcuOFXPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static CcuPluginFactory* factory = new CcuPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
