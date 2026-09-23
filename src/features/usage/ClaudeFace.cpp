#include "ClaudeFace.h"

// ---------------------------------------------------------------------------
// Geometry — clawd-mochi's EYE_* constants.
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
// EYE_W / 2 restores the source proportion.
#ifndef CHEV_REACH
#define CHEV_REACH 34
#endif

#define BLINK_H      6   // the bar left when the lids drop
#define SQUISH_H    10   // the flat bar the chevrons collapse to
#define CHEV_THK    10   // chevron half-thickness, in stacked pixel columns

// Rest between routines. The spread is the point: a fixed gap would put the
// whole face back on a cycle however random the routines themselves are.
#define REST_MIN_MS  900
#define REST_MAX_MS 3200

static inline int16_t eyeLX(int16_t ox) {
  return (FACE_W - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX + ox;
}
static inline int16_t eyeRX(int16_t ox) { return eyeLX(ox) + EYE_W + EYE_GAP; }
static inline int16_t eyeY()            { return (FACE_H - EYE_H) / 2 - EYE_OY; }
static inline int16_t eyeCY()           { return eyeY() + EYE_H / 2; }

static inline int16_t lo16(int16_t a, int16_t b) { return a < b ? a : b; }
static inline int16_t hi16(int16_t a, int16_t b) { return a > b ? a : b; }

// ---------------------------------------------------------------------------
// Poses. Each eye is posed independently, which is what a wink needs — the
// source only ever drives the pair together.
// ---------------------------------------------------------------------------
#define SH_OPEN 0   // upright bar, the eye wide open
#define SH_SHUT 1   // thin bar, the lid down
#define SH_CHEV 2   // > or < , the happy squint
#define SH_BAR  3   // the flat bar a squint collapses to

struct FacePose {
  uint8_t  left, right;   // SH_*
  int8_t   ox;            // both eyes slide this far sideways
  uint16_t hold;          // ms
};

// Holds are clawd-mochi's delay()s at its shipped animation speed where the
// routine came from it, and chosen to match where it did not.
static const FacePose kWiggle[] = {          // look about, then blink twice
  {SH_OPEN, SH_OPEN, -16, 160}, {SH_OPEN, SH_OPEN,  16, 160},
  {SH_OPEN, SH_OPEN, -16, 160}, {SH_OPEN, SH_OPEN,  16, 160},
  {SH_OPEN, SH_OPEN,   0, 160}, {SH_SHUT, SH_SHUT,   0, 200},
  {SH_OPEN, SH_OPEN,   0, 140}, {SH_SHUT, SH_SHUT,   0, 140},
  {SH_OPEN, SH_OPEN,   0, 260},
};
static const FacePose kSquint[] = {          // three > < squints
  {SH_CHEV, SH_CHEV, 0, 320}, {SH_BAR,  SH_BAR,  0, 200},
  {SH_CHEV, SH_CHEV, 0, 320}, {SH_BAR,  SH_BAR,  0, 200},
  {SH_CHEV, SH_CHEV, 0, 320}, {SH_BAR,  SH_BAR,  0, 200},
  {SH_CHEV, SH_CHEV, 0, 520}, {SH_OPEN, SH_OPEN, 0, 200},
};
static const FacePose kWink[] = {            // one eye squints, the other watches
  {SH_OPEN, SH_OPEN, 0, 200}, {SH_CHEV, SH_OPEN, 0, 420},
  {SH_OPEN, SH_OPEN, 0, 220}, {SH_CHEV, SH_OPEN, 0, 360},
  {SH_OPEN, SH_OPEN, 0, 200},
};
static const FacePose kHalfBlink[] = {       // one lid, twice
  {SH_SHUT, SH_OPEN, 0, 260}, {SH_OPEN, SH_OPEN, 0, 180},
  {SH_SHUT, SH_OPEN, 0, 200}, {SH_OPEN, SH_OPEN, 0, 160},
};
static const FacePose kSlowBlink[] = {
  {SH_OPEN, SH_OPEN, 0, 240}, {SH_SHUT, SH_SHUT, 0, 520},
  {SH_OPEN, SH_OPEN, 0, 300},
};
static const FacePose kGlance[] = {          // look aside, hold it, come back
  {SH_OPEN, SH_OPEN, 18, 240}, {SH_OPEN, SH_OPEN, 18, 900},
  {SH_OPEN, SH_OPEN,  0, 200}, {SH_SHUT, SH_SHUT,  0, 150},
  {SH_OPEN, SH_OPEN,  0, 220},
};
static const FacePose kFlutter[] = {         // two quick blinks
  {SH_SHUT, SH_SHUT, 0, 110}, {SH_OPEN, SH_OPEN, 0, 120},
  {SH_SHUT, SH_SHUT, 0, 110}, {SH_OPEN, SH_OPEN, 0, 200},
};

struct FaceRoutine {
  const char*     name;
  const FacePose* poses;
  uint8_t         count;
  uint8_t         mirrors;   // asymmetric: play it from either side
};

#define ROUTINE(n, arr, m) { n, arr, (uint8_t)(sizeof(arr) / sizeof(arr[0])), m }
static const FaceRoutine kRoutines[] = {
  ROUTINE("wiggle",     kWiggle,    0),
  ROUTINE("squint",     kSquint,    0),
  ROUTINE("wink",       kWink,      1),
  ROUTINE("half blink", kHalfBlink, 1),
  ROUTINE("slow blink", kSlowBlink, 0),
  ROUTINE("glance",     kGlance,    1),
  ROUTINE("flutter",    kFlutter,   0),
};
static const uint8_t kRoutineCount = sizeof(kRoutines) / sizeof(kRoutines[0]);

// The calm pose held between routines.
static const FacePose kRest = {SH_OPEN, SH_OPEN, 0, 0};

// ---------------------------------------------------------------------------
// State. xorshift32 rather than the Arduino RNG, so the host harness replays a
// seed exactly and ClaudeFace stays free of Arduino headers.
// ---------------------------------------------------------------------------
struct Box { int16_t x, y, w, h; };

static uint32_t s_rng      = 1;
static uint8_t  s_routine  = 0;
static uint8_t  s_pose     = 0;
static uint8_t  s_mirror   = 0;
static bool     s_resting  = true;
static uint16_t s_restHold = 0;
static uint32_t s_stepMs   = 0;
static Box      s_lPrev    = {0, 0, 0, 0};   // ink the left eye last occupied
static Box      s_rPrev    = {0, 0, 0, 0};

static inline uint32_t rnd() {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return s_rng;
}
static inline uint16_t rndRange(uint16_t lo, uint16_t hi) {
  return (uint16_t)(lo + rnd() % (uint32_t)(hi - lo + 1));
}

// Never the routine that just played: back to back repeats are exactly what
// reads as a loop. Hopping 1..n-1 picks uniformly among the others.
static void pickRoutine() {
  if (kRoutineCount > 1) {
    uint8_t hop = (uint8_t)(1 + rnd() % (uint32_t)(kRoutineCount - 1));
    s_routine = (uint8_t)((s_routine + hop) % kRoutineCount);
  }
  s_mirror = kRoutines[s_routine].mirrors ? (uint8_t)(rnd() & 1) : 0;
  s_pose = 0;
}

static inline const FacePose& curPose() {
  return s_resting ? kRest : kRoutines[s_routine].poses[s_pose];
}

uint16_t faceHoldMs() { return s_resting ? s_restHold : curPose().hold; }

const char* faceRoutineName() { return s_resting ? "rest" : kRoutines[s_routine].name; }

void faceReset(uint32_t nowMs, uint32_t seed) {
  s_rng = (seed ^ (nowMs * 2654435761u)) | 1u;   // xorshift stalls at zero
  s_routine  = (uint8_t)(rnd() % kRoutineCount);
  s_pose     = 0;
  s_mirror   = 0;
  s_resting  = true;                             // open on a calm face
  s_restHold = 800;
  s_stepMs   = nowMs;
  const Box empty = {0, 0, 0, 0};
  s_lPrev = s_rPrev = empty;
}

bool faceTick(uint32_t nowMs) {
  if (nowMs - s_stepMs < faceHoldMs()) return false;
  if (s_resting) {
    s_resting = false;
    pickRoutine();
  } else if (s_pose + 1 < kRoutines[s_routine].count) {
    s_pose++;
  } else {
    s_resting  = true;
    s_restHold = rndRange(REST_MIN_MS, REST_MAX_MS);
  }
  s_stepMs = nowMs;
  return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// The ink one eye occupies in a given shape, at base x. Also the erase unit:
// a pose repaints the union of where an eye was and where it now is, instead of
// clearing a band wide enough for every pose — which is what left the field
// briefly blank while a squint drew itself in.
static Box eyeBox(uint8_t shape, int16_t x) {
  Box b;
  switch (shape) {
    case SH_SHUT:
      b.x = x; b.y = (int16_t)(eyeY() + EYE_H / 2 - BLINK_H / 2);
      b.w = EYE_W; b.h = BLINK_H;
      break;
    case SH_BAR:
      b.x = x; b.y = (int16_t)(eyeCY() - SQUISH_H / 2);
      b.w = EYE_W; b.h = SQUISH_H;
      break;
    case SH_CHEV:
      b.x = (int16_t)(x + EYE_W / 2 - CHEV_REACH / 2);
      b.y = (int16_t)(eyeCY() - EYE_H / 2 - CHEV_THK);
      b.w = (int16_t)(CHEV_REACH + 1);
      b.h = (int16_t)(EYE_H + 2 * CHEV_THK + 1);
      break;
    default:
      b.x = x; b.y = eyeY(); b.w = EYE_W; b.h = EYE_H;
      break;
  }
  return b;
}

static Box unite(const Box& a, const Box& b) {
  if (a.w == 0 || a.h == 0) return b;
  if (b.w == 0 || b.h == 0) return a;
  Box u;
  u.x = lo16(a.x, b.x);
  u.y = lo16(a.y, b.y);
  u.w = (int16_t)(hi16((int16_t)(a.x + a.w), (int16_t)(b.x + b.w)) - u.x);
  u.h = (int16_t)(hi16((int16_t)(a.y + a.h), (int16_t)(b.y + b.h)) - u.y);
  return u;
}

// A > or < as one 1 px column per step along the arm, each column the full
// stroke thickness. clawd-mochi stacks 2*thk+1 diagonal lines instead, which
// draws the same shape a pixel at a time; this is the same figure in
// (reach+1)*2 rectangle fills.
static void drawChevron(FaceCanvas& c, int16_t cx, int16_t cy, bool rightFacing, uint16_t col) {
  const int16_t arm = EYE_H / 2, reach = CHEV_REACH, thk = CHEV_THK;
  const int16_t x0 = (int16_t)(cx - reach / 2);
  const int16_t h  = (int16_t)(2 * thk + 1);
  for (int16_t i = 0; i <= reach; i++) {
    const int16_t dy = (int16_t)(((int32_t)i * arm) / reach);
    const int16_t x  = rightFacing ? (int16_t)(x0 + i) : (int16_t)(x0 + reach - i);
    c.fillRect(x, (int16_t)(cy - arm + dy - thk), 1, h, col);   // upper arm
    c.fillRect(x, (int16_t)(cy + arm - dy - thk), 1, h, col);   // lower arm
  }
}

static void drawEye(FaceCanvas& c, uint8_t shape, int16_t x, bool rightFacing, uint16_t ink) {
  if (shape == SH_CHEV) {
    drawChevron(c, (int16_t)(x + EYE_W / 2), eyeCY(), rightFacing, ink);
    return;
  }
  const Box b = eyeBox(shape, x);
  c.fillRect(b.x, b.y, b.w, b.h, ink);
}

void faceRender(FaceCanvas& c, uint16_t bg, uint16_t ink, bool full) {
  const FacePose& p = curPose();

  // A mirrored routine swaps the eyes and flips the slide, so a wink comes from
  // whichever side was drawn this time round.
  const uint8_t lSh = s_mirror ? p.right : p.left;
  const uint8_t rSh = s_mirror ? p.left  : p.right;
  const int16_t ox  = s_mirror ? (int16_t)(-p.ox) : (int16_t)p.ox;

  const int16_t lx = eyeLX(ox), rx = eyeRX(ox);
  const Box lNew = eyeBox(lSh, lx), rNew = eyeBox(rSh, rx);

  c.begin();
  if (full) {
    c.fillRect(0, 0, FACE_W, FACE_H, bg);
  } else {
    const Box le = unite(s_lPrev, lNew), re = unite(s_rPrev, rNew);
    c.fillRect(le.x, le.y, le.w, le.h, bg);
    c.fillRect(re.x, re.y, re.w, re.h, bg);
  }
  drawEye(c, lSh, lx, /*rightFacing=*/true,  ink);   // left eye squints as ">"
  drawEye(c, rSh, rx, /*rightFacing=*/false, ink);   // right eye as "<"
  c.end();

  s_lPrev = lNew;
  s_rPrev = rNew;
}

// The stats screen's corner glyph: the same pair of eyes at roughly a third
// scale, with the gap pulled in so they fit the 40x40 box the header reserves.
void faceBadge(FaceCanvas& c, int16_t x, int16_t y, uint16_t ink) {
  const int16_t w = EYE_W / 3, h = EYE_H / 3, gap = 12;   // 10 x 20, 32 px wide
  const int16_t x0 = (int16_t)(x + (40 - (w * 2 + gap)) / 2);
  const int16_t y0 = (int16_t)(y + (40 - h) / 2);
  c.begin();
  c.fillRect(x0, y0, w, h, ink);
  c.fillRect((int16_t)(x0 + w + gap), y0, w, h, ink);
  c.end();
}
