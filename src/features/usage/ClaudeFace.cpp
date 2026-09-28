#include "ClaudeFace.h"
#include <string.h>   // strcasecmp, as NotifyTypes.h matches its own presets

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

// The chevron's diagonal is quantised into stairs this wide. Two reasons, one
// cosmetic and one mechanical: the source art is pixel art, and columns that
// share an offset can be emitted as one rectangle. A smooth one-pixel diagonal
// costs one SPI address window per column -- 220 of them for a squint against
// 2 for any flat pose -- and on a panel with no framebuffer that is watched
// being painted on from left to right, which reads as a flicker.
#ifndef CHEV_STEP
#define CHEV_STEP    5
#endif

// Every pose is reached by a tween rather than a jump. A pose's hold from the
// tables below is split into a morph and a rest: the morph is half the hold,
// bounded so a 110 ms flutter still snaps and a 2 s rest does not spend two
// seconds closing an eyelid.
#define FACE_FRAME_MS   33   // ~30 fps while morphing; nothing is drawn at rest
#define MORPH_MIN_MS   120
#define MORPH_MAX_MS   260

// Viewport. The face normally fills the panel, but the usage screen wants the
// same live eyes small above the bars. Rather than a second animation (or a
// canvas that rescales finished rectangles, which rounds seams into the
// difference painting), every length is scaled here at eval time: the shapes
// that come out are already in whole device pixels, so paintEye and paintBent
// stay exact and the small face is as flicker-free as the big one.
static int16_t s_vx = 0, s_vy = 0;
static uint8_t s_vpct = 100;

static inline int16_t sc(int32_t v) { return (int16_t)((v * (int32_t)s_vpct) / 100); }

void faceSetViewport(int16_t x, int16_t y, uint8_t pct) {
  if (pct < 10) pct = 10;
  s_vx = x; s_vy = y; s_vpct = pct;
}
int16_t faceViewW() { return sc(FACE_W); }
int16_t faceViewH() { return sc(FACE_H); }
static inline int16_t eyeW()            { return sc(EYE_W); }

static inline int16_t eyeLX(int16_t ox) {
  return s_vx + sc((FACE_W - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX + ox);
}
static inline int16_t eyeRX(int16_t ox) { return eyeLX(ox) + sc(EYE_W + EYE_GAP); }
static inline int16_t eyeY()            { return s_vy + sc((FACE_H - EYE_H) / 2 - EYE_OY); }
static inline int16_t eyeCY()           { return eyeY() + sc(EYE_H) / 2; }

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
static const FacePose kSquint[] = {          // two unhurried > < squints
  {SH_CHEV, SH_CHEV, 0, 620}, {SH_BAR,  SH_BAR,  0, 360},
  {SH_CHEV, SH_CHEV, 0, 760}, {SH_OPEN, SH_OPEN, 0, 300},
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
// Parametric eye.
//
// Every shape the face can hold is one figure: a run of `w` columns, each a bar
// of height `h`, with the column centres displaced vertically by up to `bend` —
// one bar above the centre line and one below, converging at the apex.
//
//   bend = 0    the two bars coincide: a plain rectangle, w x h
//   bend > 0    they separate along the run into a > or a <
//
// That is what makes the animation smooth. An open eye, a dropped lid and a
// squint stop being three drawings to cut between and become three points in
// the same (w, h, bend) space, so a blink is h falling from 60 to 6, a squint
// is bend rising from 0 to 30, and any pose tweens into any other.
// ---------------------------------------------------------------------------
struct EyeGeom { int16_t w, h, bend; };
struct Box     { int16_t x, y, w, h; };

static const EyeGeom kShapes[4] = {
  {EYE_W,          EYE_H,            0},           // SH_OPEN
  {EYE_W,          BLINK_H,          0},           // SH_SHUT
  {CHEV_REACH + 1, 2 * CHEV_THK + 1, EYE_H / 2},   // SH_CHEV
  {EYE_W,          SQUISH_H,         0},           // SH_BAR
};

// How often each routine is picked, relative to the others, and the window the
// rest between them is drawn from. Both are fixed: the face is an idle
// animation, not a status light, and the spread in the rest is what keeps it
// from settling into a rhythm.
//                                 wig squ win hlf slo gla flu
static const uint8_t kWeights[7] = { 3,  2,  2,  2,  3,  3,  2};

// The calm table, for a face sharing a screen rather than owning one. Lengthening
// the rest between routines was not enough on its own, because most routines are
// bursts in themselves: wiggle ends on two blinks, halfBlink blinks twice and
// flutter is two 110ms blinks back to back. Those three are what read as
// "blinking a lot at once", so calm drops them and leans on the single slow
// blink, leaving the glance and the occasional squint for variety.
//                                 wig squ win hlf slo gla flu
static const uint8_t kCalmWeights[7] = { 0,  2,  1,  0,  5,  3,  0};
#define REST_MIN_MS  900
#define REST_MAX_MS 3200

// ---------------------------------------------------------------------------
// State. xorshift32 rather than the Arduino RNG, so the host harness replays a
// seed exactly and ClaudeFace stays free of Arduino headers.
// ---------------------------------------------------------------------------
static uint32_t s_rng      = 1;
static uint8_t  s_routine  = 0;
static uint8_t  s_pose     = 0;
static uint8_t  s_mirror   = 0;
static bool     s_resting  = true;
static uint16_t s_restHold = 0;

// How long the face sits still between routines, as a multiple of the base
// rest. The idle screen is the whole panel and something happening every
// couple of seconds reads as alive; the same cadence in a 20px header strip
// above the numbers reads as fidgeting, because the eye keeps being pulled
// back to a thing that is not the point of that screen.
static uint8_t s_restMult = 1;
static bool    s_calm     = false;
void faceSetPace(uint8_t restMult, bool calm) {
  s_restMult = restMult ? restMult : 1;
  s_calm     = calm;
}

// Tween: the eyes travel from s_from to s_to over s_morphMs, then hold still
// for s_holdMs. s_progress is the raw 0..255 position, eased at render time.
static EyeGeom  s_from[2], s_to[2];
static int16_t  s_fromOx = 0, s_toOx = 0;
static uint8_t  s_progress   = 255;
static uint16_t s_morphMs    = 0;
static uint16_t s_holdMs     = 0;
static uint32_t s_poseStart  = 0;
static uint32_t s_lastDrawMs = 0;

static Box s_lPrev = {0, 0, 0, 0};   // ink the left eye last occupied
static Box s_rPrev = {0, 0, 0, 0};
static EyeGeom s_lDrawn = {0, 0, 0};  // and the shape it was in, to diff against
static EyeGeom s_rDrawn = {0, 0, 0};

static inline uint32_t rnd() {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return s_rng;
}
static inline uint16_t rndRange(uint16_t lo, uint16_t hi) {
  return (uint16_t)(lo + rnd() % (uint32_t)(hi - lo + 1));
}

// Weighted pick over the mood's table, barring the routine that just played:
// back to back repeats are exactly what reads as a loop. A mood that leans
// heavily on one routine gets to repeat it, though — barring the last pick
// would otherwise turn "error" into a strict alternation between going flat and
// whatever it was allowed to fall back to, the very metronome the exclusion
// exists to prevent.
static uint8_t weightedPick(const uint8_t* w, int16_t exclude) {
  uint16_t all = 0;
  for (uint8_t i = 0; i < kRoutineCount; i++) all = (uint16_t)(all + w[i]);
  if (exclude >= 0 && (uint16_t)(w[exclude] * 2) > all) exclude = -1;

  uint16_t total = 0;
  for (uint8_t i = 0; i < kRoutineCount; i++)
    if (i != exclude) total = (uint16_t)(total + w[i]);
  if (total == 0) return (uint8_t)(exclude >= 0 ? exclude : 0);

  uint16_t r = (uint16_t)(rnd() % total);
  for (uint8_t i = 0; i < kRoutineCount; i++) {
    if (i == exclude) continue;
    if (r < w[i]) return i;
    r = (uint16_t)(r - w[i]);
  }
  return (uint8_t)(exclude >= 0 ? exclude : 0);   // unreachable while total > 0
}

static void pickRoutine() {
  s_routine = weightedPick(s_calm ? kCalmWeights : kWeights, (int16_t)s_routine);
  s_mirror  = kRoutines[s_routine].mirrors ? (uint8_t)(rnd() & 1) : 0;
  s_pose    = 0;
}

static inline const FacePose& curPose() {
  return s_resting ? kRest : kRoutines[s_routine].poses[s_pose];
}

uint16_t faceHoldMs() { return s_resting ? s_restHold : curPose().hold; }

const char* faceRoutineName() { return s_resting ? "rest" : kRoutines[s_routine].name; }

// ---------------------------------------------------------------------------
// Tweening
// ---------------------------------------------------------------------------

// Smoothstep, in integers: 0 and 255 map to themselves, and the curve eases in
// and out so the eyes accelerate off a pose and settle into the next one rather
// than tracking it at a constant speed, which reads as mechanical.
static inline uint8_t ease(uint8_t p) {
  return (uint8_t)(((uint32_t)p * p * (765 - 2 * (uint32_t)p)) / 65025);
}

static inline int16_t lerp16(int16_t a, int16_t b, uint8_t p) {
  return (int16_t)(a + (((int32_t)(b - a) * p) / 255));
}

// Where the eyes are right now, between the pose they left and the one they are
// heading for.
static void evalNow(EyeGeom out[2], int16_t* ox) {
  const uint8_t p = ease(s_progress);
  for (uint8_t i = 0; i < 2; i++) {
    out[i].w    = sc(lerp16(s_from[i].w,    s_to[i].w,    p));
    out[i].h    = sc(lerp16(s_from[i].h,    s_to[i].h,    p));
    out[i].bend = sc(lerp16(s_from[i].bend, s_to[i].bend, p));
  }
  *ox = lerp16(s_fromOx, s_toOx, p);   // scaled with the eye position, not here
}

// Aim the tween at the pose now current. The origin is where the eyes actually
// are, not the pose they were nominally heading for, so a mood cut landing
// mid-morph continues from the shape on screen instead of snapping back.
static void armTween(uint32_t now) {
  EyeGeom cur[2];
  int16_t curOx;
  evalNow(cur, &curOx);
  s_from[0] = cur[0];
  s_from[1] = cur[1];
  s_fromOx  = curOx;

  const FacePose& p = curPose();
  const uint8_t lSh = s_mirror ? p.right : p.left;
  const uint8_t rSh = s_mirror ? p.left  : p.right;
  s_to[0]  = kShapes[lSh];
  s_to[1]  = kShapes[rSh];
  s_toOx   = s_mirror ? (int16_t)(-p.ox) : (int16_t)p.ox;

  const uint16_t hold = faceHoldMs();
  uint16_t morph = (uint16_t)(hold / 2);
  if (morph < MORPH_MIN_MS) morph = MORPH_MIN_MS;
  if (morph > MORPH_MAX_MS) morph = MORPH_MAX_MS;
  if (morph > hold) morph = hold;          // never outrun the pose itself
  s_morphMs = morph;
  s_holdMs  = (uint16_t)(hold - morph);

  s_progress   = 0;
  s_poseStart  = now;
  s_lastDrawMs = now;
}

static void stepPose(uint32_t now) {
  if (s_resting) {
    s_resting = false;
    pickRoutine();
  } else if (s_pose + 1 < kRoutines[s_routine].count) {
    s_pose++;
  } else {
    s_resting  = true;
    // uint16 would overflow at a multiplier of 21, so cap the span rather than
    // letting a long rest wrap into a frantic one.
    uint32_t lo = (uint32_t)REST_MIN_MS * s_restMult;
    uint32_t hi = (uint32_t)REST_MAX_MS * s_restMult;
    if (hi > 60000UL) { hi = 60000UL; if (lo > hi) lo = hi; }
    s_restHold = rndRange((uint16_t)lo, (uint16_t)hi);
  }
  armTween(now);
}

void faceReset(uint32_t nowMs, uint32_t seed) {
  s_rng = (seed ^ (nowMs * 2654435761u)) | 1u;   // xorshift stalls at zero
  s_routine  = (uint8_t)(rnd() % kRoutineCount);
  s_pose     = 0;
  s_mirror   = 0;
  s_resting  = true;                             // open on a calm face
  s_restHold = 800;

  s_from[0] = s_from[1] = s_to[0] = s_to[1] = kShapes[SH_OPEN];
  s_fromOx = s_toOx = 0;
  s_progress   = 255;
  s_morphMs    = 0;
  s_holdMs     = s_restHold;
  s_poseStart  = nowMs;
  s_lastDrawMs = nowMs;

  const Box empty = {0, 0, 0, 0};
  s_lPrev = s_rPrev = empty;
  const EyeGeom none = {0, 0, 0};
  s_lDrawn = s_rDrawn = none;
}

bool faceTick(uint32_t nowMs) {
  const uint32_t el = nowMs - s_poseStart;

  if (el >= (uint32_t)s_morphMs + s_holdMs) {   // pose served its time
    stepPose(nowMs);
    return true;
  }
  if (el < s_morphMs) {                          // mid-morph: draw on the frame clock
    if (nowMs - s_lastDrawMs < FACE_FRAME_MS) return false;
    s_progress   = (uint8_t)((el * 255UL) / s_morphMs);
    s_lastDrawMs = nowMs;
    return true;
  }
  if (s_progress != 255) {                       // land exactly on the pose, once
    s_progress   = 255;
    s_lastDrawMs = nowMs;
    return true;
  }
  return false;                                  // at rest: nothing to draw
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static Box geomBox(const EyeGeom& g, int16_t x) {
  const int16_t cx = (int16_t)(x + eyeW() / 2);
  Box b;
  b.x = (int16_t)(cx - g.w / 2);
  b.w = g.w;
  b.y = (int16_t)(eyeCY() - g.bend - g.h / 2);
  b.h = (int16_t)(2 * g.bend + g.h);
  return b;
}


// One eye. A flat shape (bend 0) is a single rectangle, which is the common case
// and stays one fill; a bent one is drawn column by column, the two bars
// converging on the apex. `apexRight` puts the point of a > on the right.
// Vertical offset of the arm at column `i`, quantised to a stair.
// The stair scales with the viewport, floored at 2px: a fixed 5px step leaves a
// quarter-scale chevron with two stairs, which is a blob rather than a "> <".
static inline int16_t chevStep() {
  const int16_t st = sc(CHEV_STEP);
  return st < 2 ? 2 : st;
}
static int16_t chevOff(const EyeGeom& g, bool apexRight, int16_t i) {
  const int16_t span = (int16_t)(g.w - 1);
  if (span <= 0) return 0;
  const int16_t step = chevStep();
  int16_t n = apexRight ? (int16_t)(span - i) : i;
  n = (int16_t)((n / step) * step);
  return (int16_t)(((int32_t)g.bend * n) / span);
}

static void drawEye(FaceCanvas& c, const EyeGeom& g, int16_t x,
                    bool apexRight, uint16_t ink) {
  if (g.w <= 0 || g.h <= 0) return;
  const int16_t cx = (int16_t)(x + eyeW() / 2);
  const int16_t cy = eyeCY();

  if (g.bend <= 0) {
    c.fillRect((int16_t)(cx - g.w / 2), (int16_t)(cy - g.h / 2), g.w, g.h, ink);
    return;
  }

  const int16_t x0 = (int16_t)(cx - g.w / 2);
  for (int16_t i = 0; i < g.w; ) {
    const int16_t off = chevOff(g, apexRight, i);
    int16_t j = (int16_t)(i + 1);
    while (j < g.w && chevOff(g, apexRight, j) == off) j++;
    const int16_t w = (int16_t)(j - i);
    c.fillRect((int16_t)(x0 + i), (int16_t)(cy - off - g.h / 2), w, g.h, ink);
    if (off) c.fillRect((int16_t)(x0 + i), (int16_t)(cy + off - g.h / 2), w, g.h, ink);
    i = j;
  }
}

// The part of `a` that `b` does not cover, as up to four bands.
static void rectSubtract(FaceCanvas& c, const Box& a, const Box& b, uint16_t col) {
  if (a.w <= 0 || a.h <= 0) return;
  if (b.w <= 0 || b.h <= 0) { c.fillRect(a.x, a.y, a.w, a.h, col); return; }

  const int16_t ax0 = a.x, ay0 = a.y, ax1 = (int16_t)(a.x + a.w), ay1 = (int16_t)(a.y + a.h);
  const int16_t bx0 = b.x, by0 = b.y, bx1 = (int16_t)(b.x + b.w), by1 = (int16_t)(b.y + b.h);
  if (bx1 <= ax0 || bx0 >= ax1 || by1 <= ay0 || by0 >= ay1) {
    c.fillRect(ax0, ay0, a.w, a.h, col);   // disjoint
    return;
  }
  if (by0 > ay0) c.fillRect(ax0, ay0, a.w, (int16_t)(by0 - ay0), col);            // above
  if (by1 < ay1) c.fillRect(ax0, by1, a.w, (int16_t)(ay1 - by1), col);            // below
  const int16_t oy0 = hi16(ay0, by0), oy1 = lo16(ay1, by1);
  if (oy1 > oy0) {
    if (bx0 > ax0) c.fillRect(ax0, oy0, (int16_t)(bx0 - ax0), (int16_t)(oy1 - oy0), col);
    if (bx1 < ax1) c.fillRect(bx1, oy0, (int16_t)(ax1 - bx1), (int16_t)(oy1 - oy0), col);
  }
}

// A column of ink, as up to two runs: the upper and lower bars. They merge into
// one run wherever the bend is small enough that they touch.
struct Span { int16_t y0, y1; };   // [y0, y1)

static uint8_t columnSpans(int16_t cy, int16_t off, int16_t h, Span out[2]) {
  if (h <= 0) return 0;
  const int16_t aTop = (int16_t)(cy - off - h / 2), aBot = (int16_t)(aTop + h);
  const int16_t bTop = (int16_t)(cy + off - h / 2), bBot = (int16_t)(bTop + h);
  if (aBot >= bTop) { out[0].y0 = aTop; out[0].y1 = bBot; return 1; }   // merged
  out[0].y0 = aTop; out[0].y1 = aBot;
  out[1].y0 = bTop; out[1].y1 = bBot;
  return 2;
}

// Where column `x` sits on this shape, if it sits on it at all.
static bool columnOff(const EyeGeom& g, int16_t cx, bool apexRight, int16_t x, int16_t* off) {
  if (g.w <= 0 || g.h <= 0) return false;
  const int16_t i = (int16_t)(x - (cx - g.w / 2));
  if (i < 0 || i >= g.w) return false;
  *off = chevOff(g, apexRight, i);
  return true;
}

// Paint `a` minus the (ordered, disjoint) spans `b`, one pixel wide at x.
static void spanSubtract(FaceCanvas& c, int16_t x, int16_t w, Span a,
                         const Span* b, uint8_t nb, uint16_t col) {
  int16_t cur = a.y0;
  for (uint8_t i = 0; i < nb; i++) {
    if (b[i].y1 <= cur) continue;
    if (b[i].y0 >= a.y1) break;
    if (b[i].y0 > cur) c.fillRect(x, cur, w, (int16_t)(b[i].y0 - cur), col);
    if (b[i].y1 > cur) cur = b[i].y1;
    if (cur >= a.y1) return;
  }
  if (cur < a.y1) c.fillRect(x, cur, w, (int16_t)(a.y1 - cur), col);
}

// The bent-shape counterpart of rectSubtract: walk every column the two shapes
// between them cover, and paint only the runs that differ. A > or < is not a
// rectangle, so the band difference cannot describe it — without this, those
// frames fell back to clearing the whole eye and redrawing it, which is exactly
// the flicker the difference painting exists to remove, and with the session
// states gone the squint and the wink are back in the ordinary rotation where
// it is seen constantly.
static bool sameSpans(const Span* a, uint8_t na, const Span* b, uint8_t nb) {
  if (na != nb) return false;
  for (uint8_t i = 0; i < na; i++)
    if (a[i].y0 != b[i].y0 || a[i].y1 != b[i].y1) return false;
  return true;
}

static void paintBent(FaceCanvas& c, const EyeGeom& oldG, const EyeGeom& newG,
                      int16_t cx, bool apexRight, uint16_t bg, uint16_t ink) {
  const int16_t cy = eyeCY();
  int16_t x0 = (int16_t)(cx - newG.w / 2), x1 = (int16_t)(x0 + newG.w);
  if (oldG.w > 0) {
    const int16_t ox0 = (int16_t)(cx - oldG.w / 2);
    x0 = lo16(x0, ox0);
    x1 = hi16(x1, (int16_t)(ox0 + oldG.w));
  }

  // Columns of a stair share their spans, so a run of them is one rectangle.
  // x1 itself is walked, with no spans, purely to flush the final run.
  Span pOld[2] = {}, pNew[2] = {};
  uint8_t pNo = 0, pNn = 0;
  int16_t runX = x0;
  bool inRun = false;

  for (int16_t x = x0; x <= x1; x++) {
    Span os[2] = {}, ns[2] = {};
    uint8_t no = 0, nn = 0;
    if (x < x1) {
      int16_t off;
      no = columnOff(oldG, cx, apexRight, x, &off) ? columnSpans(cy, off, oldG.h, os) : 0;
      nn = columnOff(newG, cx, apexRight, x, &off) ? columnSpans(cy, off, newG.h, ns) : 0;
      if (inRun && sameSpans(os, no, pOld, pNo) && sameSpans(ns, nn, pNew, pNn)) continue;
    }
    if (inRun) {
      const int16_t w = (int16_t)(x - runX);
      for (uint8_t i = 0; i < pNo; i++) spanSubtract(c, runX, w, pOld[i], pNew, pNn, bg);
      for (uint8_t i = 0; i < pNn; i++) spanSubtract(c, runX, w, pNew[i], pOld, pNo, ink);
    }
    if (x < x1) {
      pOld[0] = os[0]; pOld[1] = os[1]; pNo = no;
      pNew[0] = ns[0]; pNew[1] = ns[1]; pNn = nn;
      runX = x;
      inRun = true;
    }
  }
}

// Paint one eye as the difference between what is on the glass and what should
// be, rather than clearing it and drawing it again.
//
// This is what the animation being "rough" actually was. The panel has no
// framebuffer, so a clear is visible: for one frame the eye is background and
// only then becomes ink again, and at 30 fps that shimmer on every eye every
// frame is the roughness. Painting only the bands that genuinely change never
// touches the part of the eye that stays ink, so there is nothing to shimmer —
// and a blink, where the shape only shrinks, now costs two small erases and no
// drawing at all.
static void paintEye(FaceCanvas& c, const EyeGeom& oldG, const Box& oldB,
                     const EyeGeom& newG, const Box& newB,
                     int16_t x, bool apexRight, uint16_t bg, uint16_t ink) {
  if (oldG.bend > 0 || newG.bend > 0) {
    paintBent(c, oldG, newG, (int16_t)(x + eyeW() / 2), apexRight, bg, ink);
    return;
  }
  // Both flat: the whole eye is one rectangle, so four bands describe the
  // change and the per-column walk is not worth its cost.
  rectSubtract(c, oldB, newB, bg);    // what the eye vacated
  rectSubtract(c, newB, oldB, ink);   // what it moved into
}

void faceRender(FaceCanvas& c, uint16_t bg, uint16_t ink, bool full) {
  EyeGeom g[2];
  int16_t ox;
  evalNow(g, &ox);

  const int16_t lx = eyeLX(ox), rx = eyeRX(ox);
  const Box lNew = geomBox(g[0], lx), rNew = geomBox(g[1], rx);

  c.begin();
  if (full) {
    c.fillRect(s_vx, s_vy, sc(FACE_W), sc(FACE_H), bg);
    drawEye(c, g[0], lx, /*apexRight=*/true,  ink);   // left eye squints as ">"
    drawEye(c, g[1], rx, /*apexRight=*/false, ink);   // right eye as "<"
  } else {
    paintEye(c, s_lDrawn, s_lPrev, g[0], lNew, lx, /*apexRight=*/true,  bg, ink);
    paintEye(c, s_rDrawn, s_rPrev, g[1], rNew, rx, /*apexRight=*/false, bg, ink);
  }
  c.end();

  s_lPrev = lNew;  s_lDrawn = g[0];
  s_rPrev = rNew;  s_rDrawn = g[1];
}

// The stats screen's corner glyph: the same pair of eyes at roughly a third
// scale, with the gap pulled in so they fit the 40x40 box the header reserves.
