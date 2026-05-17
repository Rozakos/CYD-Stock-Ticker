#pragma once
#include <stddef.h>

namespace util {

// Monotone cubic Hermite (PCHIP / Fritsch-Carlson) interpolation. Computes
// tangents at each input point such that the interpolating cubic is
// monotonic between successive samples that are themselves monotonic — so
// the curve never overshoots above the local max or below the local min of
// its four nearest control points.
//
// Replaces uniform Catmull-Rom, which was producing visible wiggles and
// overshoot on the (noisy but trending) daily stock-close series.
//
// `in_n` input samples → `out_n = (in_n-1)*factor + 1` output samples.
// `out_n` must equal that value; smaller buffers are filled and the rest
// is left untouched. Not re-entrant — uses a static internal slope buffer.
void monotone_cubic_interpolate(const float* in, size_t in_n,
                                float* out, size_t out_n,
                                int factor);

}  // namespace util
