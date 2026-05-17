#include <unity.h>
#include "util/interpolate.h"

void setUp() {}
void tearDown() {}

static void test_endpoints_preserved() {
  const float in[]  = {0.0f, 1.0f, 2.0f, 3.0f};
  float out[7] = {};
  util::monotone_cubic_interpolate(in, 4, out, 7, 2);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, out[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, out[6]);
}

// PCHIP must reproduce a perfectly linear input exactly.
static void test_linear_sequence_reproduces_line() {
  const float in[]  = {0.0f, 1.0f, 2.0f, 3.0f};
  float out[7] = {};
  util::monotone_cubic_interpolate(in, 4, out, 7, 2);
  for (int i = 0; i < 7; ++i) {
    float expected = (float)i * 0.5f;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, expected, out[i]);
  }
}

// Headline property: on a monotonic input, every consecutive pair of
// output samples is also monotonic — no overshoot, no ringing. This is
// the regression test for the "wiggle / bump" bug that uniform
// Catmull-Rom produced on noisy stock-close series.
static void test_monotonic_input_stays_monotonic() {
  const float in[] = {
      100.0f, 100.5f, 101.0f, 101.3f, 101.4f, 101.8f,
      102.0f, 102.1f, 102.5f, 103.0f, 103.4f, 104.0f
  };
  const int  in_n   = sizeof(in) / sizeof(in[0]);
  const int  factor = 5;
  const int  out_n  = (in_n - 1) * factor + 1;
  float out[ (12 - 1) * 5 + 1 ];
  util::monotone_cubic_interpolate(in, in_n, out, out_n, factor);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, in[0],      out[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, in[in_n-1], out[out_n-1]);

  for (int i = 1; i < out_n; ++i) {
    TEST_ASSERT_TRUE_MESSAGE(out[i] >= out[i-1] - 1e-4f,
                             "PCHIP output not monotonic on monotonic input");
  }

  float in_min = in[0], in_max = in[0];
  for (int i = 0; i < in_n; ++i) {
    if (in[i] < in_min) in_min = in[i];
    if (in[i] > in_max) in_max = in[i];
  }
  for (int i = 0; i < out_n; ++i) {
    TEST_ASSERT_TRUE_MESSAGE(out[i] >= in_min - 1e-4f,
                             "PCHIP undershoots input minimum");
    TEST_ASSERT_TRUE_MESSAGE(out[i] <= in_max + 1e-4f,
                             "PCHIP overshoots input maximum");
  }
}

static void test_monotonic_decreasing_stays_monotonic() {
  const float in[] = {200.0f, 198.5f, 197.0f, 196.8f, 195.0f, 194.0f, 193.5f, 192.0f};
  const int   in_n   = sizeof(in) / sizeof(in[0]);
  const int   factor = 5;
  const int   out_n  = (in_n - 1) * factor + 1;
  float out[ (8 - 1) * 5 + 1 ];
  util::monotone_cubic_interpolate(in, in_n, out, out_n, factor);

  for (int i = 1; i < out_n; ++i) {
    TEST_ASSERT_TRUE_MESSAGE(out[i] <= out[i-1] + 1e-4f,
                             "PCHIP output not monotonic on decreasing input");
  }
}

static void test_single_segment() {
  const float in[] = {5.0f, 10.0f};
  float out[3] = {};
  util::monotone_cubic_interpolate(in, 2, out, 3, 2);
  TEST_ASSERT_FLOAT_WITHIN(0.001f,  5.0f, out[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, out[2]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_endpoints_preserved);
  RUN_TEST(test_linear_sequence_reproduces_line);
  RUN_TEST(test_monotonic_input_stays_monotonic);
  RUN_TEST(test_monotonic_decreasing_stays_monotonic);
  RUN_TEST(test_single_segment);
  return UNITY_END();
}
