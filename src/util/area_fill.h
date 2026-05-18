#pragma once
#include <lvgl.h>
#include <stdint.h>

namespace util {

// Scanline polygon-fill rasterizer used by the detail chart and the list
// sparklines. Draws a vertical gradient (color at top, transparent at
// bottom) clipped to the polygon whose top edge is the polyline
// (xs[i], ys[i]) for i in [0, n), and whose bottom edge is a horizontal
// line at y = bottom_y. xs/ys are LOCAL to the owning obj; (ox, oy) is
// the obj's absolute top-left so we can emit lv_draw_rect coords on the
// layer.
//
// top_opa is the alpha at row 0 (top). Each row's alpha is interpolated
// linearly to 0 at bottom_y. The polygon is rasterised as one horizontal
// lv_draw_rect per output row — every pixel in a given row uses the SAME
// alpha, so vertical bands cannot appear at trapezoid seams (which was
// the symptom of every previous per-x-column or per-trapezoid attempt).
void draw_polyline_fill(lv_layer_t* layer,
                        const int32_t* xs, const int32_t* ys, int n,
                        int32_t ox, int32_t oy,
                        int32_t bottom_y,
                        lv_color_t color, lv_opa_t top_opa);

}  // namespace util
