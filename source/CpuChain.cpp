#include "CpuChain.h"

#include <algorithm>
#include <cmath>

/*
	Everything in here is float on purpose: it is the GPU's precision, and
	the point of this file is to agree with the GPU, not to be more right
	than it. GLSL's unsuffixed literals are floats, so every literal here
	carries an f. The GLSL built-ins it needs are restated below with the
	definitions the GLSL 4.10 specification gives them (8.3, 8.1).
*/

namespace ccu::cpu
{
namespace
{
const float kLumaR = 0.2126f;
const float kLumaG = 0.7152f;
const float kLumaB = 0.0722f;

/// GLSL clamp( x, lo, hi ) = min( max( x, lo ), hi ).
inline float clampf( float x, float lo, float hi )
{
	return std::min( std::max( x, lo ), hi );
}

/// GLSL smoothstep: t = clamp( ( x - e0 ) / ( e1 - e0 ), 0, 1 ), t^2 ( 3 - 2t ).
inline float smoothstepf( float e0, float e1, float x )
{
	const float t = clampf( ( x - e0 ) / ( e1 - e0 ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}

/// GLSL mod: x - y floor( x / y ).
inline float modf_glsl( float x, float y )
{
	return x - y * std::floor( x / y );
}

/// GLSL sign: -1, 0 or 1.
inline float signf( float x )
{
	return x > 0.0f ? 1.0f : ( x < 0.0f ? -1.0f : 0.0f );
}

//= mirrored from kLinear's linearise(). Edit both.
inline float linearise( const chain::Uniforms& u, float v )
{
	//Perturb 1: a plain 2.2 power in place of the inverse OETF (a negative control).
	if( ( u.Perturb & 1 ) != 0 )
		return std::pow( v, 2.2f );
	return v < u.InvKnee ? v / u.InvK : std::pow( ( v + u.InvC ) / u.InvA, u.InvGamma );
}

//= mirrored from knee() in both shaders. Edit all three.
inline float knee( const chain::Uniforms& u, float c )
{
	return c > u.KneePoint ? u.KneePoint + ( c - u.KneePoint ) * u.KneeSlope : c;
}

//= mirrored from kProcess's encode(). Edit both.
inline float encode( const chain::Uniforms& u, float l )
{
	l = std::max( l, 0.0f );
	return l < u.OetfBreak ? u.OetfK * l : u.OetfA * std::pow( l, u.OetfGamma ) - u.OetfC;
}

//= mirrored from kProcess's blackGamma(). Edit both.
inline float blackGamma( const chain::Uniforms& u, float v )
{
	if( v < u.BlackLevel )
	{
		const float t = v / u.BlackLevel;
		v += u.BlackGamma * u.BlackLift * u.BlackLevel * t * ( 1.0f - t ) * ( 1.0f - t );
	}
	return v;
}

//= mirrored from kProcess's hueDegrees(). Edit both.
inline float hueDegrees( float r, float g, float b )
{
	const float h = std::atan2( 1.7320508075688772f * ( g - b ), 2.0f * r - g - b ) * 57.295779513082321f;
	return h < 0.0f ? h + 360.0f : h;
}

struct Linear
{
	const float* data;
	int width;
	int height;

	//= mirrored from kProcess's lumaAt(): clamped to the picture, the edge
	//= pixel's neighbour past the edge is the edge pixel. Edit both.
	float lumaAt( int x, int y ) const
	{
		x = std::clamp( x, 0, width - 1 );
		y = std::clamp( y, 0, height - 1 );
		return data[ ( static_cast< size_t >( y ) * width + x ) * 4 + 3 ];
	}

	//= mirrored from kProcess's highpass(): the ( -1/4, 1/2, -1/4 ) kernel at
	//= SpacingInt + SpacingFrac, each far tap a hand lerp. Edit both.
	float highpass( const chain::Uniforms& u, int x, int y, int dx, int dy, float centre ) const
	{
		const int i        = u.SpacingInt;
		const float f      = u.SpacingFrac;
		const float before = ( 1.0f - f ) * lumaAt( x - i * dx, y - i * dy ) + f * lumaAt( x - ( i + 1 ) * dx, y - ( i + 1 ) * dy );
		const float after  = ( 1.0f - f ) * lumaAt( x + i * dx, y + i * dy ) + f * lumaAt( x + ( i + 1 ) * dx, y + ( i + 1 ) * dy );
		return 0.5f * centre - 0.25f * ( before + after );
	}
};
} // namespace

//= mirrored from kLinear's main(). Edit both.
void LinearRows( const chain::Uniforms& u, const float* source, int width, int y0, int y1, float* linear )
{
	const float* M = u.Matrix;
	for( int y = y0; y < y1; ++y )
	{
		const float* src = source + static_cast< size_t >( y ) * width * 4;
		float* dst       = linear + static_cast< size_t >( y ) * width * 4;
		for( int x = 0; x < width; ++x, src += 4, dst += 4 )
		{
			const float v[ 3 ] = { std::max( src[ 0 ], 0.0f ), std::max( src[ 1 ], 0.0f ), std::max( src[ 2 ], 0.0f ) };

			//The clip stands in for scene light: linearise, then head-end gain.
			float r = linearise( u, v[ 0 ] ) * u.MasterGain;
			float g = linearise( u, v[ 1 ] ) * u.MasterGain;
			float b = linearise( u, v[ 2 ] ) * u.MasterGain;

			//Perturb 2: the knee before white balance (a negative control).
			if( ( u.Perturb & 2 ) != 0 && u.KneeOn != 0 )
			{
				r = knee( u, r );
				g = knee( u, g );
				b = knee( u, b );
			}

			//White balance: R and B gains about G.
			r *= u.GainR;
			b *= u.GainB;

			//The matrix, in linear light. Row-major: out_i = sum_j M[ i ][ j ] in_j,
			//which is what `Matrix * lin` computes from the transposed upload.
			const float lr = M[ 0 ] * r + M[ 1 ] * g + M[ 2 ] * b;
			const float lg = M[ 3 ] * r + M[ 4 ] * g + M[ 5 ] * b;
			const float lb = M[ 6 ] * r + M[ 7 ] * g + M[ 8 ] * b;

			dst[ 0 ] = lr;
			dst[ 1 ] = lg;
			dst[ 2 ] = lb;
			dst[ 3 ] = lr * kLumaR + lg * kLumaG + lb * kLumaB;
		}
	}
}

//= mirrored from kProcess's main(). Edit both.
void ProcessRow( const chain::Uniforms& u, const float* linear, const float* source, int width, int height, int y, int x0, int x1,
                 float* out )
{
	const Linear lin = { linear, width, height };

	for( int x = x0; x < x1; ++x, out += 4 )
	{
		const size_t at  = ( static_cast< size_t >( y ) * width + x ) * 4;
		const float* here = linear + at;
		const float* src  = source + at;
		float r           = here[ 0 ];
		float g           = here[ 1 ];
		float b           = here[ 2 ];
		const float Y     = here[ 3 ];

		//--- detail ---------------------------------------------------------
		float d = u.HWeight * lin.highpass( u, x, y, 1, 0, Y ) + u.VWeight * lin.highpass( u, x, y, 0, 1, Y );

		//Perturb 8: the kernel doubled (a negative control).
		if( ( u.Perturb & 8 ) != 0 )
			d *= 2.0f;

		//Coring: a dead zone. Perturb 4 removes it (a negative control).
		if( ( u.Perturb & 4 ) == 0 )
			d = signf( d ) * std::max( std::fabs( d ) - u.CoringDead, 0.0f );

		//Level dependence: less detail where the luma is low.
		const float levelGain = 1.0f - u.LevelDep * ( 1.0f - std::min( 1.0f, Y / u.LevelRef ) );

		//Skin detail: less detail inside a hue window, gated on chroma.
		const float dh  = std::fabs( modf_glsl( hueDegrees( r, g, b ) - u.SkinHue + 540.0f, 360.0f ) - 180.0f );
		float window    = 1.0f - smoothstepf( u.SkinInner * u.SkinWidth, u.SkinWidth, dh );
		const float hi  = std::max( r, std::max( g, b ) );
		const float chr = hi > 0.0f ? ( hi - std::min( r, std::min( g, b ) ) ) / hi : 0.0f;
		window *= smoothstepf( u.SkinChromaLo, u.SkinChromaHi, chr );
		float skinGain = 1.0f - u.SkinSuppress * window;
		//Perturb 32: the window ignored (a negative control).
		if( ( u.Perturb & 32 ) != 0 )
			skinGain = 1.0f;

		const float D = u.DetailLevel * d * levelGain * skinGain;
		r += D;
		g += D;
		b += D;

		//--- knee, per channel, in linear light -----------------------------
		if( u.KneeOn != 0 && ( u.Perturb & 2 ) == 0 )
		{
			r = knee( u, r );
			g = knee( u, g );
			b = knee( u, b );
		}

		//--- gamma ----------------------------------------------------------
		float v[ 3 ] = { encode( u, r ), encode( u, g ), encode( u, b ) };

		//--- black gamma, pedestal, white clip ------------------------------
		for( float& c : v )
		{
			c = blackGamma( u, c );
			c += u.Pedestal;
			c = std::max( std::min( c, u.WhiteClip ), 0.0f );
		}

		if( u.ShowDetail != 0 )
			v[ 0 ] = v[ 1 ] = v[ 2 ] = 0.5f + D;

		//The mix written out, as the shader writes it: at Mix = 1 exactly v.
		for( int c = 0; c < 3; ++c )
			out[ c ] = v[ c ] * u.MixAmount + src[ c ] * ( 1.0f - u.MixAmount );
		out[ 3 ] = src[ 3 ];
	}
}

} // namespace ccu::cpu
