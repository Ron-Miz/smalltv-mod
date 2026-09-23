#include "ClaudeFace.h"

// ---------------------------------------------------------------------------
// Geometry — clawd-mochi's EYE_* constants, unchanged.
//
// The eyes sit 40 px above centre (EYE_OY) because on the mochi they share the
// face with a printed body below; on a bare SmallTV that leaves the lower third
// of the panel empty, which is the intended look. Drop EYE_OY to 0 to centre.
// ---------------------------------------------------------------------------
#define EYE_W    30
#define EYE_H    60
#define EYE_GAP 120
#define EYE_OX    0

// How far above centre the pair sits. clawd-mochi uses 40, which on its printed
// body puts the eyes where a face's would be; 0 centres them on the panel.
// Overridable so tools/face_sim.cpp can render the alternatives side by side.
#ifndef EYE_OY
#define EYE_OY   40
#endif

// Horizontal reach of each squint chevron. The source derives it from the eye
// width, giving 15 px against a 60 px arm — a > < so narrow it reads as a
// squiggle at this size, so the reach is widened here to open the angle out.
// EYE_W / 2 restores the source proportion. Both this and EYE_OY stay
// overridable so tools/face_sim.cpp can render the alternatives side by side.
#ifndef CHEV_REACH
#define CHEV_REACH 34
#endif

#define WIGGLE_PX   16   // how far the wiggle throws the pair sideways
#define BLINK_H      6   // normal-eye blink: the bar left when the lid drops
#define SQUISH_H    10   // squish-eye closed: the flat bar the chevrons collapse to
#define CHEV_THK    10   // chevron half-thickness, in stacked lines

static inline int16_t eyeLX(int16_t ox) {
  return (FACE_W - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX + ox;
}
static inline int16_t eyeRX(int16_t ox) { return eyeLX(ox) + EYE_W + EYE_GAP; }
static inline int16_t eyeY()            { return (FACE_H - EYE_H) / 2 - EYE_OY; }
static inline int16_t eyeCY()           { return eyeY() + EYE_H / 2; }

// The rectangle every pose stays inside: the widest wiggle horizontally, and the
// chevron's arms plus their stacked thickness vertically (which reach further
// than the upright bars do). Repainting just this band, rather than the whole
// screen the way the blocking original does, is what removes the flicker.
// The left edge is whichever reaches further out, the wiggle or a wide chevron.
#define CHEV_LX (eyeLX(0) + EYE_W / 2 - CHEV_REACH / 2 - 1)
#define CHEV_RX (eyeRX(0) + EYE_W / 2 + CHEV_REACH / 2 + 1)
#define BAND_X  (eyeLX(-WIGGLE_PX) < CHEV_LX ? eyeLX(-WIGGLE_PX) : CHEV_LX)
#define BAND_Y  (eyeCY() - EYE_H / 2 - CHEV_THK)
#define BAND_R  (eyeRX(WIGGLE_PX) + EYE_W > CHEV_RX ? eyeRX(WIGGLE_PX) + EYE_W : CHEV_RX)
#define BAND_W  (BAND_R - BAND_X)
#define BAND_H  (EYE_H + 2 * CHEV_THK)

// ---------------------------------------------------------------------------
// The script: clawd-mochi's animNormalEyes() and animSquishEyes() unrolled into
// poses, back to back, looping. Holds are the source's delay()s at its default
// animation speed (speedMs() doubles them at speed 1, the shipped default), plus
// the three rests that give the loop somewhere to breathe — the original returns
// to a static view between runs instead of cycling.
// ---------------------------------------------------------------------------
#define ST_NORMAL 0
#define ST_SQUISH 1

struct FaceStep {
  uint8_t  style;
  int8_t   ox;      // wiggle offset (normal eyes only)
  uint8_t  shut;    // lids down / chevrons collapsed
  uint16_t hold;    // ms
};

static const FaceStep kScript[] = {
  { ST_NORMAL,   0, 0, 1400 },   // rest
  { ST_NORMAL, -16, 0,  160 },   // wiggle
  { ST_NORMAL,  16, 0,  160 },
  { ST_NORMAL, -16, 0,  160 },
  { ST_NORMAL,  16, 0,  160 },
  { ST_NORMAL,   0, 0,  160 },
  { ST_NORMAL,   0, 1,  200 },   // blink, twice
  { ST_NORMAL,   0, 0,  140 },
  { ST_NORMAL,   0, 1,  140 },
  { ST_NORMAL,   0, 0, 2200 },   // rest
  { ST_SQUISH,   0, 0,  320 },   // squint, three times
  { ST_SQUISH,   0, 1,  200 },
  { ST_SQUISH,   0, 0,  320 },
  { ST_SQUISH,   0, 1,  200 },
  { ST_SQUISH,   0, 0,  320 },
  { ST_SQUISH,   0, 1,  200 },
  { ST_SQUISH,   0, 0, 2000 },   // rest, then round again
};
static const uint8_t kStepCount = sizeof(kScript) / sizeof(kScript[0]);

static uint8_t  s_step    = 0;
static uint32_t s_stepMs  = 0;

// ---------------------------------------------------------------------------
void faceReset(uint32_t nowMs) {
  s_step   = 0;
  s_stepMs = nowMs;
}

bool faceTick(uint32_t nowMs) {
  if (nowMs - s_stepMs < kScript[s_step].hold) return false;
  s_step   = (uint8_t)((s_step + 1) % kStepCount);
  s_stepMs = nowMs;
  return true;
}

const char* faceStyleName() {
  return kScript[s_step].style == ST_SQUISH ? "squish" : "normal";
}

uint8_t faceStepCount() { return kStepCount; }

// ---------------------------------------------------------------------------
// One half of a squint: a ">" or "<" built from stacked lines, the source's
// drawChevron(). `arm` is the vertical reach of each stroke, `reach` the
// horizontal one; thickness comes from drawing the pair once per offset.
static void drawChevron(FaceCanvas& c, int16_t cx, int16_t cy, int16_t arm,
                        int16_t reach, uint8_t thk, bool rightFacing, uint16_t col) {
  for (int8_t t = -(int8_t)thk; t <= (int8_t)thk; t++) {
    if (rightFacing) {
      c.drawLine(cx - reach / 2, cy - arm + t, cx + reach / 2, cy + t,       col);
      c.drawLine(cx + reach / 2, cy + t,       cx - reach / 2, cy + arm + t, col);
    } else {
      c.drawLine(cx + reach / 2, cy - arm + t, cx - reach / 2, cy + t,       col);
      c.drawLine(cx - reach / 2, cy + t,       cx + reach / 2, cy + arm + t, col);
    }
  }
}

void faceRender(FaceCanvas& c, uint16_t bg, uint16_t ink, bool full) {
  const FaceStep& st = kScript[s_step];

  if (full) c.fillRect(0, 0, FACE_W, FACE_H, bg);
  else      c.fillRect(BAND_X, BAND_Y, BAND_W, BAND_H, bg);

  const int16_t lx = eyeLX(st.ox), rx = eyeRX(st.ox);
  const int16_t ey = eyeY(), cy = eyeCY();

  if (st.style == ST_NORMAL) {
    if (!st.shut) {
      c.fillRect(lx, ey, EYE_W, EYE_H, ink);
      c.fillRect(rx, ey, EYE_W, EYE_H, ink);
    } else {
      c.fillRect(lx, ey + EYE_H / 2 - BLINK_H / 2, EYE_W, BLINK_H, ink);
      c.fillRect(rx, ey + EYE_H / 2 - BLINK_H / 2, EYE_W, BLINK_H, ink);
    }
  } else {
    if (!st.shut) {
      drawChevron(c, lx + EYE_W / 2, cy, EYE_H / 2, CHEV_REACH, CHEV_THK, true,  ink);
      drawChevron(c, rx + EYE_W / 2, cy, EYE_H / 2, CHEV_REACH, CHEV_THK, false, ink);
    } else {
      c.fillRect(lx, cy - SQUISH_H / 2, EYE_W, SQUISH_H, ink);
      c.fillRect(rx, cy - SQUISH_H / 2, EYE_W, SQUISH_H, ink);
    }
  }
}

// The stats screen's corner glyph. Same pair of eyes at roughly a third scale,
// with the gap pulled in so they fit the 40x40 box the header reserves.
void faceBadge(FaceCanvas& c, int16_t x, int16_t y, uint16_t ink) {
  const int16_t w = EYE_W / 3, h = EYE_H / 3, gap = 12;   // 10 x 20, 32 px wide
  const int16_t x0 = x + (40 - (w * 2 + gap)) / 2;
  const int16_t y0 = y + (40 - h) / 2;
  c.fillRect(x0, y0, w, h, ink);
  c.fillRect(x0 + w + gap, y0, w, h, ink);
}
