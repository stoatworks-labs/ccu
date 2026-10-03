#include "Chain.h"

#include "Controls.h"

#include <cmath>

namespace ccu::chain
{
namespace
{
float f( double v )
{
	return static_cast< float >( v );
}
} // namespace

Uniforms Resolve( const HostValues& host, double driftWalk, int perturb )
{
	Uniforms u = {};

	//---------------------------------------------------------------------
	// The settings, in physical units, in double.
	//---------------------------------------------------------------------
	const double masterGain = controls::MasterGain( host.masterGain );
	const double pedestal   = controls::MasterBlack( host.masterBlack );
	const double whiteClip  = controls::WhiteClip( host.whiteClip );

	const double shiftMired = controls::DriftMireds( host.drift ) * driftWalk;
	const double gainR      = controls::ChannelGain( host.rGain ) * std::exp( model::kDriftGainPerMired * shiftMired );
	const double gainB      = controls::ChannelGain( host.bGain ) * std::exp( -model::kDriftGainPerMired * shiftMired );

	const int preset         = model::OptionIndex( host.matrix, model::kMatrixCount );
	const model::Mat3 matrix = model::Saturation( controls::Saturation( host.saturation ) ) * model::PresetMatrix( preset );

	const double detailLevel = controls::DetailLevel( host.detailLevel );
	const double spacing     = controls::DetailSpacing( host.detailFreq );
	const int spacingInt     = static_cast< int >( std::floor( spacing ) );
	const double spacingFrac = spacing - spacingInt;
	const double hWeight     = controls::DetailHorizontalWeight( host.hvRatio );
	const double vWeight     = controls::DetailVerticalWeight( host.hvRatio );
	const double coringDead  = controls::CoringEdge( host.coring ) * model::kDetailStepPeak;
	const double levelDep    = controls::LevelDependence( host.levelDep );
	const double skinDetail  = controls::SkinDetail( host.skinDetail );
	const double skinHue     = controls::SkinHueDegrees( host.skinHue );
	const double skinWidth   = controls::SkinWidthDegrees( host.skinWidth );

	const bool kneeOn      = host.kneeOn >= 0.5f;
	const double kneePoint = controls::KneePoint( host.kneePoint );
	//Perturb 16: the slope 10% steeper than the control says (a negative control).
	const double kneeSlope = controls::KneeSlope( host.kneeSlope ) * ( ( perturb & model::kPerturbKneeSlope ) ? 1.1 : 1.0 );

	//Perturb 64: the exponent 0.05 above the control (a negative control).
	const double exponent     = controls::GammaExponent( host.gamma ) + ( ( perturb & model::kPerturbGammaExponent ) ? 0.05 : 0.0 );
	const model::Oetf oetf    = model::OetfFor( exponent );
	const model::Oetf inverse = model::OetfFor( model::kOetfExponentNull );
	const double blackGamma   = controls::BlackGamma( host.blackGamma );

	const bool showDetail = host.showDetail >= 0.5f;
	const double mix      = controls::Mix( host.mix );

	//---------------------------------------------------------------------
	// To float, once, as the GPU receives each one.
	//---------------------------------------------------------------------
	u.MasterGain = f( masterGain );
	u.GainR      = f( gainR );
	u.GainB      = f( gainB );
	for( int i = 0; i < 3; ++i )
		for( int j = 0; j < 3; ++j )
			u.Matrix[ i * 3 + j ] = f( matrix.m[ i ][ j ] );
	u.InvA     = f( inverse.a );
	u.InvC     = f( inverse.a - 1.0 );
	u.InvK     = f( inverse.k );
	u.InvKnee  = f( inverse.knee );
	u.InvGamma = f( 1.0 / inverse.gamma );

	u.Perturb   = perturb;
	u.KneeOn    = kneeOn ? 1 : 0;
	u.KneePoint = f( kneePoint );
	u.KneeSlope = f( kneeSlope );

	u.DetailLevel  = f( detailLevel );
	u.SpacingInt   = spacingInt;
	u.SpacingFrac  = f( spacingFrac );
	u.HWeight      = f( hWeight );
	u.VWeight      = f( vWeight );
	u.CoringDead   = f( coringDead );
	u.LevelDep     = f( levelDep );
	u.LevelRef     = f( model::kLevelDependenceRef );
	u.SkinSuppress = f( skinDetail );
	u.SkinHue      = f( skinHue );
	u.SkinWidth    = f( skinWidth );
	u.SkinInner    = f( model::kSkinInnerFraction );
	u.SkinChromaLo = f( model::kSkinChromaLow );
	u.SkinChromaHi = f( model::kSkinChromaHigh );

	u.OetfA     = f( oetf.a );
	u.OetfC     = f( oetf.a - 1.0 );
	u.OetfK     = f( oetf.k );
	u.OetfBreak = f( model::kOetfBreak );
	u.OetfGamma = f( oetf.gamma );

	u.BlackGamma = f( blackGamma );
	u.BlackLevel = f( model::kBlackGammaLevel );
	u.BlackLift  = f( model::kBlackGammaLift );
	u.Pedestal   = f( pedestal );
	u.WhiteClip  = f( whiteClip );

	u.MixAmount  = f( mix );
	u.ShowDetail = showDetail ? 1 : 0;
	return u;
}

} // namespace ccu::chain
