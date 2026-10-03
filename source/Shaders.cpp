#include "Shaders.h"

namespace ccu::shaders
{

const char* const kVertex = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	//Straight through in 0..1 picture space. The host's MaxUV is applied
	//where the host's texture is read and nowhere else.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// 1. linear: the front half of the chain, into an RGBA32F buffer.
//
// Every constant of the OETF arrives as a uniform computed in double on the
// CPU (Model.h, OetfFor), so the linearise here and the encode in the
// process pass are exact inverses in principle and the identity check's
// bound is the float arithmetic alone.
//
//= mirrored on the CPU, statement for statement, by LinearRows in
//= CpuChain.cpp for the OpenFX build. Edit both; `cctest --cpu` holds them
//= together. (Not inside the string: demo/plugin.js carries a character-
//= for-character copy of it that check_shaders.py compares.)
//---------------------------------------------------------------------------
const char* const kLinear = R"(#version 410 core

in vec2 uv;
out vec4 fragColor;

uniform sampler2D InputTexture;
uniform vec2 MaxUV;

uniform float MasterGain;   //linear, exactly 1 at 0 dB
uniform float GainR;        //white balance, drift included
uniform float GainB;
uniform mat3 Matrix;        //Saturation . Preset, row-major on the CPU, transposed on upload

//The inverse OETF at the null exponent: V < Knee ? V / K : ( ( V + C ) / A )^( 1 / g ).
uniform float InvA;
uniform float InvC;         //A - 1
uniform float InvK;
uniform float InvKnee;      //K * 0.018, in video
uniform float InvGamma;     //1 / 0.45

//Only the negative controls read these: with KneeFirst the knee runs here,
//before white balance and the matrix, and the process pass skips it.
uniform int Perturb;
uniform int KneeOn;
uniform float KneePoint;
uniform float KneeSlope;

const vec3 kLuma = vec3( 0.2126, 0.7152, 0.0722 );

float linearise( float v )
{
	//Perturb 1: a plain 2.2 power in place of the inverse OETF -- the
	//rest of the chain then runs in the wrong space (a negative control).
	if( ( Perturb & 1 ) != 0 )
		return pow( v, 2.2 );
	return v < InvKnee ? v / InvK : pow( ( v + InvC ) / InvA, InvGamma );
}

float knee( float c )
{
	return c > KneePoint ? KneePoint + ( c - KneePoint ) * KneeSlope : c;
}

void main()
{
	vec4 src = texture( InputTexture, uv * MaxUV );
	vec3 v   = max( src.rgb, vec3( 0.0 ) );

	//The clip stands in for scene light: linearise, then head-end gain.
	vec3 lin = vec3( linearise( v.r ), linearise( v.g ), linearise( v.b ) ) * MasterGain;

	//Perturb 2: the knee before white balance (a negative control). The
	//order is the whole point of --order: a warm white through WB then the
	//knee compresses in R; through the knee then WB it clips in R.
	if( ( Perturb & 2 ) != 0 && KneeOn != 0 )
		lin = vec3( knee( lin.r ), knee( lin.g ), knee( lin.b ) );

	//White balance: R and B gains about G.
	lin = vec3( lin.r * GainR, lin.g, lin.b * GainB );

	//The matrix, in linear light.
	lin = Matrix * lin;

	fragColor = vec4( lin, dot( lin, kLuma ) );
}
)";

//---------------------------------------------------------------------------
// 2. process: detail, knee, gamma, black gamma, pedestal, white clip, mix.
//
//= mirrored on the CPU, statement for statement, by ProcessRow in
//= CpuChain.cpp for the OpenFX build. Edit both; `cctest --cpu` holds them
//= together.
//---------------------------------------------------------------------------
const char* const kProcess = R"(#version 410 core

in vec2 uv;
out vec4 fragColor;

uniform sampler2D LinearTexture;//rgb linear after the matrix, a = luma
uniform sampler2D InputTexture; //the host's picture: alpha and the mix
uniform vec2 MaxUV;
uniform int PictureW;
uniform int PictureH;

//Detail
uniform float DetailLevel;
uniform int SpacingInt;         //s = SpacingInt + SpacingFrac
uniform float SpacingFrac;
uniform float HWeight;
uniform float VWeight;
uniform float CoringDead;       //the dead zone on the detail signal: Coring / 4
uniform float LevelDep;
uniform float LevelRef;
uniform float SkinSuppress;     //the Skin Detail control; the gain inside the window is 1 - this
uniform float SkinHue;          //degrees
uniform float SkinWidth;        //degrees, half-width
uniform float SkinInner;        //the flat part of the window, as a fraction of the width
uniform float SkinChromaLo;
uniform float SkinChromaHi;

//Knee, in linear light, per channel
uniform int KneeOn;
uniform float KneePoint;
uniform float KneeSlope;

//The OETF at the control's exponent: L < Break ? K L : A L^g - C
uniform float OetfA;
uniform float OetfC;            //A - 1
uniform float OetfK;
uniform float OetfBreak;
uniform float OetfGamma;

//Black gamma, pedestal, clip
uniform float BlackGamma;
uniform float BlackLevel;
uniform float BlackLift;
uniform float Pedestal;
uniform float WhiteClip;

uniform float MixAmount;
uniform int ShowDetail;
uniform int Perturb;

//Clamped to the picture: texelFetch outside the texture is undefined, and
//the edge pixel's neighbour past the edge is the edge pixel itself.
float lumaAt( int x, int y )
{
	return texelFetch( LinearTexture, ivec2( clamp( x, 0, PictureW - 1 ), clamp( y, 0, PictureH - 1 ) ), 0 ).a;
}

//The ( -1/4, 1/2, -1/4 ) kernel at a spacing of SpacingInt + SpacingFrac
//along ( dx, dy ), each far tap the lerp of the two texels either side.
//At a whole spacing the lerp is ( 1 - 0 ) a + 0 b, which is a exactly.
float highpass( int x, int y, int dx, int dy, float centre )
{
	int i   = SpacingInt;
	float f = SpacingFrac;
	float before = ( 1.0 - f ) * lumaAt( x - i * dx, y - i * dy ) + f * lumaAt( x - ( i + 1 ) * dx, y - ( i + 1 ) * dy );
	float after  = ( 1.0 - f ) * lumaAt( x + i * dx, y + i * dy ) + f * lumaAt( x + ( i + 1 ) * dx, y + ( i + 1 ) * dy );
	return 0.5 * centre - 0.25 * ( before + after );
}

float hueDegrees( vec3 c )
{
	float h = degrees( atan( 1.7320508075688772 * ( c.g - c.b ), 2.0 * c.r - c.g - c.b ) );
	return h < 0.0 ? h + 360.0 : h;
}

float knee( float c )
{
	return c > KneePoint ? KneePoint + ( c - KneePoint ) * KneeSlope : c;
}

float encode( float l )
{
	l = max( l, 0.0 );
	return l < OetfBreak ? OetfK * l : OetfA * pow( l, OetfGamma ) - OetfC;
}

float blackGamma( float v )
{
	if( v < BlackLevel )
	{
		float u = v / BlackLevel;
		v += BlackGamma * BlackLift * BlackLevel * u * ( 1.0 - u ) * ( 1.0 - u );
	}
	return v;
}

void main()
{
	ivec2 p   = ivec2( gl_FragCoord.xy );
	vec4 here = texelFetch( LinearTexture, p, 0 );
	vec3 lin  = here.rgb;
	float Y   = here.a;
	vec4 src  = texture( InputTexture, uv * MaxUV );

	//--- detail ---------------------------------------------------------
	float d = HWeight * highpass( p.x, p.y, 1, 0, Y ) + VWeight * highpass( p.x, p.y, 0, 1, Y );

	//Perturb 8: the kernel doubled (a negative control).
	if( ( Perturb & 8 ) != 0 )
		d *= 2.0;

	//Coring: a dead zone. Small detail is not boosted at all. Perturb 4
	//removes it (a negative control).
	if( ( Perturb & 4 ) == 0 )
		d = sign( d ) * max( abs( d ) - CoringDead, 0.0 );

	//Level dependence: less detail where the luma is low.
	float levelGain = 1.0 - LevelDep * ( 1.0 - min( 1.0, Y / LevelRef ) );

	//Skin detail: less detail inside a hue window, gated on chroma so a
	//neutral is never skin. Flat 1 inside SkinInner * SkinWidth, 0 outside
	//SkinWidth: smoothstep clamps exactly at both edges.
	float dh     = abs( mod( hueDegrees( lin ) - SkinHue + 540.0, 360.0 ) - 180.0 );
	float window = 1.0 - smoothstep( SkinInner * SkinWidth, SkinWidth, dh );
	float hi     = max( lin.r, max( lin.g, lin.b ) );
	float chroma = hi > 0.0 ? ( hi - min( lin.r, min( lin.g, lin.b ) ) ) / hi : 0.0;
	window *= smoothstep( SkinChromaLo, SkinChromaHi, chroma );
	float skinGain = 1.0 - SkinSuppress * window;
	//Perturb 32: the window ignored (a negative control).
	if( ( Perturb & 32 ) != 0 )
		skinGain = 1.0;

	float D = DetailLevel * d * levelGain * skinGain;
	lin += vec3( D );

	//--- knee, per channel, in linear light -----------------------------
	//Perturb 2 ran it in the linear pass instead (a negative control).
	if( KneeOn != 0 && ( Perturb & 2 ) == 0 )
		lin = vec3( knee( lin.r ), knee( lin.g ), knee( lin.b ) );

	//--- gamma ----------------------------------------------------------
	vec3 v = vec3( encode( lin.r ), encode( lin.g ), encode( lin.b ) );

	//--- black gamma, pedestal, white clip ------------------------------
	v = vec3( blackGamma( v.r ), blackGamma( v.g ), blackGamma( v.b ) );
	v += vec3( Pedestal );
	v = max( min( v, vec3( WhiteClip ) ), vec3( 0.0 ) );

	if( ShowDetail != 0 )
		v = vec3( 0.5 + D );

	//The mix written out: at MixAmount = 1 this is v * 1 + src * 0, which
	//is exactly v. mix() may be evaluated as src + ( v - src ) * a, which
	//is not (GLSL 4.10 8.3 promises the form, not the rounding).
	vec3 result = v * MixAmount + src.rgb * ( 1.0 - MixAmount );
	fragColor   = vec4( result, src.a );
}
)";

} // namespace ccu::shaders
