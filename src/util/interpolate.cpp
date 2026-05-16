#include "interpolate.h"

namespace util {

// Evaluate Catmull-Rom at parameter t in [0,1] given four control points.
static float cr_eval(float p0, float p1, float p2, float p3, float t) {
  float t2 = t * t;
  float t3 = t2 * t;
  return 0.5f * ((2.0f * p1)
                 + (-p0 + p2) * t
                 + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

void catmull_rom_interpolate(const float* in, size_t in_n,
                             float* out, size_t out_n,
                             int factor) {
  if (!in || !out || in_n < 2 || out_n == 0) return;

  size_t write = 0;
  size_t segs  = in_n - 1;

  for (size_t seg = 0; seg < segs && write < out_n; ++seg) {
    // Ghost points: duplicate first/last endpoint as boundary condition.
    float p0 = (seg == 0)          ? in[0]          : in[seg - 1];
    float p1 = in[seg];
    float p2 = in[seg + 1];
    float p3 = (seg + 2 < in_n)    ? in[seg + 2]    : in[in_n - 1];

    int steps = (seg == segs - 1) ? factor : factor;
    for (int i = 0; i < steps && write < out_n; ++i) {
      float t = (float)i / (float)factor;
      out[write++] = cr_eval(p0, p1, p2, p3, t);
    }
  }

  // Final endpoint
  if (write < out_n) out[write] = in[in_n - 1];
}

}  // namespace util
