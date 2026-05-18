#include "area_fill.h"

namespace util {

void draw_polyline_fill(lv_layer_t* layer,
                        const int32_t* xs, const int32_t* ys, int n,
                        int32_t ox, int32_t oy,
                        int32_t bottom_y,
                        lv_color_t color, lv_opa_t top_opa) {
  if (!layer || !xs || !ys) return;
  if (n < 2 || bottom_y <= 0) return;

  lv_draw_rect_dsc_t row_dsc;
  lv_draw_rect_dsc_init(&row_dsc);
  row_dsc.bg_color            = color;
  row_dsc.border_width        = 0;
  row_dsc.bg_grad.dir         = LV_GRAD_DIR_NONE;
  row_dsc.bg_grad.stops_count = 0;

  // Worst-case one crossing per line segment. Sparklines have ≤30 points;
  // the detail chart's smoothed series ≤150.
  static constexpr int MAX_CROSSINGS = 256;
  int32_t crossings[MAX_CROSSINGS];

  const int32_t x_end = xs[n - 1];

  for (int32_t y = 0; y < bottom_y; ++y) {
    int alpha = ((int)top_opa * (bottom_y - y)) / bottom_y;
    if (alpha <= 0) continue;
    row_dsc.bg_opa = (lv_opa_t)alpha;

    // Half-open bracketing so each crossing is counted exactly once.
    int n_cross = 0;
    for (int i = 0; i + 1 < n; ++i) {
      int32_t y0 = ys[i];
      int32_t y1 = ys[i + 1];
      if ((y0 < y && y1 >= y) || (y0 >= y && y1 < y)) {
        int32_t x0 = xs[i];
        int32_t x1 = xs[i + 1];
        int32_t dy = y1 - y0;
        if (dy == 0) continue;
        if (n_cross < MAX_CROSSINGS) {
          crossings[n_cross++] = x0 + ((x1 - x0) * (y - y0)) / dy;
        }
      }
    }

    // Insertion sort — typical n_cross is 0..2 so this is essentially free.
    for (int i = 1; i < n_cross; ++i) {
      int32_t v = crossings[i];
      int j = i;
      while (j > 0 && crossings[j - 1] > v) {
        crossings[j] = crossings[j - 1];
        --j;
      }
      crossings[j] = v;
    }

    bool in = (ys[0] < y);
    int32_t x_curr = xs[0];
    for (int k = 0; k < n_cross; ++k) {
      int32_t x_next = crossings[k];
      if (in && x_next > x_curr) {
        lv_area_t r;
        r.x1 = ox + x_curr;
        r.y1 = oy + y;
        r.x2 = ox + x_next - 1;
        r.y2 = oy + y;
        lv_draw_rect(layer, &row_dsc, &r);
      }
      x_curr = x_next;
      in = !in;
    }
    if (in && x_end > x_curr) {
      lv_area_t r;
      r.x1 = ox + x_curr;
      r.y1 = oy + y;
      r.x2 = ox + x_end;
      r.y2 = oy + y;
      lv_draw_rect(layer, &row_dsc, &r);
    }
  }
}

}  // namespace util
