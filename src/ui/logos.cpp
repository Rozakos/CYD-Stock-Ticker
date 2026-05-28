#include "logos.h"

#include <LittleFS.h>

#include <cstring>
#include <cstdlib>
#include <vector>

#include <lgfx/utility/lgfx_pngle.h>

#include "../display/fs_littlefs.h"
#include "logos_data.h"
#include "styles.h"

namespace {

constexpr size_t MAX_RUNTIME_PNG_BYTES = 64 * 1024;
constexpr unsigned MAX_RUNTIME_LOGO_SIDE = 128;
// Matches the size the API serves now (see fetcher's ?size=48). Caching
// at the source resolution means the pngle callback writes pixels 1:1
// into the slot and LVGL bilinearly scales 48→38 at draw time, the
// same path the embedded logos take. No homebrew downscale.
constexpr uint32_t RUNTIME_LOGO_CACHE_SIDE = 48;

pngle_t* g_pngle = nullptr;

struct RuntimeLogo {
  lv_image_dsc_t dsc;
  uint8_t* pixels;
};

struct CachedLogo {
  String symbol;
  uint32_t signature;
  RuntimeLogo* logo;
};

std::vector<CachedLogo> g_runtime_cache;

struct PixelBounds {
  uint32_t x0;
  uint32_t y0;
  uint32_t x1;
  uint32_t y1;
};

enum class PngDecodePass : uint8_t {
  Bounds,
  Render,
};

struct PngDecodeCtx {
  const uint8_t* png;
  size_t pngSize;
  size_t pos;
  PngDecodePass pass;
  uint32_t srcW;
  uint32_t srcH;
  PixelBounds bounds;
  uint8_t* dst;
  uint32_t dstSide;
  uint32_t outW;
  uint32_t outH;
  uint32_t offX;
  uint32_t offY;
  uint16_t* accum;
};

String logoPath(const String& symbol) {
  String s = symbol;
  s.toUpperCase();
  return String("/logos/") + s + ".png";
}

void freeRuntimeLogo(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
  auto* logo = static_cast<RuntimeLogo*>(lv_event_get_user_data(e));
  if (!logo) return;
  std::free(logo->pixels);
  std::free(logo);
}

void destroyRuntimeLogo(RuntimeLogo* logo) {
  if (!logo) return;
  std::free(logo->pixels);
  std::free(logo);
}

lv_obj_t* makeBadge(lv_obj_t* parent, const String& symbol, lv_coord_t size) {
  lv_obj_t* badge = lv_obj_create(parent);
  lv_obj_set_size(badge, size, size);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(badge, styles::brand_color(symbol), 0);
  lv_obj_set_style_border_width(badge, 0, 0);
  lv_obj_set_style_pad_all(badge, 0, 0);
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

  String letters = symbol;
  letters.toUpperCase();
  if (letters.length() > 2) letters = letters.substring(0, 2);

  lv_obj_t* lbl = lv_label_create(badge);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0x0b0f17), 0);
  if (size >= 48) {
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
  } else if (size >= 32) {
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
  } else {
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  }
  lv_label_set_text(lbl, letters.c_str());
  lv_obj_center(lbl);
  return badge;
}

uint32_t pngReadCb(void* userData, uint8_t* buf, uint32_t len) {
  auto* ctx = static_cast<PngDecodeCtx*>(userData);
  if (!ctx || !ctx->png) return 0;
  size_t avail = ctx->pngSize - ctx->pos;
  if (len > avail) len = (uint32_t)avail;
  if (buf && len) {
    memcpy(buf, ctx->png + ctx->pos, len);
  }
  ctx->pos += len;
  return len;
}

void pngDrawCb(void* userData, uint32_t x, uint32_t y,
               uint_fast8_t divX, size_t len, const uint8_t* argb) {
  auto* ctx = static_cast<PngDecodeCtx*>(userData);
  if (!ctx || !argb || !ctx->srcW || !ctx->srcH || y >= ctx->srcH) {
    return;
  }

  for (size_t i = 0; i < len; ++i) {
    uint32_t px = x + (uint32_t)i * divX;
    if (px >= ctx->srcW) continue;

    const uint8_t* in = argb + i * 4;
    if (ctx->pass == PngDecodePass::Bounds) {
      if (in[0] > 8) {
        if (px < ctx->bounds.x0) ctx->bounds.x0 = px;
        if (y < ctx->bounds.y0) ctx->bounds.y0 = y;
        if (px + 1 > ctx->bounds.x1) ctx->bounds.x1 = px + 1;
        if (y + 1 > ctx->bounds.y1) ctx->bounds.y1 = y + 1;
      }
      continue;
    }

    if (!ctx->dst || !ctx->dstSide || !ctx->outW || !ctx->outH ||
        px < ctx->bounds.x0 || px >= ctx->bounds.x1 ||
        y < ctx->bounds.y0 || y >= ctx->bounds.y1) {
      continue;
    }

    uint32_t cropW = ctx->bounds.x1 - ctx->bounds.x0;
    uint32_t cropH = ctx->bounds.y1 - ctx->bounds.y0;
    uint32_t relX = px - ctx->bounds.x0;
    uint32_t relY = y - ctx->bounds.y0;
    if (ctx->accum) {
      uint32_t dx = ctx->offX + (relX * ctx->outW) / cropW;
      uint32_t dy = ctx->offY + (relY * ctx->outH) / cropH;
      if (dx >= ctx->dstSide) dx = ctx->dstSide - 1;
      if (dy >= ctx->dstSide) dy = ctx->dstSide - 1;
      uint16_t* acc = ctx->accum + ((size_t)dy * ctx->dstSide + dx) * 5;
      acc[0] += in[3];
      acc[1] += in[2];
      acc[2] += in[1];
      acc[3] += in[0];
      acc[4] += 1;
    } else {
      uint32_t dx0 = ctx->offX + (relX * ctx->outW) / cropW;
      uint32_t dx1 = ctx->offX + ((relX + 1) * ctx->outW + cropW - 1) / cropW;
      uint32_t dy0 = ctx->offY + (relY * ctx->outH) / cropH;
      uint32_t dy1 = ctx->offY + ((relY + 1) * ctx->outH + cropH - 1) / cropH;
      if (dx1 <= dx0) dx1 = dx0 + 1;
      if (dy1 <= dy0) dy1 = dy0 + 1;
      if (dx1 > ctx->dstSide) dx1 = ctx->dstSide;
      if (dy1 > ctx->dstSide) dy1 = ctx->dstSide;

      for (uint32_t yy = dy0; yy < dy1; ++yy) {
        for (uint32_t xx = dx0; xx < dx1; ++xx) {
          uint8_t* out = ctx->dst + ((size_t)yy * ctx->dstSide + xx) * 4;
          // pngle emits A,R,G,B. LVGL ARGB8888 wants little-endian B,G,R,A.
          out[0] = in[3];
          out[1] = in[2];
          out[2] = in[1];
          out[3] = in[0];
        }
      }
    }
  }
}

uint8_t clampByte(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 255.0f) return 255;
  return (uint8_t)(v + 0.5f);
}

PixelBounds opaqueBounds(const std::vector<uint8_t>& src, uint32_t srcW,
                         uint32_t srcH) {
  PixelBounds b{srcW, srcH, 0, 0};
  for (uint32_t y = 0; y < srcH; ++y) {
    for (uint32_t x = 0; x < srcW; ++x) {
      uint8_t a = src[((size_t)y * srcW + x) * 4 + 3];
      if (a > 8) {
        if (x < b.x0) b.x0 = x;
        if (y < b.y0) b.y0 = y;
        if (x + 1 > b.x1) b.x1 = x + 1;
        if (y + 1 > b.y1) b.y1 = y + 1;
      }
    }
  }
  if (b.x0 >= b.x1 || b.y0 >= b.y1) return PixelBounds{0, 0, srcW, srcH};
  return b;
}

void samplePremulBgra(const std::vector<uint8_t>& src, uint32_t srcW,
                      uint32_t srcH, float sx, float sy, float out[4]) {
  if (sx < 0.0f) sx = 0.0f;
  if (sy < 0.0f) sy = 0.0f;
  if (sx > (float)(srcW - 1)) sx = (float)(srcW - 1);
  if (sy > (float)(srcH - 1)) sy = (float)(srcH - 1);

  uint32_t x0 = (uint32_t)sx;
  uint32_t y0 = (uint32_t)sy;
  uint32_t x1 = (x0 + 1 < srcW) ? x0 + 1 : x0;
  uint32_t y1 = (y0 + 1 < srcH) ? y0 + 1 : y0;
  float fx = sx - (float)x0;
  float fy = sy - (float)y0;

  out[0] = out[1] = out[2] = out[3] = 0.0f;
  const uint32_t xs[2] = {x0, x1};
  const uint32_t ys[2] = {y0, y1};
  for (int yy = 0; yy < 2; ++yy) {
    float wy = yy ? fy : (1.0f - fy);
    for (int xx = 0; xx < 2; ++xx) {
      float wx = xx ? fx : (1.0f - fx);
      float weight = wx * wy;
      const uint8_t* p = src.data() + ((size_t)ys[yy] * srcW + xs[xx]) * 4;
      float a = (float)p[3] / 255.0f;
      out[0] += (float)p[0] * a * weight;
      out[1] += (float)p[1] * a * weight;
      out[2] += (float)p[2] * a * weight;
      out[3] += (float)p[3] * weight;
    }
  }
}

void resampleBgraFit(const std::vector<uint8_t>& src, uint32_t srcW,
                     uint32_t srcH, uint8_t* dst, uint32_t dstSide) {
  memset(dst, 0, (size_t)dstSide * dstSide * 4);
  PixelBounds b = opaqueBounds(src, srcW, srcH);
  uint32_t cropW = b.x1 - b.x0;
  uint32_t cropH = b.y1 - b.y0;
  float scaleX = (float)dstSide / (float)cropW;
  float scaleY = (float)dstSide / (float)cropH;
  float scale = (scaleX < scaleY) ? scaleX : scaleY;
  uint32_t outW = (uint32_t)((float)cropW * scale + 0.5f);
  uint32_t outH = (uint32_t)((float)cropH * scale + 0.5f);
  if (outW < 1) outW = 1;
  if (outH < 1) outH = 1;
  if (outW > dstSide) outW = dstSide;
  if (outH > dstSide) outH = dstSide;
  uint32_t offX = (dstSide - outW) / 2;
  uint32_t offY = (dstSide - outH) / 2;

  for (uint32_t y = 0; y < outH; ++y) {
    float sy = (float)b.y0 + ((float)y + 0.5f) * (float)cropH /
                              (float)outH - 0.5f;
    for (uint32_t x = 0; x < outW; ++x) {
      float sx = (float)b.x0 + ((float)x + 0.5f) * (float)cropW /
                                (float)outW - 0.5f;
      float premul[4];
      samplePremulBgra(src, srcW, srcH, sx, sy, premul);

      uint8_t* out = dst + ((size_t)(offY + y) * dstSide + (offX + x)) * 4;
      float a = premul[3];
      out[3] = clampByte(a);
      if (a > 0.0f) {
        float invA = 255.0f / a;
        out[0] = clampByte(premul[0] * invA);
        out[1] = clampByte(premul[1] * invA);
        out[2] = clampByte(premul[2] * invA);
      }
    }
  }
}

bool configureRenderBounds(PngDecodeCtx& ctx, uint32_t dstSide) {
  if (ctx.bounds.x0 >= ctx.bounds.x1 || ctx.bounds.y0 >= ctx.bounds.y1) {
    ctx.bounds = PixelBounds{0, 0, ctx.srcW, ctx.srcH};
  }
  uint32_t cropW = ctx.bounds.x1 - ctx.bounds.x0;
  uint32_t cropH = ctx.bounds.y1 - ctx.bounds.y0;
  if (!cropW || !cropH || !dstSide) return false;

  float scaleX = (float)dstSide / (float)cropW;
  float scaleY = (float)dstSide / (float)cropH;
  float scale = (scaleX < scaleY) ? scaleX : scaleY;
  ctx.outW = (uint32_t)((float)cropW * scale + 0.5f);
  ctx.outH = (uint32_t)((float)cropH * scale + 0.5f);
  if (ctx.outW < 1) ctx.outW = 1;
  if (ctx.outH < 1) ctx.outH = 1;
  if (ctx.outW > dstSide) ctx.outW = dstSide;
  if (ctx.outH > dstSide) ctx.outH = dstSide;
  ctx.offX = (dstSide - ctx.outW) / 2;
  ctx.offY = (dstSide - ctx.outH) / 2;
  ctx.dstSide = dstSide;
  return true;
}

void resolveAccumulatedLogo(PngDecodeCtx& ctx) {
  if (!ctx.accum || !ctx.dst || !ctx.dstSide) return;
  for (uint32_t y = 0; y < ctx.dstSide; ++y) {
    for (uint32_t x = 0; x < ctx.dstSide; ++x) {
      uint16_t* acc = ctx.accum + ((size_t)y * ctx.dstSide + x) * 5;
      uint16_t n = acc[4];
      if (!n) continue;
      uint8_t* out = ctx.dst + ((size_t)y * ctx.dstSide + x) * 4;
      out[0] = (uint8_t)((acc[0] + n / 2) / n);
      out[1] = (uint8_t)((acc[1] + n / 2) / n);
      out[2] = (uint8_t)((acc[2] + n / 2) / n);
      out[3] = (uint8_t)((acc[3] + n / 2) / n);
    }
  }
}

RuntimeLogo* decodeRuntimeLogo(const String& symbol, const String& path,
                               lv_coord_t targetSide, size_t* fileBytes) {
  uint8_t* png = nullptr;
  size_t bytes = 0;
  {
    fs_littlefs::Guard g;
    File f = LittleFS.open(path, "rb");
    if (!f) return nullptr;
    bytes = f.size();
    if (fileBytes) *fileBytes = bytes;
    if (bytes == 0 || bytes > MAX_RUNTIME_PNG_BYTES) {
      log_w("[logo] %s runtime skip bytes=%u",
            symbol.c_str(), (unsigned)bytes);
      return nullptr;
    }
    png = static_cast<uint8_t*>(std::malloc(bytes));
    if (!png) {
      log_w("[logo] %s runtime png buffer alloc failed bytes=%u",
            symbol.c_str(), (unsigned)bytes);
      return nullptr;
    }
    size_t got = f.readBytes(reinterpret_cast<char*>(png), bytes);
    f.close();
    if (got != bytes) {
      log_w("[logo] %s runtime short read got=%u want=%u",
            symbol.c_str(), (unsigned)got, (unsigned)bytes);
      std::free(png);
      return nullptr;
    }
  }

  if (!logos::prepareRuntimeDecoder()) {
    log_w("[logo] %s runtime pngle alloc failed", symbol.c_str());
    std::free(png);
    return nullptr;
  }
  pngle_t* pngle = g_pngle;

  PngDecodeCtx ctx{png, bytes, 0, PngDecodePass::Bounds, 0, 0,
                   PixelBounds{0, 0, 0, 0}, nullptr, 0, 0, 0, 0, 0, nullptr};
  if (lgfx_pngle_prepare(pngle, pngReadCb, &ctx) < 0) {
    log_w("[logo] %s runtime prepare failed", symbol.c_str());
    std::free(png);
    return nullptr;
  }
  unsigned w = lgfx_pngle_get_width(pngle);
  unsigned h = lgfx_pngle_get_height(pngle);
  log_i("[logo] %s runtime source dims=%ux%u target=%ld",
        symbol.c_str(), w, h, (long)targetSide);
  if (w == 0 || h == 0 || targetSide <= 0) {
    log_w("[logo] %s runtime invalid dims=%ux%u target=%ld",
          symbol.c_str(), w, h, (long)targetSide);
    std::free(png);
    return nullptr;
  }
  if (w > MAX_RUNTIME_LOGO_SIDE || h > MAX_RUNTIME_LOGO_SIDE) {
    log_w("[logo] %s runtime too large dims=%ux%u",
          symbol.c_str(), w, h);
    std::free(png);
    return nullptr;
  }

  ctx.srcW = w;
  ctx.srcH = h;
  ctx.bounds = PixelBounds{w, h, 0, 0};
  if (lgfx_pngle_decomp(pngle, pngDrawCb) < 0) {
    log_w("[logo] %s runtime decomp failed dims=%ux%u",
          symbol.c_str(), w, h);
    std::free(png);
    return nullptr;
  }

  uint32_t outSide = (uint32_t)targetSide;
  size_t pixelBytes = (size_t)outSide * (size_t)outSide * 4;
  auto* logo = static_cast<RuntimeLogo*>(std::calloc(1, sizeof(RuntimeLogo)));
  if (!logo) {
    log_w("[logo] %s runtime descriptor alloc failed", symbol.c_str());
    std::free(png);
    return nullptr;
  }
  logo->pixels = static_cast<uint8_t*>(std::malloc(pixelBytes));
  if (!logo->pixels) {
    log_w("[logo] %s runtime pixel alloc failed bytes=%u",
          symbol.c_str(), (unsigned)pixelBytes);
    std::free(logo);
    std::free(png);
    return nullptr;
  }
  memset(logo->pixels, 0, pixelBytes);

  if (!configureRenderBounds(ctx, outSide)) {
    log_w("[logo] %s runtime render bounds invalid", symbol.c_str());
    destroyRuntimeLogo(logo);
    std::free(png);
    return nullptr;
  }
  ctx.pos = 0;
  ctx.pass = PngDecodePass::Render;
  ctx.dst = logo->pixels;
  uint32_t cropW = ctx.bounds.x1 - ctx.bounds.x0;
  uint32_t cropH = ctx.bounds.y1 - ctx.bounds.y0;
  // Strict `>`: at source==dest (the common case now that the API
  // serves 48x48 to match RUNTIME_LOGO_CACHE_SIDE) the draw callback's
  // direct-write path does a clean 1:1 copy and the 23 KB accumulator
  // isn't needed.
  if (cropW > ctx.outW && cropH > ctx.outH) {
    size_t accumBytes = (size_t)outSide * outSide * 5 * sizeof(uint16_t);
    ctx.accum = static_cast<uint16_t*>(std::calloc(1, accumBytes));
    if (!ctx.accum) {
      log_w("[logo] %s runtime smooth buffer alloc failed bytes=%u",
            symbol.c_str(), (unsigned)accumBytes);
    }
  }
  if (lgfx_pngle_prepare(pngle, pngReadCb, &ctx) < 0 ||
      lgfx_pngle_decomp(pngle, pngDrawCb) < 0) {
    log_w("[logo] %s runtime render pass failed", symbol.c_str());
    std::free(ctx.accum);
    destroyRuntimeLogo(logo);
    std::free(png);
    return nullptr;
  }
  resolveAccumulatedLogo(ctx);
  std::free(ctx.accum);
  std::free(png);

  logo->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  logo->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
  logo->dsc.header.flags = 0;
  logo->dsc.header.w = outSide;
  logo->dsc.header.h = outSide;
  logo->dsc.header.stride = outSide * 4;
  logo->dsc.header.reserved_2 = 0;
  logo->dsc.data_size = (uint32_t)pixelBytes;
  logo->dsc.data = logo->pixels;
  logo->dsc.reserved = nullptr;
  logo->dsc.reserved_2 = nullptr;
  return logo;
}

RuntimeLogo* cachedRuntimeLogo(const String& symbol, const String& path,
                               size_t bytes) {
  uint32_t sig = 0x02000000u | (uint32_t)(bytes & 0x00FFFFFFu);
  for (const auto& cached : g_runtime_cache) {
    if (cached.symbol == symbol && cached.signature == sig) {
      log_i("[logo] %s runtime cache hit", symbol.c_str());
      return cached.logo;
    }
  }

  for (auto it = g_runtime_cache.begin(); it != g_runtime_cache.end();) {
    if (it->symbol == symbol) {
      destroyRuntimeLogo(it->logo);
      it = g_runtime_cache.erase(it);
    } else {
      ++it;
    }
  }

  RuntimeLogo* logo =
      decodeRuntimeLogo(symbol, path, RUNTIME_LOGO_CACHE_SIDE, &bytes);
  if (!logo) return nullptr;
  g_runtime_cache.push_back(CachedLogo{symbol, sig, logo});
  return logo;
}

}  // namespace

namespace logos {

bool prepareRuntimeDecoder() {
  if (g_pngle) return true;
  g_pngle = lgfx_pngle_new();
  if (g_pngle) {
    log_i("[logo] runtime decoder ready");
    return true;
  }
  log_w("[logo] runtime decoder alloc failed");
  return false;
}

void releaseRuntimeDecoder() {
  if (!g_pngle) return;
  lgfx_pngle_destroy(g_pngle);
  g_pngle = nullptr;
  log_i("[logo] runtime decoder released");
}

void clearRuntimeCache() {
  if (g_runtime_cache.empty()) return;
  for (auto& cached : g_runtime_cache) {
    destroyRuntimeLogo(cached.logo);
  }
  g_runtime_cache.clear();
  log_i("[logo] runtime cache cleared");
}

lv_obj_t* make(lv_obj_t* parent, const String& symbol, lv_coord_t size) {
  String up = symbol;
  up.toUpperCase();

  // Preferred path: compile-time C array (ARGB8888) — bypasses LittleFS
  // and the LVGL FS / lodepng pipeline entirely, so it renders correctly
  // both on the device and in the desktop sim.
  if (auto* dsc = logos_data::find(up.c_str())) {
    log_i("[logo] %s embedded", up.c_str());
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, dsc);
    // Scale the image to the requested slot size. 256 = 1.0x.
    if (dsc->header.w > 0) {
      int32_t scale = (size * 256) / dsc->header.w;
      if (scale != 256) lv_image_set_scale(img, scale);
    }
    lv_obj_set_size(img, size, size);
    return img;
  }

  // Runtime fallback: decode the cached LittleFS PNG to an in-memory
  // ARGB8888 descriptor. This uses the same render path as embedded logos
  // and avoids LVGL's file-backed PNG path, which reports correct
  // dimensions on this device but drops the pixels during draw.
  String path = logoPath(symbol);
  bool exists = false;
  size_t bytes = 0;
  {
    fs_littlefs::Guard g;
    exists = LittleFS.exists(path);
    if (exists) {
      File f = LittleFS.open(path, "r");
      if (f) {
        bytes = f.size();
        f.close();
      }
    }
  }
  if (exists) {
    log_i("[logo] %s runtime.decode.start %s (%u bytes)",
          up.c_str(), path.c_str(), (unsigned)bytes);
    bool hadDecoder = (g_pngle != nullptr);
    RuntimeLogo* rt = cachedRuntimeLogo(up, path, bytes);
    if (!rt) {
      log_w("[logo] %s runtime decode failed %s bytes=%u — dropping cache to re-fetch",
            up.c_str(), path.c_str(), (unsigned)bytes);
      // The cached PNG is unusable (corrupt / truncated / undecodable). Delete
      // it so the next quote refresh re-downloads a fresh copy instead of this
      // file wedging the symbol on the badge fallback forever.
      {
        fs_littlefs::Guard g;
        LittleFS.remove(path);
      }
      if (!hadDecoder) releaseRuntimeDecoder();
      return makeBadge(parent, symbol, size);
    }
    if (!hadDecoder) releaseRuntimeDecoder();

    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, &rt->dsc);
    if (rt->dsc.header.w > 0) {
      int32_t scale = (size * 256) / rt->dsc.header.w;
      if (scale != 256) lv_image_set_scale(img, scale);
    }
    lv_obj_set_size(img, size, size);
    log_i("[logo] %s runtime mounted argb=%ux%u bytes=%u",
          up.c_str(), (unsigned)rt->dsc.header.w, (unsigned)rt->dsc.header.h,
          (unsigned)bytes);
    return img;
  }

  log_i("[logo] %s badge fallback", up.c_str());
  return makeBadge(parent, symbol, size);
}

// Stable signature for "what would make() return right now". Caller uses
// this to detect when the logo source for a symbol has changed (badge ↔
// runtime PNG ↔ embedded ARGB) so a stale logo widget can be rebuilt
// without churning every render tick. High byte = kind, low 24 bits =
// payload (file size for runtime PNG, 0 otherwise).
uint32_t signature(const String& symbol) {
  String up = symbol;
  up.toUpperCase();
  if (logos_data::find(up.c_str())) return 0x01000000u;  // embedded

  String path = logoPath(symbol);
  size_t bytes = 0;
  bool   exists = false;
  {
    fs_littlefs::Guard g;
    if (LittleFS.exists(path)) {
      exists = true;
      File f = LittleFS.open(path, "r");
      if (f) { bytes = f.size(); f.close(); }
    }
  }
  if (exists) return 0x02000000u | (uint32_t)(bytes & 0x00FFFFFFu);
  return 0x03000000u;  // badge fallback
}

}  // namespace logos
