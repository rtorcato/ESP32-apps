// Orthographic globe, rendered on the device into a PSRAM background buffer.
//
// The view tilt is fixed and only the longitude facing the viewer changes, so
// the expensive inverse projection (asin/atan2 per pixel) is done once at boot
// into two lookup tables: which latitude row and which longitude column each
// pixel sees, before the globe is turned. A render is then two table reads, a
// mask bit and a terminator compare per pixel -- fast enough to do on every
// knob click without a second core.
//
// Sun position is the standard declination approximation with the equation of
// time ignored: at most ~4 degrees, a few pixels of terminator, not worth the
// lines. ponytail: add it if someone notices.
#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>

#include "landmask.h"

#define GLOBE_W 480
#define GLOBE_H 480
#define GLOBE_R 239.0f
#define G_OUTSIDE 0xFFFF

namespace globe {
inline uint16_t *bg = nullptr;       // rendered globe, 480x480 RGB565 -- the erase source
inline uint16_t *latIdx = nullptr;   // per pixel: land-mask row, or G_OUTSIDE
inline uint16_t *lonIdx = nullptr;   // per pixel: land-mask column before turning
inline float tanLat[LANDMASK_H];     // per mask row
inline float cosLon[LANDMASK_W];     // per mask column, cos of that longitude
inline float tilt = 30.0f;           // latitude at the centre of the view

// Colours: dim on purpose so white digits stay readable on top.
inline const uint16_t SEA_DAY = RGB565(16, 46, 96), SEA_NIGHT = RGB565(5, 13, 32);
inline const uint16_t LAND_DAY = RGB565(58, 132, 70), LAND_NIGHT = RGB565(20, 44, 26);

inline bool landAt(uint16_t row, uint16_t col) {
  return LANDMASK[row * (LANDMASK_W / 8) + (col >> 3)] & (0x80 >> (col & 7));
}

// Builds the tables for the current tilt. ~1M trig calls: a few hundred ms.
inline bool begin(float tiltDeg) {
  tilt = tiltDeg;
  if (!bg) bg = (uint16_t *)heap_caps_malloc(GLOBE_W * GLOBE_H * 2, MALLOC_CAP_SPIRAM);
  if (!latIdx) latIdx = (uint16_t *)heap_caps_malloc(GLOBE_W * GLOBE_H * 2, MALLOC_CAP_SPIRAM);
  if (!lonIdx) lonIdx = (uint16_t *)heap_caps_malloc(GLOBE_W * GLOBE_H * 2, MALLOC_CAP_SPIRAM);
  if (!bg || !latIdx || !lonIdx) return false;

  for (int r = 0; r < LANDMASK_H; r++) tanLat[r] = tanf((90.0f - (r + 0.5f) * 180.0f / LANDMASK_H) * (float)M_PI / 180.0f);
  for (int c = 0; c < LANDMASK_W; c++) cosLon[c] = cosf(((c + 0.5f) * 360.0f / LANDMASK_W - 180.0f) * (float)M_PI / 180.0f);

  float lat0 = tilt * (float)M_PI / 180.0f, s0 = sinf(lat0), c0 = cosf(lat0);
  for (int py = 0; py < GLOBE_H; py++) {
    for (int px = 0; px < GLOBE_W; px++) {
      float x = (px + 0.5f - GLOBE_W / 2) / GLOBE_R, y = (GLOBE_H / 2 - (py + 0.5f)) / GLOBE_R;
      float rho2 = x * x + y * y;
      uint32_t i = py * GLOBE_W + px;
      if (rho2 > 1.0f) { latIdx[i] = G_OUTSIDE; lonIdx[i] = 0; continue; }
      float rho = sqrtf(rho2), c = asinf(rho), sc = sinf(c), cc = cosf(c);
      float lat = rho < 1e-6f ? lat0 : asinf(cc * s0 + y * sc * c0 / rho);
      float dlon = rho < 1e-6f ? 0 : atan2f(x * sc, rho * cc * c0 - y * sc * s0);
      int lr = (int)((90.0f - lat * 180.0f / (float)M_PI) * LANDMASK_H / 180.0f);
      int lc = (int)((dlon * 180.0f / (float)M_PI + 180.0f) * LANDMASK_W / 360.0f);
      latIdx[i] = (uint16_t)constrain(lr, 0, LANDMASK_H - 1);
      lonIdx[i] = (uint16_t)(((lc % LANDMASK_W) + LANDMASK_W) % LANDMASK_W);
    }
  }
  return true;
}

// Subsolar point for a UTC instant. Declination from day of year; longitude
// from the hour, 15 degrees per hour west of the noon meridian.
inline void sun(time_t t, float *declDeg, float *lonDeg) {
  struct tm u;
  gmtime_r(&t, &u);
  *declDeg = 23.44f * sinf(2.0f * (float)M_PI * (284 + u.tm_yday + 1) / 365.0f);
  float hours = u.tm_hour + u.tm_min / 60.0f + u.tm_sec / 3600.0f;
  *lonDeg = (12.0f - hours) * 15.0f;
  if (*lonDeg <= -180.0f) *lonDeg += 360.0f;
}

// Renders the globe turned so lon0 faces the viewer, lit by the sun at t,
// into bg. Nothing is drawn: call blit() or let field() copy from bg.
inline void render(float lon0Deg, time_t t) {
  float decl, sunLon;
  sun(t, &decl, &sunLon);
  float tanD = tanf(decl * (float)M_PI / 180.0f);
  int turn = (int)lroundf(lon0Deg * LANDMASK_W / 360.0f);
  int sunCol = (int)lroundf((sunLon + 180.0f) * LANDMASK_W / 360.0f);
  for (uint32_t i = 0; i < GLOBE_W * GLOBE_H; i++) {
    uint16_t la = latIdx[i];
    if (la == G_OUTSIDE) { bg[i] = 0; continue; }
    int lo = (lonIdx[i] + turn) % LANDMASK_W;
    if (lo < 0) lo += LANDMASK_W;
    // Night where the sun is below the horizon: cos(H) < -tan(lat) tan(decl).
    int h = lo - sunCol + LANDMASK_W / 2;  // cosLon is indexed from -180
    h = ((h % LANDMASK_W) + LANDMASK_W) % LANDMASK_W;
    bool night = cosLon[h] < -tanLat[la] * tanD;
    bool land = landAt(la, lo);
    bg[i] = land ? (night ? LAND_NIGHT : LAND_DAY) : (night ? SEA_NIGHT : SEA_DAY);
  }
}

inline void blit(Arduino_GFX *gfx) { gfx->draw16bitRGBBitmap(0, 0, bg, GLOBE_W, GLOBE_H); }

// Restores a rectangle of the background: the erase primitive for anything
// drawn over the globe. Row by row because bg rows are strided.
inline void restore(Arduino_GFX *gfx, int16_t x, int16_t y, int16_t w, int16_t h) {
  for (int16_t r = 0; r < h; r++) gfx->draw16bitRGBBitmap(x, y + r, bg + (uint32_t)(y + r) * GLOBE_W + x, w, 1);
}

// Screen position of a point on the turned globe. False if it's on the far side.
inline bool project(float latDeg, float lonDeg, float lon0Deg, int16_t *sx, int16_t *sy) {
  float lat = latDeg * (float)M_PI / 180.0f, dlon = (lonDeg - lon0Deg) * (float)M_PI / 180.0f;
  float lat0 = tilt * (float)M_PI / 180.0f;
  float cosc = sinf(lat0) * sinf(lat) + cosf(lat0) * cosf(lat) * cosf(dlon);
  if (cosc < -1e-6f) return false;  // the limb itself is visible; cosf(pi/2) is a hair negative
  float x = cosf(lat) * sinf(dlon);
  float y = cosf(lat0) * sinf(lat) - sinf(lat0) * cosf(lat) * cosf(dlon);
  *sx = (int16_t)lroundf(GLOBE_W / 2 + GLOBE_R * x);
  *sy = (int16_t)lroundf(GLOBE_H / 2 - GLOBE_R * y);
  return true;
}

inline void selfCheck() {
  int16_t x, y;
  float saved = tilt;
  tilt = 0;
  assert(project(0, 0, 0, &x, &y) && x == 240 && y == 240);          // facing point is the centre
  assert(project(90, 0, 0, &x, &y) && x == 240 && y == 1);            // north pole at the top
  assert(project(0, 90, 0, &x, &y) && x == 479 && y == 240);          // 90 east is the right edge
  assert(!project(0, 180, 0, &x, &y));                                // antipode hidden
  assert(project(0, 90, 90, &x, &y) && x == 240 && y == 240);         // turning the globe recentres
  tilt = saved;

  float d, l;
  sun(1773835200, &d, &l);  // 2026-03-18 12:00 UTC, near the equinox
  assert(fabsf(d) < 2.0f && fabsf(l) < 1.0f);
  sun(1773835200 + 6 * 3600, &d, &l);  // 18:00 UTC: sun over 90W
  assert(fabsf(l + 90.0f) < 1.0f);

  assert(landAt(LANDMASK_H / 2 - 5, LANDMASK_W / 2 + 40));   // 2.5N 20E: central Africa
  assert(!landAt(LANDMASK_H / 2, LANDMASK_W / 2 - 60));      // 0N 30W: Atlantic
  assert(landAt((90 - 43) * 2, (180 - 79) * 2));             // Toronto
  assert(!landAt(LANDMASK_H / 2 + 60, LANDMASK_W / 2 + 320)); // 30S 160E: Tasman Sea
}
}  // namespace globe
