#pragma once

/**
	The camera's processing chain, described, with every constant in it.

	--------------------------------------------------------------- the idea

	The "video" look of a studio or OB camera is not a filter. It is a
	PROCESSING CHAIN IN A FIXED ORDER, each stage a known circuit with the knob
	a shader on the CCU panel turned:

	    clip (BT.709 video)
	      -> linearise            the inverse OETF: the clip stands in for
	                              scene light (an assumption, stated)
	      -> master gain          head-end gain, in dB, in linear light
	      -> white balance        R and B gains, plus a slow drift in mireds
	      -> matrix               a 3x3 in linear light, plus saturation
	      -> detail               aperture correction: a high-passed copy of
	                              the luma from pixel and line delays, cored,
	                              level-dependent, suppressed in the skin
	                              window, added to all three channels
	      -> knee                 above the knee point, a gentler slope,
	                              continuous at the point, per channel
	      -> gamma                the BT.709 OETF with a trimmable exponent
	      -> black gamma          a lift below a stated video level
	      -> pedestal             master black, added on video
	      -> white clip           min( video, clip )
	      -> mix                  against the source

	Every stage is a pure function of the pixel except detail, which reads
	the linear luma at +-spacing horizontally and vertically. So the plugin
	is TWO passes: one that builds the linear picture (through the matrix)
	into a picture-sized RGBA32F buffer with the luma in its alpha, and one
	that reads it back, adds detail and runs the rest of the chain straight
	into the host's framebuffer.

	Nothing in the chain carries across frames except the white-balance
	drift, which is a double on the CPU. (The OpenFX build, which renders
	frames out of order, replays it from the frame number instead:
	DriftWalkAt, below.)

	------------------------------------------------ what this header holds

	The constants, the perturbation bits for the negative controls, the
	option tables, and the closed forms the plugin computes on the CPU in
	double (the OETF's constants for a given exponent, the matrices, the
	drift's step). The harness restates each law from the comments here and
	holds `Controls.cpp` and the shaders to them; it never reads a number
	out of the plugin to predict the plugin.
*/

#include <cmath>
#include <cstdint>

namespace ccu::model
{

//---------------------------------------------------------------------------
// Negative controls. Each perturbs the PLUGIN's shaders or its uniforms,
// never the harness's expectation, so that `cctest --negative` proves each
// check can fail. The shipped plugin carries this at zero and nothing in a
// host can set it.
//---------------------------------------------------------------------------
enum Perturb : int
{
	kPerturbNone          = 0,
	kPerturbGammaSpace    = 1 << 0,///< linearise with a plain 2.2 power, not the inverse OETF: the chain runs in the wrong space
	kPerturbKneeFirst     = 1 << 1,///< the knee applied BEFORE white balance and the matrix
	kPerturbNoCoring      = 1 << 2,///< coring's dead zone removed
	kPerturbDoubleDetail  = 1 << 3,///< the detail kernel doubled: ( -1/2, 1, -1/2 )
	kPerturbKneeSlope     = 1 << 4,///< the knee slope 10% steeper than the control says
	kPerturbSkinIgnored   = 1 << 5,///< the skin window ignored: full detail everywhere
	kPerturbGammaExponent = 1 << 6,///< the OETF exponent 0.05 above the control
};

//---------------------------------------------------------------------------
// BT.709 luma, used for the detail signal, the saturation matrix and the
// level dependence.
//---------------------------------------------------------------------------
/// pi, spelt out: MSVC has no M_PI without _USE_MATH_DEFINES, and the first
/// Windows CI runs of two siblings failed on exactly that.
inline constexpr double kPi = 3.14159265358979323846;

inline constexpr double kLumaR = 0.2126;
inline constexpr double kLumaG = 0.7152;
inline constexpr double kLumaB = 0.0722;

//---------------------------------------------------------------------------
// The OETF.
//
// BT.709 writes V = 1.099 L^0.45 - 0.099 above L = 0.018 and V = 4.5 L
// below. Those four numbers are not independent: given the exponent g and
// the break b, the two segments meet in value AND slope only for
//
//     a( g ) = 1 / ( 1 - ( 1 - g ) b^g ),     k( g ) = a g b^( g - 1 )
//
// and at g = 0.45, b = 0.018 that is a = 1.0991, k = 4.506 -- the standard's
// constants to their printed precision. A CCU's Gamma knob sets the
// exponent, so the curve here is that family: the exponent is the control
// and a, k follow from it in double, once a frame. The linearise stage uses
// the same family at exactly 0.45, so the round trip is exact in principle
// and the identity check's bound is the float arithmetic alone.
//
// The cost is honest: at the null this OETF differs from BT.709's printed
// constants by 1e-4 in a (1.0991 against 1.099) and 6e-3 in k (4.506
// against 4.5). The harness asserts that agreement to the standard's own
// rounding; nothing in an 8-bit clip can see the difference.
//---------------------------------------------------------------------------
inline constexpr double kOetfBreak         = 0.018;
inline constexpr double kOetfExponentNull  = 0.45;

struct Oetf
{
	double gamma;///< the exponent
	double a;    ///< the scale, a L^g - ( a - 1 )
	double k;    ///< the linear segment's slope
	double knee; ///< the break in video, k * b
};

inline Oetf OetfFor( double gamma )
{
	Oetf o;
	o.gamma = gamma;
	const double bg = std::pow( kOetfBreak, gamma );
	o.a             = 1.0 / ( 1.0 - ( 1.0 - gamma ) * bg );
	o.k             = o.a * gamma * std::pow( kOetfBreak, gamma - 1.0 );
	o.knee          = o.k * kOetfBreak;
	return o;
}

/// Scene linear to video.
inline double Encode( const Oetf& o, double L )
{
	if( L < kOetfBreak )
		return o.k * L;
	return o.a * std::pow( L, o.gamma ) - ( o.a - 1.0 );
}

/// Video to scene linear.
inline double Decode( const Oetf& o, double V )
{
	if( V < o.knee )
		return V / o.k;
	return std::pow( ( V + ( o.a - 1.0 ) ) / o.a, 1.0 / o.gamma );
}

//---------------------------------------------------------------------------
// Black gamma: a lift of the video below kBlackGammaLevel, zero at that
// level and above it, with zero slope there so the curve stays smooth:
//
//     u = V / Vb,   V' = V + BlackGamma * kBlackGammaLift * Vb * u ( 1 - u )^2
//
// The bump peaks at u = 1/3 (4/27 of the scale). At BlackGamma = 0 the
// added term is 0 * ..., which is exactly zero.
//---------------------------------------------------------------------------
inline constexpr double kBlackGammaLevel = 0.25;
inline constexpr double kBlackGammaLift  = 1.5;

inline double BlackGammaLift( double blackGamma, double V )
{
	if( V >= kBlackGammaLevel )
		return 0.0;
	const double u = V / kBlackGammaLevel;
	return blackGamma * kBlackGammaLift * kBlackGammaLevel * u * ( 1.0 - u ) * ( 1.0 - u );
}

//---------------------------------------------------------------------------
// Detail.
//
// The high-pass is the symmetric ( -1/4, 1/2, -1/4 ) kernel at a spacing of
// s pixels, on the linear luma: d = Y( x ) / 2 - Y( x - s ) / 4 - Y( x + s ) / 4.
// Its response to a unit step is +1/4 on the bright side for s pixels and
// -1/4 on the dark side for s pixels -- the halo is the width of the DELAY,
// not of the edge. The same kernel over line delays is the vertical detail.
//
// A fractional spacing lands between texels, and the tap is the lerp of the
// two texels either side, done by hand from two texelFetches:
//
//     Y( x + s ) = ( 1 - f ) Y[ x + i ] + f Y[ x + i + 1 ],   s = i + f
//
// so nothing rests on a texture unit's filtering and the harness can state
// the tap exactly.
//
// Coring is a dead zone on d: d' = sign( d ) max( |d| - c / 4, 0 ), where c
// is the Coring control in units of EDGE HEIGHT (an edge lower than c gives
// no detail at all, because a step of height h peaks at h / 4).
//
// Level dependence scales the detail down where the luma is below
// kLevelDependenceRef: gain = 1 - LevelDependence * ( 1 - min( 1, Y / ref ) ).
//
// The skin window: a hue window of half-width w about the Skin Hue, flat 1
// inside 3/4 w and falling to 0 at w by a smoothstep, gated by a chroma
// smoothstep from kSkinChromaLow to kSkinChromaHigh so neutrals never count
// as skin. Inside it the detail gain is ( 1 - SkinDetail ).
//---------------------------------------------------------------------------
inline constexpr double kDetailStepPeak     = 0.25;
inline constexpr double kLevelDependenceRef = 0.2;
inline constexpr double kSkinInnerFraction  = 0.75;
inline constexpr double kSkinChromaLow      = 0.05;
inline constexpr double kSkinChromaHigh     = 0.15;

/// Hue in degrees, 0 at red, 60 at yellow, of a linear RGB colour: the
/// angle of the chroma vector in the plane normal to grey.
inline double HueDegrees( double r, double g, double b )
{
	const double h = std::atan2( std::sqrt( 3.0 ) * ( g - b ), 2.0 * r - g - b ) * 180.0 / kPi;
	return h < 0.0 ? h + 360.0 : h;
}

/// The chroma gate's argument: ( max - min ) / max.
inline double Chroma( double r, double g, double b )
{
	const double hi = std::fmax( r, std::fmax( g, b ) );
	const double lo = std::fmin( r, std::fmin( g, b ) );
	return hi > 0.0 ? ( hi - lo ) / hi : 0.0;
}

//---------------------------------------------------------------------------
// The matrices. Each preset is stated as a derivation, not typed in.
//
//   Saturation( s )   ( 1 - s ) P + s I, P the projection onto BT.709 luma
//                     (every row the luma weights). White-preserving; s = 1
//                     is the identity EXACTLY (0 * P + 1 * I).
//   HueRotate( th )   Rodrigues' rotation about the grey axis ( 1, 1, 1 ),
//                     which is therefore preserved: white stays white.
//   Primaries         RGB -> XYZ from a set of chromaticities and a white,
//                     so a primaries-to-primaries matrix is XYZ709^-1 XYZother.
//
// Presets (generic names; nothing here is a manufacturer's table):
//   Identity          I. The camera's primaries are already BT.709's.
//   Standard          SMPTE 170M ("SMPTE-C") primaries to BT.709 primaries,
//                     D65 to D65, from the chromaticities in the two
//                     standards. A camera lined up on standard-definition
//                     phosphors, viewed on an HD display: mild, and real.
//   High Saturation   Saturation( 1.35 ).
//   Film-like         Saturation( 0.82 ) . HueRotate( +4 degrees ) -- the
//                     cross-talk of a film's dye layers expressed as the two
//                     primitives. The two numbers are judged, not measured.
//
// The Saturation control composes on top: M = Saturation( 2 p ) . Preset.
//---------------------------------------------------------------------------
enum MatrixPreset
{
	kMatrixIdentity = 0,
	kMatrixStandard,
	kMatrixHighSaturation,
	kMatrixFilmLike,
	kMatrixCount
};

inline constexpr const char* kMatrixNames[ kMatrixCount ] = { "Identity", "Standard", "High Saturation", "Film-like" };

inline constexpr double kHighSaturation   = 1.35;
inline constexpr double kFilmSaturation   = 0.82;
inline constexpr double kFilmHueDegrees   = 4.0;

struct Mat3
{
	double m[ 3 ][ 3 ];///< row-major: out_i = sum_j m[ i ][ j ] in_j

	static Mat3 Identity()
	{
		Mat3 r = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
		return r;
	}
	Mat3 operator*( const Mat3& o ) const
	{
		Mat3 r = {};
		for( int i = 0; i < 3; ++i )
			for( int j = 0; j < 3; ++j )
				for( int k = 0; k < 3; ++k )
					r.m[ i ][ j ] += m[ i ][ k ] * o.m[ k ][ j ];
		return r;
	}
	void Apply( const double in[ 3 ], double out[ 3 ] ) const
	{
		for( int i = 0; i < 3; ++i )
			out[ i ] = m[ i ][ 0 ] * in[ 0 ] + m[ i ][ 1 ] * in[ 1 ] + m[ i ][ 2 ] * in[ 2 ];
	}
};

inline Mat3 Saturation( double s )
{
	const double w[ 3 ] = { kLumaR, kLumaG, kLumaB };
	Mat3 r;
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			r.m[ i ][ j ] = ( 1.0 - s ) * w[ j ] + s * ( i == j ? 1.0 : 0.0 );
	return r;
}

inline Mat3 HueRotate( double degrees )
{
	const double t = degrees * kPi / 180.0;
	const double c = std::cos( t ), s = std::sin( t );
	const double k = 1.0 / std::sqrt( 3.0 );
	//R = c I + s [k]x + ( 1 - c ) k k^T, k = ( 1, 1, 1 ) / sqrt 3.
	Mat3 r;
	const double kk = ( 1.0 - c ) * k * k;
	const double sk = s * k;
	r.m[ 0 ][ 0 ] = c + kk;
	r.m[ 0 ][ 1 ] = kk - sk;
	r.m[ 0 ][ 2 ] = kk + sk;
	r.m[ 1 ][ 0 ] = kk + sk;
	r.m[ 1 ][ 1 ] = c + kk;
	r.m[ 1 ][ 2 ] = kk - sk;
	r.m[ 2 ][ 0 ] = kk - sk;
	r.m[ 2 ][ 1 ] = kk + sk;
	r.m[ 2 ][ 2 ] = c + kk;
	return r;
}

inline Mat3 Inverse( const Mat3& a )
{
	const double( *m )[ 3 ] = a.m;
	const double det = m[ 0 ][ 0 ] * ( m[ 1 ][ 1 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 1 ] )
	                   - m[ 0 ][ 1 ] * ( m[ 1 ][ 0 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 0 ] )
	                   + m[ 0 ][ 2 ] * ( m[ 1 ][ 0 ] * m[ 2 ][ 1 ] - m[ 1 ][ 1 ] * m[ 2 ][ 0 ] );
	Mat3 r;
	r.m[ 0 ][ 0 ] = ( m[ 1 ][ 1 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 1 ] ) / det;
	r.m[ 0 ][ 1 ] = ( m[ 0 ][ 2 ] * m[ 2 ][ 1 ] - m[ 0 ][ 1 ] * m[ 2 ][ 2 ] ) / det;
	r.m[ 0 ][ 2 ] = ( m[ 0 ][ 1 ] * m[ 1 ][ 2 ] - m[ 0 ][ 2 ] * m[ 1 ][ 1 ] ) / det;
	r.m[ 1 ][ 0 ] = ( m[ 1 ][ 2 ] * m[ 2 ][ 0 ] - m[ 1 ][ 0 ] * m[ 2 ][ 2 ] ) / det;
	r.m[ 1 ][ 1 ] = ( m[ 0 ][ 0 ] * m[ 2 ][ 2 ] - m[ 0 ][ 2 ] * m[ 2 ][ 0 ] ) / det;
	r.m[ 1 ][ 2 ] = ( m[ 0 ][ 2 ] * m[ 1 ][ 0 ] - m[ 0 ][ 0 ] * m[ 1 ][ 2 ] ) / det;
	r.m[ 2 ][ 0 ] = ( m[ 1 ][ 0 ] * m[ 2 ][ 1 ] - m[ 1 ][ 1 ] * m[ 2 ][ 0 ] ) / det;
	r.m[ 2 ][ 1 ] = ( m[ 0 ][ 1 ] * m[ 2 ][ 0 ] - m[ 0 ][ 0 ] * m[ 2 ][ 1 ] ) / det;
	r.m[ 2 ][ 2 ] = ( m[ 0 ][ 0 ] * m[ 1 ][ 1 ] - m[ 0 ][ 1 ] * m[ 1 ][ 0 ] ) / det;
	return r;
}

/// RGB -> XYZ for primaries ( xr, yr ), ( xg, yg ), ( xb, yb ) and white
/// ( xw, yw ), the white mapping to Y = 1. The textbook construction.
inline Mat3 PrimariesToXYZ( double xr, double yr, double xg, double yg, double xb, double yb, double xw, double yw )
{
	Mat3 p = { { { xr, xg, xb }, { yr, yg, yb }, { 1.0 - xr - yr, 1.0 - xg - yg, 1.0 - xb - yb } } };
	const double white[ 3 ] = { xw / yw, 1.0, ( 1.0 - xw - yw ) / yw };
	double s[ 3 ];
	Inverse( p ).Apply( white, s );
	Mat3 r;
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			r.m[ i ][ j ] = p.m[ i ][ j ] * s[ j ];
	return r;
}

/// SMPTE 170M primaries -> BT.709 primaries. Both standards' white is D65.
inline Mat3 SmpteCToRec709()
{
	const Mat3 c   = PrimariesToXYZ( 0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290 );
	const Mat3 r709 = PrimariesToXYZ( 0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290 );
	return Inverse( r709 ) * c;
}

inline Mat3 PresetMatrix( int preset )
{
	switch( preset )
	{
	case kMatrixStandard: return SmpteCToRec709();
	case kMatrixHighSaturation: return Saturation( kHighSaturation );
	case kMatrixFilmLike: return Saturation( kFilmSaturation ) * HueRotate( kFilmHueDegrees );
	default: return Mat3::Identity();
	}
}

//---------------------------------------------------------------------------
// White-balance drift.
//
// A warming-up camera's colour temperature wanders. Modelled as an
// Ornstein-Uhlenbeck walk with unit stationary variance and a time constant
// kDriftTauSeconds, stepped once a frame in double from the frame's dt:
//
//     u <- a u + sqrt( 1 - a^2 ) g,   a = exp( -dt / tau ),  g ~ N( 0, 1 )
//
// g is drawn from the fleet's integer hash of ( seed, frame ) through
// Box-Muller, so the walk is the same in the harness as in a host and the
// same every time. The shift in mireds is Drift * u, and a mired shift
// moves the R and B gains in opposite directions:
//
//     gR *= exp( +kDriftGainPerMired * shift ),  gB *= exp( -... )
//
// At Drift = 0 the shift is 0 * u = 0 exactly and exp( 0 ) is exactly 1.
//---------------------------------------------------------------------------
inline constexpr double kDriftTauSeconds     = 20.0;
inline constexpr double kDriftMiredScale     = 60.0; ///< RMS mireds at Drift = 1
inline constexpr double kDriftGainPerMired   = 0.004;///< about 4% of R/B ratio per 10 mireds
inline constexpr uint32_t kDriftSeed         = 0x43433031u;///< "CC01"

inline uint32_t HashInt( uint32_t v )
{
	uint32_t state = v * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/// A standard normal for frame n: Box-Muller on two hashed uniforms, the
/// first in ( 0, 1 ] so the log is finite.
inline double DriftGaussian( uint32_t frame )
{
	const double u1 = ( ( HashInt( kDriftSeed + 2u * frame ) >> 8 ) + 1.0 ) / 16777216.0;
	const double u2 = ( HashInt( kDriftSeed + 2u * frame + 1u ) >> 8 ) / 16777216.0;
	return std::sqrt( -2.0 * std::log( u1 ) ) * std::cos( 2.0 * kPi * u2 );
}

inline double DriftStep( double u, double dtSeconds, uint32_t frame )
{
	if( dtSeconds <= 0.0 )
		return u;
	const double a = std::exp( -dtSeconds / kDriftTauSeconds );
	return a * u + std::sqrt( 1.0 - a * a ) * DriftGaussian( frame );
}

//---------------------------------------------------------------------------
// The same walk as a function of the frame number, for a host that renders
// frames out of order, alone and concurrently (the OpenFX build).
//
// The FFGL build steps the walk once a frame from u = 0: its first frame has
// no dt and leaves u at 0, and frame n >= 1 steps with dt and the Gaussian
// for n. So at frame n
//
//     u( n ) = sum over k = 1 .. n of  a^( n - k ) sqrt( 1 - a^2 ) g( k )
//
// and replaying those steps gives it back. Replaying ALL of them costs n
// steps a frame, and a timeline that starts at 01:00:00:00 begins at frame
// 86 400 at 24 fps -- 216 000 at 60. But step k weighs a^( n - k ) =
// exp( -( n - k ) dt / tau ), and kDriftReplayTaus time constants back that
// is exp( -40 ) = 4.2e-18: below half an ulp of any u this hash can produce
// (|g| <= sqrt( 2 ln 2^24 ) = 5.8, and a realised |u| beyond 6 does not
// happen). So the replay starts at most 40 tau = 800 s back, from u = 0,
// and agrees with the full walk to the rounding of a double -- `cctest
// --laws` measures that, and the cost. At 60 fps that is 48 000 steps.
//
// The steps are DriftStep's own, with the frame's dt; nothing about the walk
// is restated here. Frame 0 and before is u = 0, as the FFGL build's first
// frame is.
//---------------------------------------------------------------------------
inline constexpr double kDriftReplayTaus = 40.0;

inline double DriftWalkAt( int64_t frame, double dtSeconds )
{
	if( frame <= 0 || !( dtSeconds > 0.0 ) )
		return 0.0;
	const double windowFrames = std::ceil( kDriftReplayTaus * kDriftTauSeconds / dtSeconds );
	int64_t first             = 1;
	if( static_cast< double >( frame ) > windowFrames )
		first = frame - static_cast< int64_t >( windowFrames ) + 1;
	double u = 0.0;
	for( int64_t k = first; k <= frame; ++k )
		u = DriftStep( u, dtSeconds, static_cast< uint32_t >( k ) );
	return u;
}

//---------------------------------------------------------------------------
// Options are mapped by index: an option parameter's range reads back 0..1
// whatever its element count, so the value is rounded and clamped here.
//---------------------------------------------------------------------------
inline int OptionIndex( float value, int count )
{
	const int i = static_cast< int >( std::lround( value ) );
	return i < 0 ? 0 : ( i >= count ? count - 1 : i );
}

} // namespace ccu::model
