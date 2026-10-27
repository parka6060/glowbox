#include "effects/rim_model.hpp"
#include <cmath>
namespace glowbox {
RimAxes prepare_rim_axes(float angle,float size) {
    // Calibrated sine/cosine fit.

    constexpr double turn=360.182624827992754;
    const double radians=std::remainder(double(angle),turn)*(6.2831853071795864769/turn);
    const double sine=std::sin(radians),cosine=std::cos(radians);
    const auto axis=[&](double sx,double cx,double sy,double cy){
        vec2 v;vec2_set(&v,size*float(sx*sine+cx*cosine),size*float(sy*sine+cy*cosine));return v;
    };
    RimAxes result;
    result.left=axis(-0.003628051608116,0.999993354791679,-0.000000065967517,-0.999999952153559);
    result.right=axis(0.005221592575758,-0.999986303902076,-0.000000065967517,-0.999999952153559);
    result.single=axis(0.999990184767299,0.004424859683438,-0.000000065967517,-0.999999952153559);
    return result;
}
}
