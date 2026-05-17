#include "interpolate.h"

#include <math.h>

namespace util {

// Slope buffer sized for the worst-case input series (HISTORY_POINTS).
// Static because the firmware budget is too tight to allocate this on
// every render; callers are single-threaded behind g_lvglMu.
static constexpr size_t kMaxN = 256;
static float g_secant[kMaxN];   // d_i = y[i+1] - y[i]   (uniform x spacing, h=1)
static float g_tangent[kMaxN];  // m_i — slope at each input knot

// Hermite basis functions for parameter t ∈ [0,1].
static inline float h00(float t, float t2, float t3) { return  2.0f*t3 - 3.0f*t2 + 1.0f; }
static inline float h10(float t, float t2, float t3) { return       t3 - 2.0f*t2 + t;   }
static inline float h01(float t, float t2, float t3) { return -2.0f*t3 + 3.0f*t2;        }
static inline float h11(float t, float t2, float t3) { return       t3 -      t2;        }

void monotone_cubic_interpolate(const float* in, size_t in_n,
                                float* out, size_t out_n,
                                int factor) {
  if (!in || !out || out_n == 0) return;
  if (in_n == 0) return;
  if (in_n == 1) {
    for (size_t k = 0; k < out_n; ++k) out[k] = in[0];
    return;
  }

  // Clamp to the static buffer.
  if (in_n > kMaxN) in_n = kMaxN;
  const size_t segs = in_n - 1;

  // 1. Secant slopes between consecutive input samples.
  for (size_t i = 0; i < segs; ++i) {
    g_secant[i] = in[i + 1] - in[i];
  }

  // 2. Initial tangent estimates (Fritsch-Carlson):
  //    - At a sign change between adjacent secants the slope is forced
  //      to zero, which is what kills overshoot on noisy data.
  //    - Elsewhere we use the arithmetic mean of the two adjacent
  //      secants (this is the "3-point difference" form for h=1).
  //    - The endpoints use the one-sided difference.
  g_tangent[0]      = g_secant[0];
  g_tangent[segs]   = g_secant[segs - 1];
  for (size_t i = 1; i < segs; ++i) {
    float d_prev = g_secant[i - 1];
    float d_next = g_secant[i];
    if (d_prev * d_next <= 0.0f) {
      g_tangent[i] = 0.0f;
    } else {
      g_tangent[i] = 0.5f * (d_prev + d_next);
    }
  }

  // 3. Enforce the Fritsch-Carlson monotonicity condition. If a segment
  //    is flat we pin both endpoint tangents to zero; otherwise we scale
  //    the pair (α, β) = (m_i, m_{i+1}) / d_i so it lives in the circle
  //    α²+β² ≤ 9, which is the necessary and sufficient region for the
  //    Hermite cubic to be monotone in y over [t_i, t_{i+1}].
  for (size_t i = 0; i < segs; ++i) {
    float d = g_secant[i];
    if (d == 0.0f) {
      g_tangent[i]     = 0.0f;
      g_tangent[i + 1] = 0.0f;
      continue;
    }
    float alpha = g_tangent[i]     / d;
    float beta  = g_tangent[i + 1] / d;
    float s = alpha * alpha + beta * beta;
    if (s > 9.0f) {
      float t = 3.0f / sqrtf(s);
      g_tangent[i]     = t * alpha * d;
      g_tangent[i + 1] = t * beta  * d;
    }
  }

  // 4. Hermite cubic eval per segment. h = 1 (uniform spacing) so the
  //    tangent terms don't need a scaling factor.
  size_t write = 0;
  for (size_t seg = 0; seg < segs && write < out_n; ++seg) {
    float y0 = in[seg];
    float y1 = in[seg + 1];
    float m0 = g_tangent[seg];
    float m1 = g_tangent[seg + 1];
    for (int k = 0; k < factor && write < out_n; ++k) {
      float t  = (float)k / (float)factor;
      float t2 = t * t;
      float t3 = t2 * t;
      out[write++] = h00(t, t2, t3) * y0
                   + h10(t, t2, t3) * m0
                   + h01(t, t2, t3) * y1
                   + h11(t, t2, t3) * m1;
    }
  }
  if (write < out_n) out[write] = in[in_n - 1];
}

}  // namespace util
