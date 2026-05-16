#pragma once
#include <stddef.h>

namespace util {

// Uniform Catmull-Rom spline.  Interpolates `in_n` control points into
// `out_n` output samples (must be >= in_n).  `factor` extra samples are
// inserted between each pair: out_n = (in_n-1)*factor + 1.
// Uses a static internal buffer — not re-entrant, single-call-at-a-time.
void catmull_rom_interpolate(const float* in, size_t in_n,
                             float* out, size_t out_n,
                             int factor);

}  // namespace util
