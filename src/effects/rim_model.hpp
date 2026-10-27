#pragma once
#include <graphics/vec2.h>
namespace glowbox {
struct RimAxes { vec2 single={},left={},right={}; };
// Angle in degrees, size from 0 to 1.
RimAxes prepare_rim_axes(float angle,float size);
}
