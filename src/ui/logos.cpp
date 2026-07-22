#include "logos.h"

#include <LittleFS.h>

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "../display/fs_littlefs.h"
#include "logos_data.h"
#include "styles.h"

// LVGL's bundled lodepng (compiled in via LV_USE_LODEPNG in lv_conf.h).
// Declared here rather than including its header so both the PlatformIO and
// sim builds link it without caring where lvgl's src tree sits on the include
// path. NOTE: this is LVGL's fork, whose out-param contract differs from
// upstream lodepng — see the call site in decodeRuntimeLogo.
extern "C" unsigned lodepng_decode32(unsigned char** out, unsigned* w,
                                     unsigned* h, const unsigned char* in,
                                     size_t insize);

namespace {

constexpr size_t MAX_RUNTIME_PNG_BYTES = 64 * 1024;
// Each cached runtime logo holds a decoded 48×48 ARGB8888 bitmap (~9 KB) in
// heap for its whole lifetime. Too many at once (plus WiFi + a ~40 KB TLS
// session per fetch) exhausts the heap and aborts the firmware. Cap how many
// we keep resident; symbols past the cap fall back to the letter badge (which
// costs no heap). Embedded logos live in flash and don't count against this.
// 4, down from 6: each resident bitmap is a durable 6.4 KB heap tenant, and
// past four of them the fragmentation reliably costs the TLS reconnect its
// second 16.7 KB buffer (SSL -32512 loop → transport-watchdog reboot). The
// web UI already tells users extra symbols fall back to lettered badges.
constexpr size_t MAX_RUNTIME_LOGOS = 4;
constexpr unsigned MAX_RUNTIME_LOGO_SIDE = 128;
// 40, down from the API's native 48 (see fetcher's ?size=48): the largest
// on-screen slot is the 38 px list logo, so a 40 px ARGB cache loses no
// visible quality while cutting each resident bitmap from 9.2 KB to 6.4 KB.
// That ~11 KB (at 4 symbols) is what lets the decoded-logo cache coexist
// with the persistent TLS session's buffers on the no-PSRAM CYD — at 48 px
// a TLS reconnect missed fitting its second 16.7 KB buffer by ~700 bytes.
// The decoder's accumulator path handles the 48→40 downscale at decode
// time; LVGL still bilinearly scales 40→38/32 at draw time.
constexpr uint32_t RUNTIME_LOGO_CACHE_SIDE = 40;

// Logo source-kind signatures (high byte = kind, low 24 bits = payload).
// Shared by make()'s mounted-signature out-param and signature()'s probe so
// list_screen can tell when the *actually mounted* logo differs from what is
// now available (e.g. a badge is up but the PNG has since become decodable)
// and rebuild the widget.
constexpr uint32_t SIG_EMBEDDED = 0x01000000u;
constexpr uint32_t SIG_RUNTIME  = 0x02000000u;   // | (file bytes & 0xFFFFFF)
constexpr uint32_t SIG_BADGE    = 0x03000000u;

// A runtime PNG decode transiently needs ~15 KB of small allocations: the
// PNG file buffer (1–3 KB), lodepng's 48×48 RGBA output (9.2 KB) plus its
// Huffman-tree transients, and the resident 40×40 pixel buffer (6.4 KB).
// lodepng inflates straight into the output buffer — unlike the previous
// pngle path, whose pngle_t was a single ~45 KB contiguous malloc (inline
// 32 KB LZ dict + 11 KB inflator) that could never fit once the boot-time
// heap block was carved up; that is what used to make mid-session logo
// mounts impossible and forced a reboot for every newly added symbol.
//
// Still gate on contiguous headroom: decoding while the net task holds a
// TLS request in flight can drive the largest block toward zero, which both
// fails the decode AND (worse) starves LVGL's draw pool mid-redraw. On a
// deferral the badge shows, g_decode_starved tells the net task to drop the
// persistent TLS session, and rebuild_logo's retry sweep mounts the real
// logo a few seconds later.
//
// 24 KB, because the decode's two big transients — the ~9.5 KB decompressed
// scanlines and the 9.2 KB RGBA draw buf — are live at the same time, so a
// single-largest-block gate must cover both plus slack. A 12 KB gate let
// mid-fetch decodes start (largest ~16 KB), fail lodepng's second big alloc
// (error 83), and retry into the same wall every 3 s forever — a badge that
// never resolved plus visible row-rebuild flicker. Post-release the largest
// block recovers to ~30 KB, so 24 KB is reliably reachable mid-session.
constexpr size_t RUNTIME_DECODE_MIN_MAXALLOC = 24 * 1024;

bool runtimeDecodeAffordable() {
  return ESP.getMaxAllocHeap() >= RUNTIME_DECODE_MIN_MAXALLOC;
}

// Set (UI task) when a decode is deferred by the affordability gate; consumed
// (net task) to drop the persistent TLS session whose resident buffers are
// usually what keeps the gate from ever passing again once the heap fragments.
std::atomic<bool> g_decode_starved{false};

// Some brand marks (e.g. MU) are a near-black glyph on a transparent
// background, so they vanish against the dark UI. Sample the ARGB8888 bitmap
// and report whether the *opaque* pixels are essentially black: both very dark
// AND neutral (no real hue). The neutrality test is what keeps dark-but-
// coloured marks that read fine on the dark UI — ASML's blue, a navy, a dark
// green — from getting an unwanted plate; only true black/greyscale qualifies.
// Data is LVGL ARGB8888 == little-endian B,G,R,A (see pngDrawCb). Sampled on a
// coarse grid to stay cheap.
bool logoIsNearBlack(const lv_image_dsc_t* dsc) {
  if (!dsc || !dsc->data) return false;
  uint32_t w = dsc->header.w, h = dsc->header.h;
  if (w == 0 || h == 0) return false;
  uint32_t stride = dsc->header.stride ? dsc->header.stride : w * 4;
  uint32_t stepX = (w + 31) / 32, stepY = (h + 31) / 32;
  if (stepX == 0) stepX = 1;
  if (stepY == 0) stepY = 1;
  uint64_t lumAcc = 0, chromaAcc = 0, alphaAcc = 0;
  for (uint32_t y = 0; y < h; y += stepY) {
    const uint8_t* row = dsc->data + (size_t)y * stride;
    for (uint32_t x = 0; x < w; x += stepX) {
      const uint8_t* px = row + (size_t)x * 4;
      uint8_t a = px[3];
      if (a < 40) continue;  // ignore near-transparent pixels
      uint8_t b = px[0], g = px[1], r = px[2];
      // Rec.601 luma, fixed-point: 0.299R + 0.587G + 0.114B.
      uint32_t lum = (77u * r + 150u * g + 29u * b) >> 8;
      uint8_t hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
      uint8_t lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
      lumAcc += (uint64_t)lum * a;
      chromaAcc += (uint64_t)(hi - lo) * a;  // 0 == perfectly neutral
      alphaAcc += a;
    }
  }
  if (alphaAcc == 0) return false;  // fully transparent — nothing to back
  uint32_t meanLuma = (uint32_t)(lumAcc / alphaAcc);
  uint32_t meanChroma = (uint32_t)(chromaAcc / alphaAcc);
  return meanLuma < 50 && meanChroma < 28;  // near-black AND neutral
}

// White circular plate behind a near-black logo, clipped to the circle so the
// mark reads cleanly — mirrors the letter-badge look.
void applyDarkLogoBackplate(lv_obj_t* img) {
  lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(img, lv_color_white(), 0);
  lv_obj_set_style_radius(img, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_clip_corner(img, true, 0);
}

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

uint8_t clampByte(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 255.0f) return 255;
  return (uint8_t)(v + 0.5f);
}

PixelBounds opaqueBounds(const uint8_t* src, uint32_t srcW, uint32_t srcH) {
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

void samplePremulBgra(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                      float sx, float sy, float out[4]) {
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
      const uint8_t* p = src + ((size_t)ys[yy] * srcW + xs[xx]) * 4;
      float a = (float)p[3] / 255.0f;
      out[0] += (float)p[0] * a * weight;
      out[1] += (float)p[1] * a * weight;
      out[2] += (float)p[2] * a * weight;
      out[3] += (float)p[3] * weight;
    }
  }
}

void resampleBgraFit(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                     uint8_t* dst, uint32_t dstSide) {
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

// `oom` (optional) reports whether a null return was caused by memory
// pressure — the only failure kind the caller can cure by freeing heap
// (dropping the TLS session) and retrying. Structural failures (bad file,
// undecodable PNG, bad dims) leave it false so the caller doesn't churn.
RuntimeLogo* decodeRuntimeLogo(const String& symbol, const String& path,
                               lv_coord_t targetSide, size_t* fileBytes,
                               bool* oom = nullptr) {
  if (targetSide <= 0) return nullptr;
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
      if (oom) *oom = true;
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

  // Allocate the RESIDENT pieces (descriptor + 40×40 pixel buffer) BEFORE
  // the decode: TLSF good-fit places these ~6.5 KB into existing small
  // fragments now, whereas after the decode transients have carved the big
  // contiguous block they'd land in the middle of it — and once the
  // transients free, that stranded resident buffer durably splits the block
  // the next TLS reconnect needs (its second 16.7 KB buffer then misses and
  // the transport watchdog ends up rebooting the device after every added
  // symbol). Same ordering trick as the old boot-time esp_bt_mem_release
  // comment: long-lived small allocations first, big transients second.
  uint32_t outSide = (uint32_t)targetSide;
  size_t pixelBytes = (size_t)outSide * (size_t)outSide * 4;
  auto* logo = static_cast<RuntimeLogo*>(std::calloc(1, sizeof(RuntimeLogo)));
  if (!logo) {
    log_w("[logo] %s runtime descriptor alloc failed", symbol.c_str());
    std::free(png);
    if (oom) *oom = true;
    return nullptr;
  }
  logo->pixels = static_cast<uint8_t*>(std::malloc(pixelBytes));
  if (!logo->pixels) {
    log_w("[logo] %s runtime pixel alloc failed bytes=%u",
          symbol.c_str(), (unsigned)pixelBytes);
    std::free(logo);
    std::free(png);
    if (oom) *oom = true;
    return nullptr;
  }

  // lodepng converts any PNG color type to 8-bit RGBA. Its transients are
  // several small allocations (largest: the w*h*4 output buffer), so this
  // works in the fragmented mid-session heap where pngle's monolithic
  // ~45 KB state never fit.
  //
  // CONTRACT (LVGL's lodepng fork, not upstream): *out is an lv_draw_buf_t*,
  // NOT a raw pixel buffer. The pixels live at ->data in classic lodepng
  // R,G,B,A order with stride 4*w, and the buffer must be released with
  // lv_draw_buf_destroy. Treating the struct pointer as pixel data corrupts
  // the heap ("assert failed: block_trim_free" boot loop).
  lv_draw_buf_t* decoded = nullptr;
  unsigned w = 0, h = 0;
  unsigned err = lodepng_decode32(reinterpret_cast<unsigned char**>(&decoded),
                                  &w, &h, png, bytes);
  std::free(png);
  if (err) {
    log_w("[logo] %s runtime lodepng error %u", symbol.c_str(), err);
    if (decoded) lv_draw_buf_destroy(decoded);
    destroyRuntimeLogo(logo);
    // 83 is lodepng's own out-of-memory; everything else (bad IDAT,
    // unsupported bit depth, …) won't get better on retry.
    if (oom && err == 83) *oom = true;
    return nullptr;
  }
  log_i("[logo] %s runtime source dims=%ux%u target=%ld",
        symbol.c_str(), w, h, (long)targetSide);
  if (!decoded || !decoded->data || w == 0 || h == 0 ||
      w > MAX_RUNTIME_LOGO_SIDE || h > MAX_RUNTIME_LOGO_SIDE) {
    log_w("[logo] %s runtime invalid dims=%ux%u target=%ld",
          symbol.c_str(), w, h, (long)targetSide);
    if (decoded) lv_draw_buf_destroy(decoded);
    destroyRuntimeLogo(logo);
    return nullptr;
  }
  uint8_t* rgba = decoded->data;

  // lodepng emits R,G,B,A; LVGL ARGB8888 wants little-endian B,G,R,A. Swap
  // R/B in place so the resampler reads and writes a single layout.
  size_t npx = (size_t)w * (size_t)h;
  for (size_t i = 0; i < npx; ++i) {
    uint8_t* p = rgba + i * 4;
    uint8_t r = p[0];
    p[0] = p[2];
    p[2] = r;
  }

  // Crop to the opaque bounds and bilinearly fit into the cache square —
  // same visual contract as the old two-pass pngle decode (the resampler
  // memsets the destination, so no pre-zero needed).
  resampleBgraFit(rgba, w, h, logo->pixels, outSide);
  lv_draw_buf_destroy(decoded);

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
                               size_t bytes, bool* capMiss) {
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

  // Cap resident logos to bound heap use. Past the cap, return null so the
  // caller draws a (heap-free) badge instead of decoding another ~9 KB bitmap.
  if (g_runtime_cache.size() >= MAX_RUNTIME_LOGOS) {
    log_w("[logo] %s runtime cache full (%u) — badge fallback to cap RAM",
          symbol.c_str(), (unsigned)g_runtime_cache.size());
    if (capMiss) *capMiss = true;
    return nullptr;
  }

  // Defer a fresh decode (cache hits above are exempt — they cost no heap)
  // while contiguous heap is tight. The persistent TLS session is the usual
  // culprit, so signal the net task to release it; the caller shows a badge
  // and list_screen's retry sweep mounts the real logo a few seconds later.
  if (!runtimeDecodeAffordable()) {
    g_decode_starved.store(true, std::memory_order_relaxed);
    log_w("[logo] %s runtime decode deferred (maxalloc=%u<%u) — badge, retry later",
          symbol.c_str(), (unsigned)ESP.getMaxAllocHeap(),
          (unsigned)RUNTIME_DECODE_MIN_MAXALLOC);
    return nullptr;
  }

  bool oom = false;
  RuntimeLogo* logo =
      decodeRuntimeLogo(symbol, path, RUNTIME_LOGO_CACHE_SIDE, &bytes, &oom);
  if (!logo) {
    // Raise the starved signal only when the decode failed for memory (the
    // gate only sees the single largest block; lodepng error 83 needs two
    // ~9.5 KB blocks at once) — the net task then drops the TLS session and
    // the retry sweep succeeds a few seconds later. A structurally bad PNG
    // must NOT raise it: that failure repeats identically every 3 s sweep,
    // and dropping TLS for it would buy a full re-handshake per fetch cycle
    // forever. The badge stays up instead.
    if (oom) g_decode_starved.store(true, std::memory_order_relaxed);
    return nullptr;
  }
  g_runtime_cache.push_back(CachedLogo{symbol, sig, logo});
  return logo;
}

}  // namespace

namespace logos {

// lodepng allocates per decode and frees everything before returning, so
// there is no persistent decoder state to prepare or release any more.
// Kept as no-ops so callers (boot prewarm bracketing in main.cpp) stay valid.
bool prepareRuntimeDecoder() { return true; }

void releaseRuntimeDecoder() {}

bool consumeDecodeStarved() {
  return g_decode_starved.exchange(false, std::memory_order_relaxed);
}

void clearRuntimeCache() {
  if (g_runtime_cache.empty()) return;
  for (auto& cached : g_runtime_cache) {
    destroyRuntimeLogo(cached.logo);
  }
  g_runtime_cache.clear();
  log_i("[logo] runtime cache cleared");
}

void pruneRuntimeCache(const std::vector<String>& keep) {
  size_t dropped = 0;
  for (auto it = g_runtime_cache.begin(); it != g_runtime_cache.end();) {
    bool wanted = false;
    for (const auto& s : keep) {
      if (it->symbol.equalsIgnoreCase(s)) {
        wanted = true;
        break;
      }
    }
    if (wanted) {
      ++it;
    } else {
      destroyRuntimeLogo(it->logo);
      it = g_runtime_cache.erase(it);
      ++dropped;
    }
  }
  if (dropped) {
    log_i("[logo] runtime cache pruned: %u dropped, %u kept",
          (unsigned)dropped, (unsigned)g_runtime_cache.size());
  }
}

void prewarmRuntimeCache(const std::vector<String>& symbols) {
  size_t warmed = 0;
  for (const auto& s : symbols) {
    String up = s;
    up.toUpperCase();
    if (logos_data::find(up.c_str())) continue;   // embedded — no decode needed
    String path = logoPath(up);
    size_t bytes = 0;
    {
      fs_littlefs::Guard g;
      if (!LittleFS.exists(path)) continue;       // not downloaded yet
      File f = LittleFS.open(path, "r");
      if (f) {
        bytes = f.size();
        f.close();
      }
    }
    if (!bytes) continue;
    bool capMiss = false;
    if (cachedRuntimeLogo(up, path, bytes, &capMiss)) ++warmed;
  }
  if (warmed) {
    log_i("[logo] prewarmed %u runtime logo(s) (maxalloc=%u)",
          (unsigned)warmed, (unsigned)ESP.getMaxAllocHeap());
  }
}

lv_obj_t* make(lv_obj_t* parent, const String& symbol, lv_coord_t size,
               uint32_t* mountedSig) {
  String up = symbol;
  up.toUpperCase();
  if (mountedSig) *mountedSig = SIG_BADGE;  // default unless we mount better

  // Preferred path: compile-time C array (ARGB8888) — bypasses LittleFS
  // and the LVGL FS / lodepng pipeline entirely, so it renders correctly
  // both on the device and in the desktop sim.
  if (auto* dsc = logos_data::find(up.c_str())) {
    log_i("[logo] %s embedded", up.c_str());
    if (mountedSig) *mountedSig = SIG_EMBEDDED;
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, dsc);
    // Scale the image to the requested slot size. 256 = 1.0x.
    if (dsc->header.w > 0) {
      int32_t scale = (size * 256) / dsc->header.w;
      if (scale != 256) lv_image_set_scale(img, scale);
    }
    lv_obj_set_size(img, size, size);
    if (logoIsNearBlack(dsc)) applyDarkLogoBackplate(img);
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
    bool capMiss = false;
    RuntimeLogo* rt = cachedRuntimeLogo(up, path, bytes, &capMiss);
    if (!rt) {
      // Decode/cache-cap miss. Most often this is transient low heap (cache
      // full or OOM), NOT a bad file — the download path already validates PNG
      // completeness. Do NOT delete the cached file here: deleting forces a
      // wasteful re-download (~40 KB TLS) that worsens the memory pressure and
      // spirals into reboots. Just show the badge; the real logo mounts on a
      // later retry once heap frees up.
      log_w("[logo] %s runtime decode/cap miss %s bytes=%u — badge fallback",
            up.c_str(), path.c_str(), (unsigned)bytes);
      if (capMiss && mountedSig) {
        // Cache at capacity: retries can't succeed until rows are rebuilt, and
        // every path that frees slots (clearRuntimeCache) also rebuilds the
        // rows from scratch. Report the runtime sig as mounted so callers stop
        // deleting/recreating this badge on every retry tick.
        *mountedSig = SIG_RUNTIME | (uint32_t)(bytes & 0x00FFFFFFu);
      }
      return makeBadge(parent, symbol, size);
    }

    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, &rt->dsc);
    if (rt->dsc.header.w > 0) {
      int32_t scale = (size * 256) / rt->dsc.header.w;
      if (scale != 256) lv_image_set_scale(img, scale);
    }
    lv_obj_set_size(img, size, size);
    if (logoIsNearBlack(&rt->dsc)) applyDarkLogoBackplate(img);
    if (mountedSig) *mountedSig = SIG_RUNTIME | (uint32_t)(bytes & 0x00FFFFFFu);
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
  if (logos_data::find(up.c_str())) return SIG_EMBEDDED;

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
  if (exists) return SIG_RUNTIME | (uint32_t)(bytes & 0x00FFFFFFu);
  return SIG_BADGE;  // badge fallback
}

}  // namespace logos
