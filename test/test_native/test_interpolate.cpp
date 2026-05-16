#include <unity.h>
#include "util/interpolate.h"

void setUp() {}
void tearDown() {}

// Input [0,1,2,3] with factor=2 should give 5 output points.
// out_n = (4-1)*2 + 1 = 7 — but we test the 5-point case too.
static void test_endpoints_preserved() {
  const float in[]  = {0.0f, 1.0f, 2.0f, 3.0f};
  // out_n = (4-1)*2 + 1 = 7
  float out[7] = {};
  util::catmull_rom_interpolate(in, 4, out, 7, 2);

  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, out[0]);   // first point
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f, out[6]);   // last point
}

static void test_monotone_linear_sequence() {
  const float in[]  = {0.0f, 1.0f, 2.0f, 3.0f};
  float out[7] = {};
  util::catmull_rom_interpolate(in, 4, out, 7, 2);

  // For a perfectly linear sequence Catmull-Rom reproduces the line.
  // Check that all intermediate values are strictly between neighbours.
  for (int i = 1; i < 6; ++i) {
    TEST_ASSERT_TRUE(out[i] > out[i-1]);  // strictly increasing
  }
}

static void test_single_segment() {
  const float in[] = {5.0f, 10.0f};
  float out[3] = {};
  util::catmull_rom_interpolate(in, 2, out, 3, 2);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f,  out[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, out[2]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_endpoints_preserved);
  RUN_TEST(test_monotone_linear_sequence);
  RUN_TEST(test_single_segment);
  return UNITY_END();
}
