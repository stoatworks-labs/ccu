/**
	cctest -- render CCU offline, and read the camera chain back out of it.

	Every check here drives the REAL plugin class through a headless GL
	context and measures the answer out of the picture it made:

		cctest --out /tmp/frame.png     a picture, on the test card
		cctest --list                   every parameter, its kind and default
		cctest --identity               every stage at its null returns the
		                                input, within a bound DERIVED from the
		                                OETF round trip in float; alpha bitwise;
		                                and again after a resize mid-run
		cctest --detail                 on a step of height h the overshoot is
		                                Detail Level x h / 4 and it lasts exactly
		                                the kernel spacing; whole and fractional
		                                spacings separately; H and V
		cctest --coring                 an edge below Coring gets no detail; one
		                                above gets the overshoot less the dead
		                                zone
		cctest --knee                   above the knee point a linear ramp's
		                                slope is Knee Slope, continuous at the
		                                point, per channel
		cctest --gamma                  a ramp follows the OETF at three
		                                exponents (and the null's constants are
		                                BT.709's); Black Gamma lifts only below
		                                its level
		cctest --order                  a warm white through WB then the knee
		                                compresses in R; the knee-first chain
		                                predicts something else; the plugin
		                                matches the first; with the knee off, R
		                                clips first
		cctest --skin                   the detail gain inside the skin window
		                                is 1 - Skin Detail, outside it 1, and a
		                                neutral is never skin
		cctest --negative               every check above can FAIL
		cctest --cpu                    the OpenFX build's CPU copy of the
		                                two passes (CpuChain.cpp) against
		                                the GPU, picture for picture, at the
		                                defaults, at settings that move every
		                                stage, at a drifting frame and under
		                                every perturbation; and a control
		                                case that must differ
		cctest --bench-cpu              that copy's cost, no GL
		cctest --laws --names           the checks that need no GL
		cctest --bench                  the render cost
		cctest --dump-shaders DIR       the exact GLSL the plugin compiles
		cctest --pipe                   raw frames in, raw frames out

	The control laws, the OETF family, the kernel, the coring, the window and
	the drift are stated HERE from their definitions (Controls.h's comments
	and Model.h's description), and never read out of the plugin: a constant
	typed wrong there has to show up as a failed check, not as an agreement.
	AGENTS.md has one line per check on where each tolerance comes from.
*/

#include "Ccu.h"
#include "Chain.h"
#include "Controls.h"
#include "CpuChain.h"
#include "Model.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model = ccu::model;

int g_checks   = 0;
int g_failures = 0;

constexpr double kU   = 5.9604644775390625e-8;//2^-24: half a float ulp at 1, the relative rounding unit
constexpr double kLn2 = 0.6931471805599453;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	std::FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The laws, stated from their definitions. A check rounds the value it hands
// the plugin through float where the plugin hands it to the GPU as a float
// uniform.
//---------------------------------------------------------------------------
double unit( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double asShader( double v )
{
	return static_cast< double >( static_cast< float >( v ) );
}
double statedMasterGainDb( float v )
{
	return 24.0 * ( unit( v ) - 0.25 );
}
double statedMasterBlack( float v )
{
	return 0.2 * ( unit( v ) - 0.25 );
}
double statedWhiteClip( float v )
{
	return 1.0 + 0.3 * ( unit( v ) - 0.5 );
}
double statedChannelGainDb( float v )
{
	return 12.0 * ( unit( v ) - 0.5 );
}
double statedDriftMireds( float v )
{
	return 60.0 * unit( v );
}
double statedSaturation( float v )
{
	return 2.0 * unit( v );
}
double statedDetailLevel( float v )
{
	return 3.0 * unit( v );
}
double statedSpacing( float v )
{
	return 1.0 + 8.0 * unit( v );
}
double statedHWeight( float v )
{
	return std::min( 1.0, 2.0 * unit( v ) );
}
double statedVWeight( float v )
{
	return std::min( 1.0, 2.0 * ( 1.0 - unit( v ) ) );
}
double statedCoringEdge( float v )
{
	return 0.25 * unit( v ) * unit( v );
}
double statedSkinHue( float v )
{
	return 360.0 * unit( v );
}
double statedSkinWidth( float v )
{
	return 5.0 + 55.0 * unit( v );
}
double statedKneePoint( float v )
{
	return 0.4 + 0.6 * unit( v );
}
double statedKneeSlope( float v )
{
	return 1.0 - 0.95 * ( 1.0 - unit( v ) );
}
double statedGamma( float v )
{
	return 0.45 + 0.2 * ( unit( v ) - 0.5 );
}

/// The OETF family: a and k from the exponent by continuity of value and
/// slope at b = 0.018 (Model.h). Stated again here, on purpose.
struct StatedOetf
{
	double g, a, k, knee;
};
StatedOetf statedOetf( double g )
{
	StatedOetf o;
	o.g          = g;
	const double bg = std::pow( 0.018, g );
	o.a          = 1.0 / ( 1.0 - ( 1.0 - g ) * bg );
	o.k          = o.a * g * std::pow( 0.018, g - 1.0 );
	o.knee       = o.k * 0.018;
	return o;
}
/// Through the FLOAT constants the plugin uploads.
double statedEncode( const StatedOetf& o, double L )
{
	L = std::max( L, 0.0 );
	if( L < 0.018 )
		return asShader( o.k ) * L;
	return asShader( o.a ) * std::pow( L, asShader( o.g ) ) - asShader( o.a - 1.0 );
}
double statedDecode( const StatedOetf& o, double V )
{
	V = std::max( V, 0.0 );
	if( V < asShader( o.knee ) )
		return V / asShader( o.k );
	return std::pow( ( V + asShader( o.a - 1.0 ) ) / asShader( o.a ), asShader( 1.0 / o.g ) );
}
const StatedOetf kNull = statedOetf( 0.45 );

double statedKnee( double c, double point, double slope )
{
	return c > point ? point + ( c - point ) * slope : c;
}
double statedBlackGamma( double bg, double V )
{
	if( V >= 0.25 )
		return 0.0;
	const double u = V / 0.25;
	return bg * 1.5 * 0.25 * u * ( 1.0 - u ) * ( 1.0 - u );
}
double statedLuma( double r, double g, double b )
{
	return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

//---------------------------------------------------------------------------
// The tolerance, derived. GLSL 4.10 section 8.2: pow( x, y ) is inherited
// from exp2( y * log2( x ) ); log2 has 3 ULP outside [ 0.5, 2 ] and an
// absolute error under 2^-21 inside it; exp2 has 3 ULP; a multiply is
// correctly rounded. One ULP is 2 kU relative. Each term below is one
// operation of the chain. Nothing here was fitted to a number this GPU
// printed.
//---------------------------------------------------------------------------
double ulpOf( double x )
{
	x = std::fabs( x );
	if( x == 0.0 )
		return std::ldexp( 1.0, -149 );
	return std::ldexp( 1.0, static_cast< int >( std::floor( std::log2( x ) ) ) - 23 );
}
double log2Error( double x, double relIn )
{
	const double l = std::log2( x );
	return ( x >= 0.5 && x <= 2.0 ? std::ldexp( 1.0, -21 ) : 3.0 * ulpOf( l ) ) + relIn / kLn2;
}
/// Relative error bound on pow( x, y ) in float given a relative error
/// already in x, and y itself a rounded float.
double powRelError( double x, double y, double relIn )
{
	const double l  = std::log2( x );
	const double m  = y * l;
	const double eM = std::fabs( y ) * log2Error( x, relIn ) + kU * std::fabs( m ) + 0.5 * ulpOf( m );
	return kLn2 * eM + 6.0 * kU;
}
/// Relative error bound on the decode of video V (at the null exponent).
double decodeRelError( double V )
{
	if( V < kNull.knee )
		return 2.0 * kU;//one divide, one rounded uniform
	const double t = ( V + ( kNull.a - 1.0 ) ) / kNull.a;
	return powRelError( t, 1.0 / kNull.g, 4.0 * kU );//the add, the divide, two rounded uniforms
}
/// Absolute error bound on the encode of linear L carrying a relative
/// error already, at exponent o.
double encodeAbsError( double L, double relIn, const StatedOetf& o )
{
	if( L <= 0.0 )
		return 2.0 * kU;
	if( L < 0.018 )
		return o.k * L * ( relIn + 2.0 * kU ) + 2.0 * kU;
	const double Vp = o.a * std::pow( L, o.g );
	return Vp * ( powRelError( L, o.g, relIn ) + 2.0 * kU ) + 2.0 * kU;//the multiply by a, the subtract of c
}
/// The identity round trip's bound at video V: decode, store as float,
/// encode. The tolerance a check uses is twice this plus four kU.
double roundTripBound( double V, double gain = 1.0, const StatedOetf& out = kNull )
{
	const double L = statedDecode( kNull, V ) * gain;
	return encodeAbsError( L, decodeRelError( V ) + 2.0 * kU + ( gain != 1.0 ? 2.0 * kU : 0.0 ), out );
}
double tolerance( double V, double gain = 1.0, const StatedOetf& out = kNull )
{
	return 2.0 * roundTripBound( V, gain, out ) + 4.0 * kU;
}
/// dV/dL of the OETF at L: what an error in linear light becomes in video.
double encodeSlope( double L, const StatedOetf& o = kNull )
{
	if( L < 0.018 )
		return o.k;
	return o.a * o.g * std::pow( std::max( L, 1e-9 ), o.g - 1.0 );
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first.
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture flat( int W, int H, double r, double g, double b, double a = 1.0 )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ]     = static_cast< float >( r );
		p[ i + 1 ] = static_cast< float >( g );
		p[ i + 2 ] = static_cast< float >( b );
		p[ i + 3 ] = static_cast< float >( a );
	}
	return p;
}
Picture flat( int W, int H, double level )
{
	return flat( W, H, level, level, level );
}

void paint( Picture& p, int W, int H, int x0, int y0, int x1, int y1, double r, double g, double b )
{
	for( int y = std::max( 0, y0 ); y < std::min( H, y1 ); ++y )
		for( int x = std::max( 0, x0 ); x < std::min( W, x1 ); ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			px[ 0 ]   = static_cast< float >( r );
			px[ 1 ]   = static_cast< float >( g );
			px[ 2 ]   = static_cast< float >( b );
		}
}
void paint( Picture& p, int W, int H, int x0, int y0, int x1, int y1, double level )
{
	paint( p, W, H, x0, y0, x1, y1, level, level, level );
}

float at( const std::vector< float >& img, int W, int r, int c, int ch = 0 )
{
	return img[ ( static_cast< size_t >( r ) * W + c ) * 4 + ch ];
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Ccu::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Ccu& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Ccu::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range; an integer's range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Ccu& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

/// Options, booleans and integers STEP between cues; standard sliders ramp.
bool stepsBetweenCues( Ccu& plugin, unsigned int index )
{
	const unsigned int type = plugin.GetParamType( index );
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_INTEGER;
}

bool applySetting( Ccu& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Ccu& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

/// Every control a check can move, as the sliders the plugin sees. The
/// defaults here are the NULL chain: every stage at the position that is
/// exactly its identity. Each check moves the one thing it measures.
struct Knobs
{
	float masterGain  = 0.25f;//0 dB
	float masterBlack = 0.25f;//0
	float whiteClip   = 0.5f; //1.0
	float rGain       = 0.5f; //0 dB
	float bGain       = 0.5f;
	float drift       = 0.0f;
	int matrix        = model::kMatrixIdentity;
	float saturation  = 0.5f; //1
	float detailLevel = 0.0f;
	float detailFreq  = 0.125f;//2 px
	float hvRatio     = 0.5f;
	float coring      = 0.0f;
	float levelDep    = 0.0f;
	float skinDetail  = 0.0f;
	float skinHue     = 0.0556f;
	float skinWidth   = 0.2727f;
	bool kneeOn       = false;
	float kneePoint   = 0.5f;
	float kneeSlope   = 0.25f;
	float gamma       = 0.5f; //0.45
	float blackGamma  = 0.0f;
	float mix         = 1.0f;
	bool showDetail   = false;
};

void apply( Ccu& p, const Knobs& k )
{
	set( p, "Master Gain", k.masterGain );
	set( p, "Master Black", k.masterBlack );
	set( p, "White Clip", k.whiteClip );
	set( p, "R Gain", k.rGain );
	set( p, "B Gain", k.bGain );
	set( p, "Drift", k.drift );
	set( p, "Matrix", static_cast< float >( k.matrix ) );
	set( p, "Saturation", k.saturation );
	set( p, "Detail Level", k.detailLevel );
	set( p, "Crispening Freq", k.detailFreq );
	set( p, "H/V Ratio", k.hvRatio );
	set( p, "Coring", k.coring );
	set( p, "Level Dependence", k.levelDep );
	set( p, "Skin Detail", k.skinDetail );
	set( p, "Skin Hue", k.skinHue );
	set( p, "Skin Width", k.skinWidth );
	set( p, "Knee On", k.kneeOn ? 1.0f : 0.0f );
	set( p, "Knee Point", k.kneePoint );
	set( p, "Knee Slope", k.kneeSlope );
	set( p, "Gamma", k.gamma );
	set( p, "Black Gamma", k.blackGamma );
	set( p, "Mix", k.mix );
	set( p, "Show Detail", k.showDetail ? 1.0f : 0.0f );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and a synthetic 60 fps clock
// declared in seconds.
//---------------------------------------------------------------------------
struct Session
{
	Ccu plugin;
	int width        = 0;
	int height       = 0;
	bool floatOutput = true;
	double fps       = 60.0;
	int64_t frame    = 0;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		plugin.SetClockSecondsForTest();
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip changes size: the SAME instance handed
	/// a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	bool renderNow()
	{
		plugin.SetTime( static_cast< double >( frame ) / fps );
		++frame;
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed\n" );
		return ok;
	}

	bool render( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderNow();
	}

	bool render( const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderNow();
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

/// One frame of one picture through a fresh session, read back as floats.
bool renderOnce( const Knobs& k, int perturb, int W, int H, const Picture& pic, std::vector< float >& out )
{
	Session s;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	if( !s.begin( W, H ) || !s.render( pic ) )
		return false;
	out = s.readBackFloat();
	s.end();
	return true;
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( quiet )
		return ok ? 0 : 1;
	va_list args;
	va_start( args, format );
	std::printf( "   %-4s ", verdict( ok ) );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
	return ok ? 0 : 1;
}

/// Worst |got - predicted| over a run of samples, with where.
struct Worst
{
	double error = 0.0, ratio = 0.0;
	int where = -1;
	void take( double e, double tol, int i )
	{
		if( e > error )
		{
			error = e;
			where = i;
		}
		ratio = std::max( ratio, tol > 0.0 ? e / tol : 0.0 );
	}
	bool ok() const
	{
		return ratio <= 1.0;
	}
};

//---------------------------------------------------------------------------
// --identity
//---------------------------------------------------------------------------
Picture identityPicture( int W, int H )
{
	//Every channel its own ramp, alpha its own, so a channel swap or a
	//dropped alpha is visible; the ramps cover 0..1 at both segments of the
	//OETF.
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			float* px      = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			const double u = ( x + 0.5 ) / W, v = ( y + 0.5 ) / H;
			px[ 0 ]        = static_cast< float >( u );
			px[ 1 ]        = static_cast< float >( 1.0 - u * 0.7 );
			px[ 2 ]        = static_cast< float >( v );
			px[ 3 ]        = static_cast< float >( 0.25 + 0.75 * v );
		}
	return p;
}

int identityCompare( const Picture& pic, const std::vector< float >& out, int W, int H, bool quiet, const char* what )
{
	Worst worst;
	double maxTol = 0.0, alphaWorst = 0.0;
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
			for( int ch = 0; ch < 3; ++ch )
			{
				const double in  = at( pic, W, y, x, ch );
				const double got = at( out, W, y, x, ch );
				const double tol = tolerance( in );
				maxTol           = std::max( maxTol, tol );
				worst.take( std::fabs( got - in ), tol, y * W + x );
			}
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
			alphaWorst = std::max( alphaWorst, static_cast< double >( std::fabs( at( pic, W, y, x, 3 ) - at( out, W, y, x, 3 ) ) ) );
	int failed = 0;
	failed += report( worst.ok(), quiet, "%s: worst error %.2e at pixel %d (%.2f of its derived tolerance; the largest tolerance is %.2e)", what, worst.error,
	                  worst.where, worst.ratio, maxTol );
	failed += report( alphaWorst == 0.0, quiet, "  alpha passes through bitwise (worst %.2e)", alphaWorst );
	//A bound that grew past this would be a bound somebody loosened: the
	//derivation gives about 3e-6 at white.
	failed += report( maxTol <= 2e-5, quiet, "  the derived tolerance is tight (%.2e at its largest, cap 2e-5)", maxTol );
	return failed;
}

int runIdentity( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== identity at %dx%d: every stage at its null returns the input\n", W, H );
	int failed = 0;
	const Picture pic = identityPicture( W, H );
	Knobs k;
	std::vector< float > out;
	if( !renderOnce( k, perturb, W, H, pic, out ) )
		return report( false, quiet, "render failed" );
	failed += identityCompare( pic, out, W, H, quiet, "the null chain" );

	//Saturation and the Standard matrix on a GREY leave it grey: white is
	//preserved by every matrix, so this is the identity again on the
	//neutral axis. On a saturation matrix the luma weights sum to one in
	//three float roundings, hence the extra term.
	{
		Picture grey = flat( W, H, 0.0 );
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
			{
				float* px = grey.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
				px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( ( x + 0.5 ) / W );
			}
		Knobs ks;
		ks.saturation = 0.9f;
		ks.matrix     = model::kMatrixHighSaturation;
		std::vector< float > outS;
		if( !renderOnce( ks, perturb, W, H, grey, outS ) )
			return report( false, quiet, "render failed" );
		Worst worst;
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
				for( int ch = 0; ch < 3; ++ch )
				{
					const double in  = at( grey, W, y, x, ch );
					const double L   = statedDecode( kNull, in );
					const double tol = tolerance( in ) + encodeSlope( L ) * L * 8.0 * kU;
					worst.take( std::fabs( at( outS, W, y, x, ch ) - in ), tol, y * W + x );
				}
		failed += report( worst.ok(), quiet, "a grey ramp through Saturation 1.8 and the High Saturation matrix is unchanged: worst %.2e (%.2f of tolerance)", worst.error, worst.ratio );
	}

	//The same instance, resized mid-run: the linear buffer reallocates.
	{
		const int W2 = W + 33, H2 = H + 11;
		Session s;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( W, H ) || !s.render( pic ) )
			return report( false, quiet, "render failed" );
		s.resize( W2, H2 );
		const Picture pic2 = identityPicture( W2, H2 );
		if( !s.render( pic2 ) )
			return report( false, quiet, "render failed after the resize" );
		const std::vector< float > out2 = s.readBackFloat();
		s.end();
		char what[ 64 ];
		std::snprintf( what, sizeof( what ), "resized mid-run to %dx%d", W2, H2 );
		failed += identityCompare( pic2, out2, W2, H2, quiet, what );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --detail
//
// The stated kernel on the stated taps, per column, and the two facts the
// spec names: the overshoot is Detail Level x h / 4, and it lasts the
// spacing. Measured on the Show Detail view, where the detail signal
// arrives about mid grey with no OETF in the way, AND on the real output
// through the chain.
//---------------------------------------------------------------------------
/// The detail signal at pixel index x along an edge profile Y[], for a
/// spacing i + f, with the picture's clamping.
double statedDetailAt( const std::vector< double >& Y, int x, int i, double f )
{
	const int n = static_cast< int >( Y.size() );
	auto Yc     = [ & ]( int j ) { return Y[ std::clamp( j, 0, n - 1 ) ]; };
	const double ff     = asShader( f );
	const double before = ( 1.0 - ff ) * Yc( x - i ) + ff * Yc( x - i - 1 );
	const double after  = ( 1.0 - ff ) * Yc( x + i ) + ff * Yc( x + i + 1 );
	return 0.5 * Yc( x ) - 0.25 * ( before + after );
}

struct EdgeCase
{
	const char* name;
	float freq;    //the Crispening Freq slider
	bool vertical; //a vertical edge (columns change) -> horizontal detail
};

int runDetail( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== detail at %dx%d: overshoot = Detail Level x h / 4, for exactly the spacing\n", W, H );
	int failed = 0;
	const float levelSlider = 1.0f / 3.0f;//Detail Level 1.0
	const double level      = asShader( statedDetailLevel( levelSlider ) );
	const double Vlo = 0.3, Vhi = 0.7;
	const double Llo = asShader( statedDecode( kNull, asShader( Vlo ) ) ), Lhi = asShader( statedDecode( kNull, asShader( Vhi ) ) );
	const double h = Lhi - Llo;

	const EdgeCase cases[] = {
		{ "whole spacing 2 px, H", 0.125f, true },
		{ "fractional spacing 2.5 px, H", 0.1875f, true },
		{ "whole spacing 3 px, V", 0.25f, false },
		{ "fractional spacing 1.5 px, V", 0.0625f, false },
	};
	for( const EdgeCase& c : cases )
	{
		const double s = statedSpacing( c.freq );
		const int i    = static_cast< int >( std::floor( s ) );
		const double f = s - i;
		const int n    = c.vertical ? W : H;
		const int x0   = n / 2;
		Picture pic    = flat( W, H, Vlo );
		if( c.vertical )
			paint( pic, W, H, x0, 0, W, H, Vhi );
		else
			paint( pic, W, H, 0, x0, W, H, Vhi );
		std::vector< double > Y( n );
		for( int x = 0; x < n; ++x )
			Y[ x ] = x < x0 ? Llo : Lhi;

		Knobs k;
		k.detailLevel = levelSlider;
		k.detailFreq  = c.freq;
		k.hvRatio     = c.vertical ? 1.0f : 0.0f;
		std::vector< float > out, view;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );
		k.showDetail = true;
		if( !renderOnce( k, perturb, W, H, pic, view ) )
			return report( false, quiet, "render failed" );

		//The Show Detail view: the kernel alone. Tolerance: the luma dot
		//(3 roundings), the two lerps (3 each), the kernel (3), the level
		//(1), the 0.5 (1): 14 roundings of values at or below 1, 28 kU,
		//taken as 32 kU.
		const double tolView = 32.0 * kU;
		const int mid        = c.vertical ? H / 2 : W / 2;
		auto got             = [ & ]( const std::vector< float >& img, int x ) { return c.vertical ? at( img, W, mid, x ) : at( img, W, x, mid ); };
		Worst wv, wo;
		int brightRun = 0, predBrightRun = 0;
		for( int x = std::max( 0, x0 - 2 * i - 4 ); x < std::min( n, x0 + 2 * i + 4 ); ++x )
		{
			const double d = statedDetailAt( Y, x, i, f );
			const double D = level * d;
			wv.take( std::fabs( ( got( view, x ) - 0.5 ) - D ), tolView, x );
			if( x >= x0 && std::fabs( D ) > tolView )
				++predBrightRun;
			if( x >= x0 && std::fabs( got( view, x ) - 0.5 ) > tolView )
				++brightRun;

			//Through the chain: the detail lands in linear light and goes
			//through the OETF, so the view's error is scaled by its slope.
			const double Lout = Y[ x ] + D;
			const double pred = statedEncode( kNull, Lout );
			const double tol  = tolerance( x < x0 ? Vlo : Vhi ) + encodeSlope( Lout ) * ( level * 16.0 * kU + 4.0 * kU ) + 4.0 * kU;
			wo.take( std::fabs( got( out, x ) - pred ), tol, x );
		}
		const double peak     = got( view, x0 ) - 0.5;
		const double predPeak = level * 0.25 * h;
		const double under    = got( view, x0 - 1 ) - 0.5;
		failed += report( wv.ok(), quiet, "%s: Show Detail matches the kernel per pixel, worst %.2e (%.2f of %.1e)", c.name, wv.error, wv.ratio, tolView );
		failed += report( std::fabs( peak - predPeak ) <= tolView && std::fabs( under + predPeak ) <= tolView, quiet,
		                  "  overshoot %.6f / undershoot %.6f against Level x h / 4 = %.6f (h = %.4f linear)", peak, under, predPeak, h );
		failed += report( brightRun == predBrightRun && brightRun == static_cast< int >( std::ceil( s ) ), quiet,
		                  "  the overshoot lasts %d pixels: ceil( spacing %.2f ) = %d, predicted %d", brightRun, s, static_cast< int >( std::ceil( s ) ), predBrightRun );
		if( f > 0.0 )
			failed += report( std::fabs( ( got( view, x0 + i ) - 0.5 ) - level * 0.25 * f * h ) <= tolView, quiet,
			                  "  the fractional pixel at +%d carries f x that: %.6f against %.6f", i, got( view, x0 + i ) - 0.5, level * 0.25 * f * h );
		failed += report( wo.ok(), quiet, "  the output matches the chain per pixel, worst %.2e (%.2f of tolerance)", wo.error, wo.ratio );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --coring
//---------------------------------------------------------------------------
int runCoring( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== coring at %dx%d: an edge below Coring gets no detail, one above gets the overshoot less the dead zone\n", W, H );
	int failed = 0;
	const float levelSlider = 1.0f / 3.0f;
	const double level      = asShader( statedDetailLevel( levelSlider ) );
	const float coringSlider = 0.4f;
	const double c           = statedCoringEdge( coringSlider );//0.04 of linear edge height
	const double dead        = asShader( c * 0.25 );
	const double Vlo         = 0.3;
	const double Llo         = asShader( statedDecode( kNull, asShader( Vlo ) ) );
	const int i = 2;
	const int x0 = W / 2;
	const double tolView = 32.0 * kU;

	struct Edge
	{
		const char* name;
		double h;
	};
	const Edge edges[] = { { "an edge of 0.02 (below Coring 0.04)", 0.02 }, { "an edge of 0.20 (above it)", 0.20 } };
	for( const Edge& e : edges )
	{
		const double Lhi = asShader( Llo + e.h );
		const double Vhi = asShader( statedEncode( kNull, Lhi ) );
		Picture pic      = flat( W, H, Vlo );
		paint( pic, W, H, x0, 0, W, H, Vhi );
		std::vector< double > Y( W );
		for( int x = 0; x < W; ++x )
			Y[ x ] = x < x0 ? Llo : asShader( statedDecode( kNull, Vhi ) );
		const double hReal = Y[ x0 ] - Llo;

		Knobs k;
		k.detailLevel = levelSlider;
		k.detailFreq  = 0.125f;
		k.hvRatio     = 1.0f;
		k.coring      = coringSlider;
		k.showDetail  = true;
		std::vector< float > view;
		if( !renderOnce( k, perturb, W, H, pic, view ) )
			return report( false, quiet, "render failed" );

		Worst wv;
		for( int x = x0 - 2 * i - 3; x < x0 + 2 * i + 3; ++x )
		{
			const double d  = statedDetailAt( Y, x, i, 0.0 );
			const double dc = ( d > 0 ? 1.0 : ( d < 0 ? -1.0 : 0.0 ) ) * std::max( std::fabs( d ) - dead, 0.0 );
			wv.take( std::fabs( ( at( view, W, H / 2, x ) - 0.5 ) - level * dc ), tolView, x );
		}
		const double peak     = at( view, W, H / 2, x0 ) - 0.5;
		const double predPeak = std::max( 0.0, level * 0.25 * ( hReal - c ) );
		failed += report( wv.ok(), quiet, "%s: per pixel worst %.2e (%.2f of tolerance)", e.name, wv.error, wv.ratio );
		failed += report( std::fabs( peak - predPeak ) <= tolView, quiet, "  overshoot %.6f against Level x ( h - Coring ) / 4 = %.6f", peak, predPeak );
		if( e.h < c )
			failed += report( std::fabs( peak ) <= tolView && std::fabs( at( view, W, H / 2, x0 - 1 ) - 0.5 ) <= tolView, quiet,
			                  "  no overshoot and no undershoot at all (%.2e, %.2e); uncored it would be %.4f", peak, at( view, W, H / 2, x0 - 1 ) - 0.5, level * 0.25 * hReal );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --knee
//---------------------------------------------------------------------------
int runKnee( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== knee at %dx%d: above the point the slope is Knee Slope, continuous at the point, per channel\n", W, H );
	int failed = 0;
	const float pointSlider = 0.0f, slopeSlider = 0.2f;
	const double point = asShader( statedKneePoint( pointSlider ) );//0.4
	const double slope = asShader( statedKneeSlope( slopeSlider ) );//0.24

	//A ramp in LINEAR light 0..1, handed to the plugin as video.
	Picture pic = flat( W, H, 0.0 );
	std::vector< double > L( W ), Vin( W );
	for( int x = 0; x < W; ++x )
	{
		L[ x ]   = ( x + 0.5 ) / W;
		Vin[ x ] = asShader( statedEncode( kNull, L[ x ] ) );
		L[ x ]   = asShader( statedDecode( kNull, Vin[ x ] ) );//what the plugin will see
		paint( pic, W, H, x, 0, x + 1, H, Vin[ x ] );
	}

	Knobs k;
	k.kneeOn    = true;
	k.kneePoint = pointSlider;
	k.kneeSlope = slopeSlider;
	std::vector< float > out;
	if( !renderOnce( k, perturb, W, H, pic, out ) )
		return report( false, quiet, "render failed" );

	Worst w;
	std::vector< double > Lout( W ), tolL( W );
	for( int x = 0; x < W; ++x )
	{
		const double pred = statedEncode( kNull, statedKnee( L[ x ], point, slope ) );
		//The knee is a subtract, a multiply and an add on values below 1.
		const double tol  = tolerance( Vin[ x ] ) + encodeSlope( statedKnee( L[ x ], point, slope ) ) * 6.0 * kU;
		w.take( std::fabs( at( out, W, H / 2, x ) - pred ), tol, x );
		Lout[ x ] = statedDecode( kNull, at( out, W, H / 2, x ) );
		tolL[ x ] = tol / encodeSlope( std::max( statedKnee( L[ x ], point, slope ), 0.018 ) ) + 4.0 * kU;
	}
	failed += report( w.ok(), quiet, "the ramp matches point %.2f slope %.4f per column: worst %.2e (%.2f of tolerance)", point, slope, w.error, w.ratio );

	//The slope, measured in linear light between columns a quarter of the
	//ramp apart, both above the point.
	{
		int x1 = static_cast< int >( std::ceil( ( point + 0.05 ) * W ) ), x2 = std::min( W - 1, x1 + W / 4 );
		const double measured = ( Lout[ x2 ] - Lout[ x1 ] ) / ( L[ x2 ] - L[ x1 ] );
		const double tol      = ( tolL[ x1 ] + tolL[ x2 ] ) / ( L[ x2 ] - L[ x1 ] );
		failed += report( std::fabs( measured - slope ) <= tol, quiet, "  slope above the point, columns %d..%d: %.5f against %.5f (+-%.1e)", x1, x2, measured, slope, tol );
		int y1 = static_cast< int >( ( point - 0.3 ) * W ), y2 = static_cast< int >( ( point - 0.05 ) * W );
		const double below = ( Lout[ y2 ] - Lout[ y1 ] ) / ( L[ y2 ] - L[ y1 ] );
		const double tolB  = ( tolL[ y1 ] + tolL[ y2 ] ) / ( L[ y2 ] - L[ y1 ] );
		failed += report( std::fabs( below - 1.0 ) <= tolB, quiet, "  slope below the point: %.5f against 1 (+-%.1e)", below, tolB );
	}
	//Continuity: no column-to-column step larger than the input's own step
	//(slope at most 1) plus two tolerances, anywhere -- including across
	//the point.
	{
		double worstJump = 0.0;
		int where        = 0;
		for( int x = 1; x < W; ++x )
		{
			const double allowed = ( L[ x ] - L[ x - 1 ] ) + tolL[ x ] + tolL[ x - 1 ];
			const double jump    = std::fabs( Lout[ x ] - Lout[ x - 1 ] );
			if( jump - allowed > worstJump )
			{
				worstJump = jump - allowed;
				where     = x;
			}
		}
		failed += report( worstJump <= 0.0, quiet, "  continuous at the point: no column steps more than the ramp does (worst excess %.2e at %d)", worstJump, where );
	}
	//Per channel: a patch with R above the point and G below.
	{
		const double Lr = 0.8, Lg = 0.3, Lb = 0.1;
		const double Vr = asShader( statedEncode( kNull, Lr ) ), Vg = asShader( statedEncode( kNull, Lg ) ), Vb = asShader( statedEncode( kNull, Lb ) );
		const Picture patch = flat( W, H, Vr, Vg, Vb );
		std::vector< float > o;
		if( !renderOnce( k, perturb, W, H, patch, o ) )
			return report( false, quiet, "render failed" );
		const double gotR = at( o, W, H / 2, W / 2, 0 ), gotG = at( o, W, H / 2, W / 2, 1 ), gotB = at( o, W, H / 2, W / 2, 2 );
		const double LrS = statedDecode( kNull, Vr ), LgS = statedDecode( kNull, Vg ), LbS = statedDecode( kNull, Vb );
		const double predR = statedEncode( kNull, statedKnee( LrS, point, slope ) );
		const double tolR  = tolerance( Vr ) + encodeSlope( statedKnee( LrS, point, slope ) ) * 6.0 * kU;
		failed += report( std::fabs( gotR - predR ) <= tolR && std::fabs( gotG - Vg ) <= tolerance( Vg ) && std::fabs( gotB - Vb ) <= tolerance( Vb ), quiet,
		                  "  per channel: R at 0.8 linear compressed to %.5f (predicted %.5f); G at 0.3 and B at 0.1 untouched (%.1e, %.1e)", gotR, predR,
		                  std::fabs( gotG - Vg ), std::fabs( gotB - Vb ) );
		failed += report( std::fabs( statedKnee( LgS, point, slope ) - LgS ) == 0.0 && LbS < point, quiet, "  (the check's own G and B really are below the point)" );
	}
	//Knee off: the ramp is the identity.
	{
		Knobs off;
		std::vector< float > o;
		if( !renderOnce( off, perturb, W, H, pic, o ) )
			return report( false, quiet, "render failed" );
		Worst wo;
		for( int x = 0; x < W; ++x )
			wo.take( std::fabs( at( o, W, H / 2, x ) - Vin[ x ] ), tolerance( Vin[ x ] ), x );
		failed += report( wo.ok(), quiet, "  Knee On off: the ramp is the identity, worst %.2e", wo.error );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --gamma
//---------------------------------------------------------------------------
int runGamma( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== gamma at %dx%d: a ramp follows the OETF; Black Gamma lifts only below its level\n", W, H );
	int failed = 0;
	Picture pic = flat( W, H, 0.0 );
	std::vector< double > Vin( W );
	for( int x = 0; x < W; ++x )
	{
		Vin[ x ] = asShader( ( x + 0.5 ) / W );
		paint( pic, W, H, x, 0, x + 1, H, Vin[ x ] );
	}

	//1. The OETF at the null exponent, exercised above the identity by 6 dB
	//   of gain: V_out = OETF( g L ), both segments, clipped at 1.15.
	{
		Knobs k;
		k.masterGain = 0.5f;//+6 dB
		k.whiteClip  = 1.0f;//1.15
		const double g = asShader( std::pow( 10.0, statedMasterGainDb( 0.5f ) / 20.0 ) );
		const double clip = asShader( statedWhiteClip( 1.0f ) );
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );
		Worst w;
		int linearColumns = 0;
		for( int x = 0; x < W; ++x )
		{
			const double L    = asShader( statedDecode( kNull, Vin[ x ] ) ) * g;
			const double pred = std::min( clip, statedEncode( kNull, L ) );
			if( L < 0.018 )
				++linearColumns;
			w.take( std::fabs( at( out, W, H / 2, x ) - pred ), tolerance( Vin[ x ], g ), x );
		}
		failed += report( w.ok(), quiet, "+6 dB (x%.4f): OETF( g L ) per column, worst %.2e (%.2f of tolerance), %d columns on the linear segment", g, w.error, w.ratio, linearColumns );
		failed += report( linearColumns >= 2, quiet, "  (the linear segment is exercised: %d columns)", linearColumns );
	}
	//2. Two other exponents.
	for( float slider : { 0.0f, 1.0f } )
	{
		const StatedOetf o = statedOetf( statedGamma( slider ) );
		Knobs k;
		k.gamma = slider;
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );
		Worst w;
		for( int x = 0; x < W; ++x )
		{
			const double L    = asShader( statedDecode( kNull, Vin[ x ] ) );
			const double pred = statedEncode( o, L );
			w.take( std::fabs( at( out, W, H / 2, x ) - pred ), tolerance( Vin[ x ], 1.0, o ), x );
		}
		failed += report( w.ok(), quiet, "exponent %.2f (a %.4f, k %.3f): per column worst %.2e (%.2f of tolerance)", o.g, o.a, o.k, w.error, w.ratio );
	}
	//3. Black gamma at 1: the lift below 0.25, and nothing above.
	{
		Knobs k;
		k.blackGamma = 1.0f;
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );
		Worst w;
		double aboveWorst = 0.0, biggestLift = 0.0;
		int lastLifted = -1, predLastLifted = -1, firstAbove = W;
		for( int x = 0; x < W; ++x )
		{
			const double lift = statedBlackGamma( 1.0, Vin[ x ] );
			const double pred = Vin[ x ] + lift;
			//The lift: a divide, three subtracts/multiplies and an add, 6 roundings.
			const double tol  = tolerance( Vin[ x ] ) + 12.0 * kU;
			const double e    = std::fabs( at( out, W, H / 2, x ) - pred );
			w.take( e, tol, x );
			if( Vin[ x ] >= 0.25 )
			{
				aboveWorst = std::max( aboveWorst, std::fabs( at( out, W, H / 2, x ) - Vin[ x ] ) );
				firstAbove = std::min( firstAbove, x );
			}
			//"Lifted" is a lift the tolerance can see. The lift goes to zero
			//as ( 1 - u )^2, so at a wide raster the last column below the
			//level is predicted to lift by less than the tolerance, and the
			//measured run has to be compared with the PREDICTED run.
			if( std::fabs( at( out, W, H / 2, x ) - Vin[ x ] ) > tol )
				lastLifted = x;
			if( lift > tol )
				predLastLifted = x;
			biggestLift = std::max( biggestLift, lift );
		}
		failed += report( w.ok(), quiet, "Black Gamma 1: V + 1.5 x 0.25 x u ( 1 - u )^2 below 0.25, per column worst %.2e (%.2f of tolerance)", w.error, w.ratio );
		failed += report( lastLifted == predLastLifted && predLastLifted >= firstAbove - 3 && lastLifted < firstAbove && biggestLift > 0.03, quiet,
		                  "  lifted up to column %d (predicted %d) and not from %d (V = 0.25); the largest lift is %.4f", lastLifted, predLastLifted, firstAbove, biggestLift );
		failed += report( aboveWorst <= tolerance( 1.0 ), quiet, "  above the level the ramp is the identity, worst %.2e", aboveWorst );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --order
//---------------------------------------------------------------------------
int runOrder( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== order at %dx%d: white balance before the knee, and the knee before the clip\n", W, H );
	int failed = 0;
	const float rSlider = 1.0f, pointSlider = 0.0f, slopeSlider = 0.2f;
	const double gR    = asShader( std::pow( 10.0, statedChannelGainDb( rSlider ) / 20.0 ) );//+6 dB
	const double point = asShader( statedKneePoint( pointSlider ) );
	const double slope = asShader( statedKneeSlope( slopeSlider ) );
	const double Vin   = 0.9;
	const Picture pic  = flat( W, H, Vin );
	const double L     = asShader( statedDecode( kNull, asShader( Vin ) ) );

	Knobs k;
	k.rGain     = rSlider;
	k.kneeOn    = true;
	k.kneePoint = pointSlider;
	k.kneeSlope = slopeSlider;
	std::vector< float > out;
	if( !renderOnce( k, perturb, W, H, pic, out ) )
		return report( false, quiet, "render failed" );
	const double gotR = at( out, W, H / 2, W / 2, 0 ), gotG = at( out, W, H / 2, W / 2, 1 );

	//WB then knee (the plugin's order) against knee then WB.
	const double wbFirst   = statedKnee( gR * L, point, slope );
	const double kneeFirst = gR * statedKnee( L, point, slope );
	const double predWb    = statedEncode( kNull, wbFirst ), predKnee = statedEncode( kNull, kneeFirst );
	const double tol       = tolerance( Vin, gR ) + encodeSlope( wbFirst ) * 6.0 * kU;
	const double predG     = statedEncode( kNull, statedKnee( L, point, slope ) );
	failed += report( std::fabs( gotR - predWb ) <= tol, quiet, "R at +6 dB through the knee: %.6f against WB-then-knee %.6f (+-%.1e)", gotR, predWb, tol );
	failed += report( std::fabs( predWb - predKnee ) > 20.0 * tol && std::fabs( gotR - predKnee ) > 10.0 * tol, quiet,
	                  "  knee-then-WB would give %.6f, %.0f tolerances away; the plugin is not that", predKnee, std::fabs( predWb - predKnee ) / tol );
	failed += report( std::fabs( gotG - predG ) <= tolerance( Vin ) + encodeSlope( L ) * 6.0 * kU, quiet, "  G through the knee alone: %.6f against %.6f", gotG, predG );
	failed += report( gR * L > point && L > point, quiet, "  (both channels really are above the point: %.3f and %.3f against %.2f)", gR * L, L, point );

	//Knee off, clip at 1.0: R clips first. min() returns its operand.
	{
		Knobs off;
		off.rGain = rSlider;
		std::vector< float > o;
		if( !renderOnce( off, perturb, W, H, pic, o ) )
			return report( false, quiet, "render failed" );
		const double r = at( o, W, H / 2, W / 2, 0 ), g = at( o, W, H / 2, W / 2, 1 ), b = at( o, W, H / 2, W / 2, 2 );
		failed += report( std::fabs( r - 1.0 ) <= kU && g < 1.0 - 0.05 && std::fabs( g - Vin ) <= tolerance( Vin ) && std::fabs( b - Vin ) <= tolerance( Vin ), quiet,
		                  "  Knee On off: R clips to exactly %.7f while G and B stay at %.4f, %.4f (the OETF of %.3f is %.3f, above the clip)", r, g, b, gR * L,
		                  statedEncode( kNull, gR * L ) );
	}
	//And with the knee on, the clip is never reached.
	failed += report( predWb < 1.0 && gotR < 1.0 - 10.0 * tol, quiet, "  with the knee on nothing clips: R is %.4f", gotR );
	return failed;
}

//---------------------------------------------------------------------------
// --skin
//---------------------------------------------------------------------------
int runSkin( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== skin at %dx%d: detail gain 1 - Skin Detail inside the hue window, 1 outside, 1 on a neutral\n", W, H );
	int failed = 0;
	const float levelSlider = 1.0f / 3.0f, suppressSlider = 0.75f;
	const double level = asShader( statedDetailLevel( levelSlider ) );
	const int i = 2, x0 = W / 2;
	const double tolView = 48.0 * kU;//the coloured luma, three lerps, the window products

	struct Patch
	{
		const char* name;
		double r, g, b;//the bright side, linear
		double hueOffset;//degrees added to the control
		double gain;//predicted
	};
	const Patch patches[] = {
		{ "a skin tone inside the window", 0.60, 0.34, 0.24, 0.0, 1.0 - 0.75 },
		{ "the same tone with the window 90 degrees away", 0.60, 0.34, 0.24, 90.0, 1.0 },
		{ "a neutral, whatever the window", 0.50, 0.50, 0.50, 0.0, 1.0 },
	};
	for( const Patch& p : patches )
	{
		//Both sides the same hue: the dark side is half the linear colour.
		const double hi[ 3 ] = { p.r, p.g, p.b }, lo[ 3 ] = { 0.5 * p.r, 0.5 * p.g, 0.5 * p.b };
		double Vhi[ 3 ], Vlo[ 3 ], Lhi[ 3 ], Llo[ 3 ];
		for( int c = 0; c < 3; ++c )
		{
			Vhi[ c ] = asShader( statedEncode( kNull, hi[ c ] ) );
			Vlo[ c ] = asShader( statedEncode( kNull, lo[ c ] ) );
			Lhi[ c ] = asShader( statedDecode( kNull, Vhi[ c ] ) );
			Llo[ c ] = asShader( statedDecode( kNull, Vlo[ c ] ) );
		}
		Picture pic = flat( W, H, Vlo[ 0 ], Vlo[ 1 ], Vlo[ 2 ] );
		paint( pic, W, H, x0, 0, W, H, Vhi[ 0 ], Vhi[ 1 ], Vhi[ 2 ] );
		std::vector< double > Y( W );
		for( int x = 0; x < W; ++x )
			Y[ x ] = x < x0 ? statedLuma( Llo[ 0 ], Llo[ 1 ], Llo[ 2 ] ) : statedLuma( Lhi[ 0 ], Lhi[ 1 ], Lhi[ 2 ] );
		const double hue    = model::HueDegrees( Lhi[ 0 ], Lhi[ 1 ], Lhi[ 2 ] );
		const double chroma = model::Chroma( Lhi[ 0 ], Lhi[ 1 ], Lhi[ 2 ] );

		Knobs k;
		k.detailLevel = levelSlider;
		k.detailFreq  = 0.125f;
		k.hvRatio     = 1.0f;
		k.skinDetail  = suppressSlider;
		k.skinHue     = static_cast< float >( std::fmod( hue + p.hueOffset, 360.0 ) / 360.0 );
		k.skinWidth   = 0.2727f;//+-20 degrees
		k.showDetail  = true;
		std::vector< float > view;
		if( !renderOnce( k, perturb, W, H, pic, view ) )
			return report( false, quiet, "render failed" );

		Worst w;
		for( int x = x0 - 2 * i - 3; x < x0 + 2 * i + 3; ++x )
			w.take( std::fabs( ( at( view, W, H / 2, x ) - 0.5 ) - level * statedDetailAt( Y, x, i, 0.0 ) * p.gain ), tolView, x );
		const double peak = at( view, W, H / 2, x0 ) - 0.5, predPeak = level * 0.25 * ( Y[ x0 ] - Y[ 0 ] ) * p.gain;
		failed += report( w.ok(), quiet, "%s (hue %.1f, chroma %.2f, window at %.1f +-20): gain %.2f, per pixel worst %.2e (%.2f of tolerance)", p.name, hue, chroma,
		                  std::fmod( hue + p.hueOffset, 360.0 ), p.gain, w.error, w.ratio );
		failed += report( std::fabs( peak - predPeak ) <= tolView, quiet, "  overshoot %.6f against %.6f", peak, predPeak );
		if( p.gain < 1.0 )
			failed += report( chroma >= 0.15 + 0.05, quiet, "  (the check's own patch is well past the chroma gate: %.2f against 0.15)", chroma );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --cpu: the OpenFX build's copy of the two passes against the GPU.
//
// The OpenFX plugin runs `chain::Resolve` -- the same uniforms, rounded to
// float the same way -- and then CpuChain.cpp's statement-for-statement copy
// of kLinear and kProcess. This renders one picture through the real plugin
// class on the GPU and through that copy on the CPU and compares them per
// channel, in float.
//
// The tolerance is NOT derived, unlike every other one in this file: the two
// sides evaluate the same expressions in the same precision, but pow, atan
// and division are a driver's on one side and libm's on the other, and the
// driver is free to fuse and reassociate. 1e-5 is about six times the worst
// seen across every case below on an M4 Max (1.6e-6, recorded in AGENTS.md),
// and 1/400 of an 8-bit step. What it must catch -- a stage out of order, a
// constant mistyped, a uniform read from the wrong place -- moves pixels by
// 1e-3 and up, which the control cases at the end show the comparison seeing.
//---------------------------------------------------------------------------
const double kCpuTolerance = 1e-5;

/// The controls as the plugin holds them, read back out of it: the CPU side
/// is handed exactly the slider values the GPU side was.
ccu::chain::HostValues hostValuesOf( Ccu& p )
{
	ccu::chain::HostValues h;
	h.masterGain  = p.GetFloatParameter( Ccu::PT_MASTER_GAIN );
	h.masterBlack = p.GetFloatParameter( Ccu::PT_MASTER_BLACK );
	h.whiteClip   = p.GetFloatParameter( Ccu::PT_WHITE_CLIP );
	h.rGain       = p.GetFloatParameter( Ccu::PT_R_GAIN );
	h.bGain       = p.GetFloatParameter( Ccu::PT_B_GAIN );
	h.drift       = p.GetFloatParameter( Ccu::PT_DRIFT );
	h.matrix      = p.GetFloatParameter( Ccu::PT_MATRIX );
	h.saturation  = p.GetFloatParameter( Ccu::PT_SATURATION );
	h.detailLevel = p.GetFloatParameter( Ccu::PT_DETAIL_LEVEL );
	h.detailFreq  = p.GetFloatParameter( Ccu::PT_DETAIL_FREQ );
	h.hvRatio     = p.GetFloatParameter( Ccu::PT_HV_RATIO );
	h.coring      = p.GetFloatParameter( Ccu::PT_CORING );
	h.levelDep    = p.GetFloatParameter( Ccu::PT_LEVEL_DEP );
	h.skinDetail  = p.GetFloatParameter( Ccu::PT_SKIN_DETAIL );
	h.skinHue     = p.GetFloatParameter( Ccu::PT_SKIN_HUE );
	h.skinWidth   = p.GetFloatParameter( Ccu::PT_SKIN_WIDTH );
	h.kneeOn      = p.GetFloatParameter( Ccu::PT_KNEE_ON );
	h.kneePoint   = p.GetFloatParameter( Ccu::PT_KNEE_POINT );
	h.kneeSlope   = p.GetFloatParameter( Ccu::PT_KNEE_SLOPE );
	h.gamma       = p.GetFloatParameter( Ccu::PT_GAMMA );
	h.blackGamma  = p.GetFloatParameter( Ccu::PT_BLACK_GAMMA );
	h.mix         = p.GetFloatParameter( Ccu::PT_MIX );
	h.showDetail  = p.GetFloatParameter( Ccu::PT_SHOW_DETAIL );
	return h;
}

/// The whole CPU chain over a picture, single-threaded.
std::vector< float > cpuRender( const ccu::chain::Uniforms& u, const Picture& pic, int W, int H )
{
	std::vector< float > linear( pic.size() ), out( pic.size() );
	ccu::cpu::LinearRows( u, pic.data(), W, 0, H, linear.data() );
	for( int y = 0; y < H; ++y )
		ccu::cpu::ProcessRow( u, linear.data(), pic.data(), W, H, y, 0, W, out.data() + static_cast< size_t >( y ) * W * 4 );
	return out;
}

std::vector< unsigned char > buildCard( int width, int height, int64_t frame );

/// The test card as floats, optionally pushed past white.
Picture cardPicture( int W, int H, double scale )
{
	const std::vector< unsigned char > card = buildCard( W, H, 17 );
	Picture p( card.size() );
	for( size_t i = 0; i < card.size(); ++i )
		p[ i ] = static_cast< float >( card[ i ] / 255.0 * ( i % 4 == 3 ? 1.0 : scale ) );
	return p;
}

struct CpuCompare
{
	double worst    = 0.0;
	double alpha    = 0.0;
	size_t beyond   = 0;///< channels further apart than the tolerance
	size_t eightBit = 0;///< channels whose 8-bit quantisation differs
};

CpuCompare compareCpu( const std::vector< float >& gpu, const std::vector< float >& cpu )
{
	CpuCompare c;
	for( size_t i = 0; i < gpu.size(); ++i )
	{
		const double e = std::fabs( static_cast< double >( gpu[ i ] ) - cpu[ i ] );
		if( i % 4 == 3 )
		{
			c.alpha = std::max( c.alpha, e );
			continue;
		}
		c.worst = std::max( c.worst, e );
		if( e > kCpuTolerance )
			++c.beyond;
		const auto q = []( float v ) { return std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ); };
		if( q( gpu[ i ] ) != q( cpu[ i ] ) )
			++c.eightBit;
	}
	return c;
}

/// One picture through both, `frames` GPU frames in (the drift moves) and
/// the CPU at the walk's value for the last of them.
bool renderBoth( const std::vector< std::string >& settings, const std::vector< std::string >& cpuSettings, int perturbGpu, int perturbCpu, int W, int H,
                 const Picture& pic, int frames, std::vector< float >& gpu, std::vector< float >& cpu, double* walkGpu = nullptr, double* walkCpu = nullptr )
{
	Session s;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( s.plugin, setting, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return false;
		}
	}
	s.plugin.SetPerturbForTest( perturbGpu );
	if( !s.begin( W, H ) )
		return false;
	for( int f = 0; f < frames; ++f )
		if( !s.render( pic ) )
			return false;
	gpu = s.readBackFloat();

	//The CPU side's sliders: the same plugin's, then any overrides (the
	//control case moves one).
	Ccu other;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( other, setting, error );
	}
	for( const std::string& setting : cpuSettings )
	{
		std::string error;
		applySetting( other, setting, error );
	}
	const double walk = model::DriftWalkAt( frames - 1, 1.0 / s.fps );
	if( walkGpu )
		*walkGpu = s.plugin.DriftStateForTest();
	if( walkCpu )
		*walkCpu = walk;
	s.end();
	cpu = cpuRender( ccu::chain::Resolve( hostValuesOf( other ), walk, perturbCpu ), pic, W, H );
	return true;
}

int runCpu( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== cpu at %dx%d: the OpenFX build's copy of the two passes against the GPU (tolerance %.0e)\n", W, H, kCpuTolerance );
	int failed = 0;

	struct Case
	{
		const char* what;
		std::vector< std::string > settings;
		double scale;
		int frames;
	};
	const std::vector< Case > cases = {
		{ "the defaults (a camera set up in a hurry)", {}, 1.0, 1 },
		{ "the null chain", { "Drift=0", "Matrix=0", "Detail Level=0", "Coring=0", "Level Dependence=0", "Skin Detail=0", "Knee On=0", "Black Gamma=0" }, 1.0, 1 },
		{ "everything moved: fractional delay, film matrix, low knee, high gamma, lifted blacks, gain, pedestal, clip, mix",
		  { "Master Gain=0.6", "Master Black=0.6", "White Clip=0.2", "R Gain=0.7", "B Gain=0.3", "Matrix=3", "Saturation=0.8", "Detail Level=0.8",
		    "Crispening Freq=0.33", "H/V Ratio=0.7", "Coring=0.1", "Level Dependence=0.9", "Knee Point=0.2", "Knee Slope=0.1", "Gamma=0.8",
		    "Black Gamma=0.9", "Mix=0.7" },
		  1.0, 1 },
		{ "the skin window moved and widened, the detail view, vertical-heavy",
		  { "Skin Detail=1", "Skin Hue=0.5", "Skin Width=1", "Show Detail=1", "H/V Ratio=0.2", "Matrix=2", "Crispening Freq=1" }, 1.0, 1 },
		{ "the knee off at +18 dB, Standard matrix, low gamma: hard clips", { "Knee On=0", "Master Gain=1", "Gamma=0.1", "Saturation=1" }, 1.0, 1 },
		{ "a float picture 1.4x past white", { "Detail Level=0.6", "Black Gamma=0.5" }, 1.4, 1 },
		{ "a drifting frame: Drift 1, frame 300 of a 60 fps run", { "Drift=1" }, 1.0, 301 },
	};

	CpuCompare all;
	for( const Case& c : cases )
	{
		const Picture pic = cardPicture( W, H, c.scale );
		std::vector< float > gpu, cpu;
		double walkGpu = 0.0, walkCpu = 0.0;
		if( !renderBoth( c.settings, {}, perturb, perturb, W, H, pic, c.frames, gpu, cpu, &walkGpu, &walkCpu ) )
			return report( false, quiet, "render failed: %s", c.what );
		const CpuCompare r = compareCpu( gpu, cpu );
		all.worst          = std::max( all.worst, r.worst );
		all.eightBit += r.eightBit;
		failed += report( r.beyond == 0 && r.alpha == 0.0, quiet, "%s: worst %.2e (%.4f of an 8-bit step), %zu channels past tolerance, %zu of %zu differ at 8 bits; alpha bitwise",
		                  c.what, r.worst, r.worst * 255.0, r.beyond, r.eightBit, gpu.size() / 4 * 3 );
		if( c.frames > 1 )
			failed += report( std::fabs( walkGpu - walkCpu ) < 1e-12 && walkCpu != 0.0, quiet,
			                  "  the walk at frame %d: stepped frame by frame %.15f, replayed from the frame number %.15f (%.1e apart)", c.frames - 1, walkGpu, walkCpu,
			                  std::fabs( walkGpu - walkCpu ) );
	}

	//Every perturbation is in the copy too, so a perturbed chain agrees with
	//its perturbed copy -- the copy is the shaders, negative controls and all.
	if( perturb == 0 )
	{
		const int bits[] = { model::kPerturbGammaSpace, model::kPerturbKneeFirst, model::kPerturbNoCoring, model::kPerturbDoubleDetail,
		                     model::kPerturbKneeSlope, model::kPerturbSkinIgnored, model::kPerturbGammaExponent };
		double worst = 0.0;
		size_t beyond = 0;
		for( int bit : bits )
		{
			const Picture pic = cardPicture( W, H, 1.0 );
			std::vector< float > gpu, cpu;
			if( !renderBoth( { "Master Gain=0.5", "Detail Level=0.6" }, {}, bit, bit, W, H, pic, 1, gpu, cpu ) )
				return report( false, quiet, "render failed: perturbation %d", bit );
			const CpuCompare r = compareCpu( gpu, cpu );
			worst              = std::max( worst, r.worst );
			beyond += r.beyond;
		}
		failed += report( beyond == 0, quiet, "each of the 7 perturbed chains agrees with its perturbed copy: worst %.2e", worst );

		//The controls: the comparison must be able to fail. One slider a
		//hundredth apart, and the GPU perturbed while the copy is not.
		const Picture pic = cardPicture( W, H, 1.0 );
		std::vector< float > gpu, cpu;
		renderBoth( {}, { "Detail Level=0.31" }, 0, 0, W, H, pic, 1, gpu, cpu );
		const CpuCompare slider = compareCpu( gpu, cpu );
		failed += report( slider.beyond > 0, false, "control: Detail Level 0.30 on the GPU against 0.31 on the CPU DIFFERS: worst %.2e, %zu channels past tolerance",
		                  slider.worst, slider.beyond );
		renderBoth( {}, {}, model::kPerturbKneeSlope, 0, W, H, pic, 1, gpu, cpu );
		const CpuCompare steeper = compareCpu( gpu, cpu );
		failed += report( steeper.beyond > 0, false, "control: a knee 10%% steeper on the GPU only DIFFERS: worst %.2e, %zu channels past tolerance", steeper.worst,
		                  steeper.beyond );
		renderBoth( { "Drift=1" }, {}, 0, 0, W, H, pic, 301, gpu, cpu );
		std::vector< float > gpu0, cpu0;
		renderBoth( { "Drift=1" }, {}, 0, 0, W, H, pic, 1, gpu0, cpu0 );
		const CpuCompare drifted = compareCpu( gpu, cpu0 );
		failed += report( drifted.beyond > 0, false, "control: frame 300 of a drifting run against frame 0 DIFFERS: worst %.2e", drifted.worst );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --bench-cpu: what the OpenFX build's render costs. No GL.
//---------------------------------------------------------------------------
double cpuFrameMs( const ccu::chain::Uniforms& u, const Picture& pic, int W, int H, unsigned threads, int frames )
{
	std::vector< float > linear( pic.size() ), out( pic.size() );
	auto rows = [ & ]( auto&& body ) {
		std::vector< std::thread > pool;
		const int chunk = ( H + static_cast< int >( threads ) - 1 ) / static_cast< int >( threads );
		for( int y0 = 0; y0 < H; y0 += chunk )
			pool.emplace_back( [ =, &body ] { body( y0, std::min( H, y0 + chunk ) ); } );
		for( std::thread& t : pool )
			t.join();
	};
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int f = 0; f < frames; ++f )
		{
			rows( [ & ]( int y0, int y1 ) { ccu::cpu::LinearRows( u, pic.data(), W, y0, y1, linear.data() ); } );
			rows( [ & ]( int y0, int y1 ) {
				for( int y = y0; y < y1; ++y )
					ccu::cpu::ProcessRow( u, linear.data(), pic.data(), W, H, y, 0, W, out.data() + static_cast< size_t >( y ) * W * 4 );
			} );
		}
		best = std::min( best, std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / frames );
	}
	return best;
}

int runBenchCpu( int frames )
{
	const unsigned hw = std::max( 1u, std::thread::hardware_concurrency() );
	std::printf( "The OpenFX build's CPU chain (chain::Resolve + CpuChain.cpp), default controls, best of three runs of %d frames.\n"
	             "Both passes; not the host's pixel conversion or its thread pool.\n\n",
	             frames );
	std::printf( "resolution     threads   ms/frame\n" );
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720  ", 1280, 720 }, { "1920x1080 ", 1920, 1080 }, { "3840x2160 ", 3840, 2160 } };
	const ccu::chain::Uniforms u = ccu::chain::Resolve( ccu::chain::HostValues(), 0.0 );
	for( const Size& size : sizes )
	{
		const Picture pic = cardPicture( size.width, size.height, 1.0 );
		for( unsigned threads : { 1u, 8u, hw } )
		{
			if( threads > hw )
				continue;
			std::printf( "%s    %3u      %8.2f\n", size.name, threads, cpuFrameMs( u, pic, size.width, size.height, threads, frames ) );
			if( threads == hw )
				break;
		}
	}

	//The drift's replay, which the OpenFX build pays once a frame.
	std::printf( "\nthe drift replayed to a frame (model::DriftWalkAt), once per rendered frame:\n" );
	for( double fps : { 24.0, 60.0 } )
	{
		const int64_t frame = static_cast< int64_t >( 3600.0 * fps );//an hour in: the window is full
		//Written to a volatile so neither loop can be optimised away.
		static volatile double sink = 0.0;
		double best = 1e9;
		for( int run = 0; run < 5; ++run )
		{
			const auto start = std::chrono::steady_clock::now();
			sink = model::DriftWalkAt( frame, 1.0 / fps );
			best = std::min( best, std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
		}
		const auto start = std::chrono::steady_clock::now();
		double u0        = 0.0;
		for( int64_t k = 1; k <= frame; ++k )
			u0 = model::DriftStep( u0, 1.0 / fps, static_cast< uint32_t >( k ) );
		sink              = u0;
		const double full = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
		std::printf( "  %2.0f fps, an hour in: %.2f ms (a replay from frame 0 would be %.1f ms)\n", fps, best, full );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --negative: every check can fail.
//---------------------------------------------------------------------------
int runNegative( int W, int H )
{
	std::printf( "== negative controls at %dx%d: each perturbation makes its check FAIL\n", W, H );
	struct Control
	{
		const char* what;
		int perturb;
		int ( *check )( int, int, int, bool );
		const char* checkName;
	};
	const Control controls[] = {
		{ "the chain run in gamma space (a 2.2 power for the linearise)", model::kPerturbGammaSpace, runIdentity, "--identity" },
		{ "the chain run in gamma space", model::kPerturbGammaSpace, runOrder, "--order" },
		{ "the knee before white balance", model::kPerturbKneeFirst, runOrder, "--order" },
		{ "coring's dead zone removed", model::kPerturbNoCoring, runCoring, "--coring" },
		{ "the detail kernel doubled", model::kPerturbDoubleDetail, runDetail, "--detail" },
		{ "the knee slope 10% steeper", model::kPerturbKneeSlope, runKnee, "--knee" },
		{ "the skin window ignored", model::kPerturbSkinIgnored, runSkin, "--skin" },
		{ "the OETF exponent 0.05 high", model::kPerturbGammaExponent, runGamma, "--gamma" },
	};
	int failed = 0;
	for( const Control& c : controls )
	{
		const int before      = g_checks;
		const int failsBefore = g_failures;
		const int caught      = c.check( W, H, c.perturb, true );
		//The quiet run's own bookkeeping is undone: only the verdict counts.
		g_checks   = before;
		g_failures = failsBefore;
		failed += report( caught > 0, false, "%s: %s fails (%d of its assertions)", c.what, c.checkName, caught );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --laws: every control law against its statement; the model's promises.
// No GL.
//---------------------------------------------------------------------------
int runLaws()
{
	std::printf( "== laws: every control law against its statement (no GL)\n" );
	namespace ctl = ccu::controls;
	int failed    = 0;
	double worst  = 0.0;
	auto rel      = [ & ]( double a, double b ) { return std::fabs( a - b ) / std::max( 1e-12, std::fabs( b ) ); };
	for( int i = 0; i <= 20; ++i )
	{
		const float v = static_cast< float >( i ) / 20.0f;
		worst         = std::max( worst, rel( ctl::MasterGainDb( v ), statedMasterGainDb( v ) ) );
		worst         = std::max( worst, rel( ctl::MasterGain( v ), std::pow( 10.0, statedMasterGainDb( v ) / 20.0 ) ) );
		worst         = std::max( worst, rel( ctl::MasterBlack( v ), statedMasterBlack( v ) ) );
		worst         = std::max( worst, rel( ctl::WhiteClip( v ), statedWhiteClip( v ) ) );
		worst         = std::max( worst, rel( ctl::ChannelGainDb( v ), statedChannelGainDb( v ) ) );
		worst         = std::max( worst, rel( ctl::ChannelGain( v ), std::pow( 10.0, statedChannelGainDb( v ) / 20.0 ) ) );
		worst         = std::max( worst, rel( ctl::DriftMireds( v ), statedDriftMireds( v ) ) );
		worst         = std::max( worst, rel( ctl::Saturation( v ), statedSaturation( v ) ) );
		worst         = std::max( worst, rel( ctl::DetailLevel( v ), statedDetailLevel( v ) ) );
		worst         = std::max( worst, rel( ctl::DetailSpacing( v ), statedSpacing( v ) ) );
		worst         = std::max( worst, rel( ctl::DetailHorizontalWeight( v ), statedHWeight( v ) ) );
		worst         = std::max( worst, rel( ctl::DetailVerticalWeight( v ), statedVWeight( v ) ) );
		worst         = std::max( worst, rel( ctl::CoringEdge( v ), statedCoringEdge( v ) ) );
		worst         = std::max( worst, rel( ctl::LevelDependence( v ), unit( v ) ) );
		worst         = std::max( worst, rel( ctl::SkinDetail( v ), unit( v ) ) );
		worst         = std::max( worst, rel( ctl::SkinHueDegrees( v ), statedSkinHue( v ) ) );
		worst         = std::max( worst, rel( ctl::SkinWidthDegrees( v ), statedSkinWidth( v ) ) );
		worst         = std::max( worst, rel( ctl::KneePoint( v ), statedKneePoint( v ) ) );
		worst         = std::max( worst, rel( ctl::KneeSlope( v ), statedKneeSlope( v ) ) );
		worst         = std::max( worst, rel( ctl::GammaExponent( v ), statedGamma( v ) ) );
		worst         = std::max( worst, rel( ctl::BlackGamma( v ), unit( v ) ) );
		worst         = std::max( worst, rel( ctl::Mix( v ), unit( v ) ) );
	}
	failed += report( worst <= 1e-12, false, "22 laws at 21 points: worst relative difference %.1e", worst );
	failed += report( ctl::MasterGain( 0.25f ) == 1.0 && ctl::MasterBlack( 0.25f ) == 0.0 && ctl::WhiteClip( 0.5f ) == 1.0 && ctl::ChannelGain( 0.5f ) == 1.0, false,
	                  "the exposure and white nulls are exact: gain 1, pedestal 0, clip 1, R/B gain 1" );
	failed += report( ctl::Saturation( 0.5f ) == 1.0 && ctl::DetailSpacing( 0.125f ) == 2.0 && ctl::DetailHorizontalWeight( 0.5f ) == 1.0
	                      && ctl::DetailVerticalWeight( 0.5f ) == 1.0 && ctl::CoringEdge( 0.0f ) == 0.0 && ctl::DriftMireds( 0.0f ) == 0.0,
	                  false, "the matrix and detail nulls are exact: saturation 1, spacing 2 px, H and V weights 1, coring 0, drift 0" );
	failed += report( ctl::KneeSlope( 1.0f ) == 1.0 && ctl::GammaExponent( 0.5f ) == 0.45, false, "the knee slope is exactly 1 at the top and the exponent exactly 0.45 at the middle" );

	//The OETF family at the null is BT.709 to the standard's printed
	//precision, and is its own inverse.
	{
		const model::Oetf o = model::OetfFor( 0.45 );
		failed += report( std::fabs( o.a - 1.099 ) < 5e-4 && std::fabs( o.k - 4.5 ) < 0.01 && std::fabs( o.knee - 0.081 ) < 2e-4, false,
		                  "the OETF at 0.45: a %.5f (BT.709 prints 1.099), k %.4f (4.5), break at V = %.5f (0.081)", o.a, o.k, o.knee );
		double worstRt = 0.0, worstJoin = 0.0;
		for( int i = 0; i <= 1000; ++i )
		{
			const double V = i / 1000.0;
			worstRt        = std::max( worstRt, std::fabs( model::Encode( o, model::Decode( o, V ) ) - V ) );
		}
		for( double g : { 0.35, 0.45, 0.55 } )
		{
			const model::Oetf og = model::OetfFor( g );
			worstJoin            = std::max( worstJoin, std::fabs( model::Encode( og, 0.018 - 1e-12 ) - model::Encode( og, 0.018 + 1e-12 ) ) );
			const double slopeBelow = og.k, slopeAbove = og.a * og.gamma * std::pow( 0.018, og.gamma - 1.0 );
			worstJoin = std::max( worstJoin, std::fabs( slopeBelow - slopeAbove ) * 1e-6 );
		}
		failed += report( worstRt < 1e-14 && worstJoin < 1e-9, false, "encode( decode( V ) ) = V to %.1e in double; both segments meet in value and slope at 0.018 (%.1e)", worstRt, worstJoin );
	}
	//The matrices.
	{
		const model::Mat3 s1 = model::Saturation( 1.0 );
		bool exactI          = true;
		for( int i = 0; i < 3; ++i )
			for( int j = 0; j < 3; ++j )
				exactI = exactI && s1.m[ i ][ j ] == ( i == j ? 1.0 : 0.0 );
		failed += report( exactI, false, "Saturation( 1 ) is the identity EXACTLY" );
		double whiteWorst = 0.0;
		for( int p = 0; p < model::kMatrixCount; ++p )
		{
			const double white[ 3 ] = { 1, 1, 1 };
			double out[ 3 ];
			model::PresetMatrix( p ).Apply( white, out );
			for( int c = 0; c < 3; ++c )
				whiteWorst = std::max( whiteWorst, std::fabs( out[ c ] - 1.0 ) );
		}
		failed += report( whiteWorst < 1e-9, false, "every preset preserves white to %.1e", whiteWorst );
		//SMPTE-C to BT.709, against the published matrix (Poynton, Digital
		//Video and HDTV, table 26.x; four decimals).
		const double published[ 3 ][ 3 ] = { { 0.9395, 0.0502, 0.0103 }, { 0.0178, 0.9658, 0.0164 }, { -0.0016, -0.0044, 1.0060 } };
		const model::Mat3 std709        = model::SmpteCToRec709();
		double pubWorst                 = 0.0;
		for( int i = 0; i < 3; ++i )
			for( int j = 0; j < 3; ++j )
				pubWorst = std::max( pubWorst, std::fabs( std709.m[ i ][ j ] - published[ i ][ j ] ) );
		failed += report( pubWorst < 2e-3, false, "the Standard matrix (SMPTE-C -> BT.709) matches the published one to %.1e: [%.4f %.4f %.4f; %.4f %.4f %.4f; %.4f %.4f %.4f]",
		                  pubWorst, std709.m[ 0 ][ 0 ], std709.m[ 0 ][ 1 ], std709.m[ 0 ][ 2 ], std709.m[ 1 ][ 0 ], std709.m[ 1 ][ 1 ], std709.m[ 1 ][ 2 ], std709.m[ 2 ][ 0 ],
		                  std709.m[ 2 ][ 1 ], std709.m[ 2 ][ 2 ] );
		//The hue rotation is a rotation: orthonormal, determinant 1.
		const model::Mat3 r = model::HueRotate( 37.0 );
		double ortho        = 0.0;
		for( int i = 0; i < 3; ++i )
			for( int j = 0; j < 3; ++j )
			{
				double dot = 0.0;
				for( int k = 0; k < 3; ++k )
					dot += r.m[ i ][ k ] * r.m[ j ][ k ];
				ortho = std::max( ortho, std::fabs( dot - ( i == j ? 1.0 : 0.0 ) ) );
			}
		failed += report( ortho < 1e-12, false, "HueRotate is orthonormal to %.1e", ortho );
	}
	//The drift.
	{
		double u = 0.0, sumSq = 0.0;
		const int n = 20000;
		for( int f = 0; f < n; ++f )
		{
			u = model::DriftStep( u, 1.0 / 60.0, static_cast< uint32_t >( f ) );
			if( f >= 2000 )
				sumSq += u * u;
		}
		const double var = sumSq / ( n - 2000 );
		failed += report( model::DriftStep( 0.7, 0.0, 5 ) == 0.7 && std::isfinite( u ) && var > 0.6 && var < 1.5, false,
		                  "the drift walk holds at dt = 0, stays finite, and has unit variance to within the sample (%.2f over %d frames)", var, n - 2000 );
		failed += report( std::exp( model::kDriftGainPerMired * 0.0 ) == 1.0, false, "at Drift 0 the gains are exactly 1" );

		//The OpenFX build's walk: replayed from the frame number, starting at
		//most 40 time constants back. Against the walk stepped from frame 0,
		//at both rates a timeline is likely to run, through the window's edge
		//and an hour past it. Bit-identical inside the window (the same steps
		//from the same start); past it, the forgotten history is below a
		//double's rounding.
		double replayWorst = 0.0, insideWorst = 0.0;
		int64_t compared   = 0;
		for( double fps : { 24.0, 60.0 } )
		{
			const double dt     = 1.0 / fps;
			const int64_t edge  = static_cast< int64_t >( std::ceil( 40.0 * 20.0 * fps ) );
			const int64_t last  = static_cast< int64_t >( 3600.0 * fps );
			const std::set< int64_t > probes = { 1, 2, 3, 100, edge - 1, edge, edge + 1, edge + 2, 2 * edge, last / 2, last };
			double walk = 0.0;
			for( int64_t k = 1; k <= last; ++k )
			{
				walk = model::DriftStep( walk, dt, static_cast< uint32_t >( k ) );
				if( !probes.count( k ) )
					continue;
				const double e = std::fabs( model::DriftWalkAt( k, dt ) - walk );
				if( k <= edge )
					insideWorst = std::max( insideWorst, e );
				else
					replayWorst = std::max( replayWorst, e );
				++compared;
			}
		}
		//The comparison can fail: the same replay from ONE time constant back
		//is nowhere near the walk.
		double shortWorst = 0.0;
		{
			const double dt    = 1.0 / 60.0;
			const int64_t last = 216000, window = 1200;
			double walk = 0.0, shortWalk = 0.0;
			for( int64_t k = 1; k <= last; ++k )
				walk = model::DriftStep( walk, dt, static_cast< uint32_t >( k ) );
			for( int64_t k = last - window + 1; k <= last; ++k )
				shortWalk = model::DriftStep( shortWalk, dt, static_cast< uint32_t >( k ) );
			shortWorst = std::fabs( shortWalk - walk );
		}
		failed += report( insideWorst == 0.0 && replayWorst < 1e-14 && shortWorst > 1e-3, false,
		                  "the walk replayed from the frame number is the stepped walk: bit-identical inside 800 s, %.1e apart past it (%lld frames probed at 24 and 60 fps, up to an hour in); "
		                  "a replay from one time constant back is %.2f out",
		                  replayWorst, static_cast< long long >( compared ), shortWorst );
		failed += report( model::DriftWalkAt( 0, 1.0 / 60.0 ) == 0.0 && model::DriftWalkAt( -5, 1.0 / 60.0 ) == 0.0 && model::DriftWalkAt( 10, 0.0 ) == 0.0, false,
		                  "the replayed walk is 0 at frame 0, before it, and with no frame rate" );
	}
	failed += report( model::OptionIndex( 2.4f, 4 ) == 2 && model::OptionIndex( -1.0f, 4 ) == 0 && model::OptionIndex( 9.0f, 4 ) == 3, false, "options map by index" );
	return failed;
}

int runNames()
{
	std::printf( "== names: nothing the host silently truncates (no GL)\n" );
	int failed = 0;
	Ccu plugin;
	std::set< std::string > seen;
	int longest = 0;
	std::string longestName;
	bool unique = true;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( !seen.insert( p.name ).second )
		{
			unique = false;
			std::printf( "        duplicate: %s\n", p.name.c_str() );
		}
		if( static_cast< int >( p.name.size() ) > longest )
		{
			longest     = static_cast< int >( p.name.size() );
			longestName = p.name;
		}
	}
	failed += report( longest <= 16, false, "longest parameter name is %d characters (%s); the limit is 16", longest, longestName.c_str() );
	failed += report( unique, false, "every parameter name is unique (%zu of them)", seen.size() );
	failed += report( std::strlen( "SW CCU" ) <= 16, false, "the plugin name 'SW CCU' fits the 16-character field" );
	failed += report( std::strlen( "CC01" ) == 4, false, "the id 'CC01' is four characters" );
	for( int i = 0; i < model::kMatrixCount; ++i )
		failed += report( std::strlen( model::kMatrixNames[ i ] ) <= 16, false, "matrix preset '%s' fits", model::kMatrixNames[ i ] );
	return failed;
}

//---------------------------------------------------------------------------
// The test card, for --out, the sweep, the bench and a default --pipe. Five
// bands: a grey ramp; colour patches with a skin tone and a warm white; a
// face and a jacket (skin with fine hair against a textured dark blue); bars
// of falling width, low-contrast bars, and a warm sky; a dark textured
// floor with a moving white square.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t = static_cast< double >( frame ) / 60.0;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 0.12, g = 0.12, b = 0.12;
			if( fy < 0.14 )
				r = g = b = fx;
			else if( fy < 0.30 )
			{
				const double patches[ 8 ][ 3 ] = { { 0.85, 0.1, 0.1 }, { 0.1, 0.8, 0.1 }, { 0.1, 0.15, 0.85 }, { 0.1, 0.8, 0.8 },
				                                   { 0.8, 0.1, 0.8 },  { 0.85, 0.85, 0.1 }, { 0.80, 0.62, 0.53 }, { 1.0, 0.94, 0.84 } };
				const int i = std::min( 7, static_cast< int >( fx * 8 ) );
				r = patches[ i ][ 0 ], g = patches[ i ][ 1 ], b = patches[ i ][ 2 ];
			}
			else if( fy < 0.58 )
			{
				if( fx < 0.5 )
				{
					//A face: a skin disc with fine dark hair lines on top,
					//on a mid grey.
					const double dx = ( fx - 0.25 ) * width / height, dy = ( fy - 0.44 );
					const double rr = std::sqrt( dx * dx + dy * dy );
					r = g = b = 0.45;
					if( rr < 0.12 )
					{
						r = 0.80, g = 0.62, b = 0.53;
						if( dy < -0.05 && ( ( x / 2 ) % 3 ) == 0 )
							r = 0.35, g = 0.22, b = 0.15;
					}
				}
				else
				{
					//A jacket: dark blue with a fine light weave.
					r = 0.08, g = 0.10, b = 0.28;
					if( ( ( x + y ) % 4 ) == 0 )
						r = 0.20, g = 0.22, b = 0.40;
				}
			}
			else if( fy < 0.78 )
			{
				if( fx < 0.45 )
				{
					//Bars of width 1, 2, 4, 8, 16 px, black and white.
					int w = 1, xx = x;
					while( w <= 16 )
					{
						const int span = 8 * w;
						if( xx < span )
							break;
						xx -= span;
						w *= 2;
					}
					const bool on = ( ( xx / w ) & 1 ) == 0 && w <= 16;
					r = g = b = on ? 0.9 : 0.1;
				}
				else if( fx < 0.7 )
					//Low-contrast bars for coring: 0.40 against 0.45.
					r = g = b = ( ( x / 3 ) & 1 ) ? 0.40 : 0.45;
				else
				{
					//A warm sky, bright.
					const double v = 0.80 + 0.2 * ( fx - 0.7 ) / 0.3;
					r = v, g = v * 0.95, b = v * 0.82;
				}
			}
			else
			{
				//A dark floor with a fine texture, and a moving white square.
				const double n = ( ( ( x * 7 + y * 13 ) % 5 ) - 2 ) * 0.012;
				r = 0.06 + n, g = 0.06 + n, b = 0.07 + n;
				const double sx = 0.05 + 0.85 * ( 0.5 + 0.5 * std::sin( t * 1.3 ) );
				if( std::fabs( fx - sx ) < 0.03 && fy > 0.84 && fy < 0.94 )
					r = g = b = 1.0;
			}
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ]           = static_cast< unsigned char >( std::lround( 255.0 * std::clamp( r, 0.0, 1.0 ) ) );
			px[ 1 ]           = static_cast< unsigned char >( std::lround( 255.0 * std::clamp( g, 0.0, 1.0 ) ) );
			px[ 2 ]           = static_cast< unsigned char >( std::lround( 255.0 * std::clamp( b, 0.0, 1.0 ) ) );
			px[ 3 ]           = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( session.plugin, setting, error );
	}
	if( !session.begin( width, height ) )
		return -1.0;

	std::vector< std::vector< unsigned char > > loop;
	for( int i = 0; i < 4; ++i )
		loop.push_back( buildCard( width, height, i * 7 ) );

	const int warmup = 10;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( loop[ static_cast< size_t >( frame ) % loop.size() ] );
	glFinish();

	//Best of three: the GPU is shared with other builds on this machine.
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < frames; ++i )
			session.renderNow();
		glFinish();
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		best                 = std::min( best, seconds * 1000.0 / frames );
	}
	session.end();
	return best;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720  ", 1280, 720 }, { "1920x1080 ", 1920, 1080 }, { "3840x2160 ", 3840, 2160 } };
	std::printf( "%d frames each, best of three runs, after a 10-frame warm-up, glFinish both sides, default controls.\n\n", frames );
	std::printf( "resolution     ms/frame    %% of a 60fps frame\n" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( settings, size.width, size.height, frames );
		std::printf( "%s    %7.2f      %5.1f%%\n", size.name, ms, ms / 16.667 * 100.0 );
	}
	std::printf( "\nTwo passes: the linear picture into RGBA32F, then detail and the rest into the host's framebuffer.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace sh = ccu::shaders;
	const std::pair< const char*, const char* > files[] = {
		{ "vertex.vert", sh::kVertex },
		{ "linear.frag", sh::kLinear },
		{ "process.frag", sh::kProcess },
	};
	static_assert( sizeof( files ) / sizeof( files[ 0 ] ) == sh::kFragmentCount + 1, "the dump lists every shader" );
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

/// A slider ramps linearly between cues; an option, a boolean or an
/// integer STEPS: it holds each cue's value until the next cue's frame.
/// Both hold before the first cue and after the last.
float valueAt( const Track& track, int frame, bool steps )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( steps )
				return frame >= b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"cctest -- render and measure the CCU camera chain\n"
		"\n"
		"  --out PATH          render the test card through the plugin (default /tmp/ccu.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             the synthetic clock's rate, for --out and --pipe (default 60)\n"
		"  --set \"Name=V\"      set a parameter by its display name (0..1 for sliders, the element\n"
		"                      index for Matrix, 0/1 for Knee On and Show Detail). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --identity          every stage at its null returns the input, to a bound derived from the OETF round trip; alpha bitwise; and after a resize\n"
		"  --detail            a step's overshoot is Detail Level x h / 4 and lasts the spacing; whole and fractional; H and V\n"
		"  --coring            an edge below Coring gets no detail; one above gets the overshoot less the dead zone\n"
		"  --knee              above the point a ramp's slope is Knee Slope, continuous at the point, per channel\n"
		"  --gamma             a ramp follows the OETF at three exponents; Black Gamma lifts only below its level\n"
		"  --order             WB then knee compresses a warm white in R; knee-first predicts otherwise; with the knee off R clips first\n"
		"  --skin              detail gain 1 - Skin Detail inside the hue window, 1 outside, 1 on a neutral\n"
		"  --negative          every check above can fail\n"
		"  --cpu               the OpenFX build's CPU copy of the two passes against the GPU, with a control that must differ\n"
		"  --perturb BITS      run the checks verbosely against a perturbed chain (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --laws              every control law against its statement; the OETF, the matrices, the drift\n"
		"  --names             nothing a host will silently truncate; the host reads SW CCU / CC01\n"
		"  --offline           both; says loudly what it skipped. For CI.\n"
		"  --allow-no-gl       with the rendering checks: SKIP loudly, not FAIL, when no GL 4.1 context exists\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --bench-cpu         time the OpenFX build's CPU chain at 720p, 1080p and 4K, and its drift replay (no GL)\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'; sliders ramp, options and booleans step\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/ccu.png";
	std::string scriptPath;
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantBenchCpu = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--identity", "--detail", "--coring", "--knee", "--gamma", "--order", "--skin", "--negative", "--cpu" };
	const std::set< std::string > offline  = { "--laws", "--names" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--bench-cpu" )
			wantBenchCpu = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
			for( const char* m : { "--laws", "--names" } )
				checks.push_back( m );
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	//No GL: the OpenFX build has none.
	if( wantBenchCpu )
		return runBenchCpu( frames == 40 ? 20 : frames );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Ccu plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL     = false;
		bool offlineRan = false;
		for( const std::string& check : checks )
		{
			if( check == "--laws" )
				runLaws();
			else if( check == "--names" )
				runNames();
			else
			{
				needGL = true;
				continue;
			}
			offlineRan = true;
			std::printf( "\n" );
		}
		if( offlineRan && !needGL )
			std::printf( "   OFFLINE: --identity, --detail, --coring, --knee, --gamma, --order, --skin, --cpu and their\n"
			             "   negative controls were NOT run. Nothing here drew a pixel through a GL driver;\n"
			             "   the shaders were not exercised, only (in CI) compiled by glslc. The laws have\n"
			             "   no negative control of their own.\n\n" );

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--identity" )
						runIdentity( width, height, perturb );
					else if( check == "--detail" )
						runDetail( width, height, perturb );
					else if( check == "--coring" )
						runCoring( width, height, perturb );
					else if( check == "--knee" )
						runKnee( width, height, perturb );
					else if( check == "--gamma" )
						runGamma( width, height, perturb );
					else if( check == "--order" )
						runOrder( width, height, perturb );
					else if( check == "--skin" )
						runSkin( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else if( check == "--cpu" )
						runCpu( width, height, perturb );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, frames < 40 ? 60 : frames ) );

	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		std::map< unsigned int, std::pair< Track, bool > > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = { entry.second, stepsBetweenCues( session.plugin, static_cast< unsigned int >( index ) ) };
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider would.
			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second.first, index, track.second.second ) );

			const bool rendered = index != failRender && session.render( frame );
			if( !rendered )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( buildCard( width, height, frame ) ) )
			return finish( 1 );

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
