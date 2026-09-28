#include "UsageMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "UsageClient.h"
#include "ClaudeFace.h"

UsageMode g_usageMode;

// Claude-usage palette (Anthropic-inspired dark theme, RGB565 of the originals).
// Through gfxTint() like the shared palette, so the Display tab's colour
// correction reaches these too.
#define C_ACCENT  gfxTint(0xDBAA)   // terra-cotta 0xd97757
#define C_UGREEN  gfxTint(0x7C6B)   // green 0x788c5d
// The bar's own three colours, kept separate from the palette above. The card
// green was a muted sage (0x788c5d) chosen to sit quietly next to text — which
// is the opposite of what a status bar wants, and is why a 65% amber next to a
// 51% sage did not read as two different colours at a glance.
#define C_BAR_OK    gfxTint(0x262B)   // green  0x22c55e
#define C_BAR_WARN  gfxTint(0xF4E1)   // amber  0xf59e0b
#define C_BAR_FULL  gfxTint(0xE908)   // red    0xef4444
#define C_PANEL   gfxTint(0x18E3)   // card fill 0x1f1f1e
#define C_BARBG   gfxTint(0x2945)   // unfilled bar track 0x2a2a28
#define C_DIM     gfxTint(0xB574)   // secondary text 0xb0aea5

// The idle face draws through FaceCanvas (ClaudeFace.h) rather than touching
// Arduino_GFX directly, which is what lets tools/face_sim.cpp link the same
// renderer on a PC and replay the animation without a device.
//
// begin()/end() map to the driver's own transaction bracket, and the fills go
// to writeFillRect (the unbracketed primitive) rather than fillRect, which
// opens and closes a bus transaction per call. A squint is 140 fills, so that
// is 140 SPI transactions collapsed into one — the difference between the
// chevrons appearing at once and visibly drawing themselves in.
class GfxFaceCanvas : public FaceCanvas {
 public:
  explicit GfxFaceCanvas(Arduino_GFX* g) : g_(g) {}
  void begin() override { g_->startWrite(); }
  void end() override   { g_->endWrite(); }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override {
    g_->writeFillRect(x, y, w, h, c);
  }
 private:
  Arduino_GFX* g_;
};

// ClaudeFace hard-codes the panel it was drawn for; every target's config.h
// agrees, but say so here rather than letting a future 240x280 variant slide.
static_assert(FACE_W == TFT_WIDTH && FACE_H == TFT_HEIGHT,
              "ClaudeFace geometry assumes the 240x240 panel");

// True while the face's field is already painted, so a tick repaints only the
// band the eyes move in instead of the whole screen.
static bool s_facePrimed = false;

static void fmtReset(int mins, char* out, size_t n) {
  if (mins <= 0) { strlcpy(out, "now", n); return; }
  int d = mins / 1440, h = (mins % 1440) / 60, m = mins % 60;
  if (d > 0)      snprintf(out, n, "%dd %dh", d, h);
  else if (h > 0) snprintf(out, n, "%dh %02dm", h, m);
  else            snprintf(out, n, "%dm", m);
}

// The bar answers one question at a glance: how much room is left. Green while
// there is plenty, amber once most of the window is spent, red approaching the
// limit — 60 and 85 rather than 75 and 90, because the useful warning is the
// one that arrives while you can still change what you are doing.
static uint16_t barColor(float pct) {
  if (pct >= 85) return C_BAR_FULL;
  if (pct >= 60) return C_BAR_WARN;
  return C_BAR_OK;
}

// One usage card: big %, a 5h/7d label, a fill bar coloured by load, and the
// reset countdown. `top` is the card's top y; the card is 82px tall.
// One usage card. The default GFX font is 6x8 per size step, so every band
// below is stated in those terms and the gaps are what is left over — the old
// layout put the reset line's top edge exactly on the bar's bottom edge, which
// is why the text looked like it was sitting on top of it.
//
//   top +12 .. +36   the figure, size 3 (24px), left
//   top +14 .. +30   the window label, size 2 (16px), right
//   top +44 .. +60   the reset countdown, size 2 (16px), right, on its own row
//   top +66 .. +74   the bar, 8px
//   top +74 .. +88   bottom padding
//
// The figure started at size 5 against a size-1 countdown, which made everything
// that says what the figure *means* — 5h, 7d, when it resets — too small to read
// at desk distance. Closing that gap from both ends, rather than only enlarging
// the small text, is what keeps the card calm: the bar carries the at-a-glance
// reading, so the number does not have to be the loudest thing on it, and a
// thinner bar reads as a gauge rather than a block of colour.
#define CARD_H 88
static void drawMeter(Arduino_GFX* gfx, int top, const char* label,
                      float pct, int resetMins) {
  const int x = 8, w = 224, pad = 14;
  gfx->fillRoundRect(x, top, w, CARD_H, 10, C_PANEL);

  char pc[8];
  snprintf(pc, sizeof(pc), "%d%%", (int)lroundf(constrain(pct, 0.0f, 100.0f)));
  gfx->setTextSize(gfxFitSize(pc, 110, 3));
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(x + pad, top + 12);
  gfx->print(pc);

  gfx->setTextSize(2);
  gfx->setTextColor(C_ACCENT);
  gfx->setCursor(x + w - pad - gfxTextW(label, 2), top + 14);
  gfx->print(label);

  char rs[16], line[28];
  fmtReset(resetMins, rs, sizeof(rs));
  snprintf(line, sizeof(line), "resets %s", rs);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(x + w - pad - gfxTextW(line, 2), top + 44);
  gfx->print(line);

  const int bx = x + pad, by = top + 66, bw = w - pad * 2, bh = 8;
  gfx->fillRoundRect(bx, by, bw, bh, bh / 2, C_BARBG);
  const int fw = (int)(bw * constrain(pct, 0.0f, 100.0f) / 100.0f);
  if (fw >= bh)    gfx->fillRoundRect(bx, by, fw, bh, bh / 2, barColor(pct));
  else if (fw > 0) gfx->fillRect(bx, by, fw, bh, barColor(pct));
}

// The header is the title and nothing else, centred. The live face lived here
// briefly and came back out: at this size it competed with the numbers instead
// of decorating them. It still owns the idle screen, where it is the point.
#define HDR_TITLE_Y    10
#define CARD1_TOP      48
#define CARD2_TOP     144


// Whole-screen repaints are the only thing on this screen that can read as a
// flash: everything else is painted by difference. Counting them makes "it
// flickers sometimes" answerable from /api/status instead of by watching the
// glass and hoping to catch one.
static uint16_t s_nBarsFull = 0;   // stats layout cleared and redrawn
static uint16_t s_nFaceFull = 0;   // idle face cleared and redrawn
static uint16_t s_nFlips    = 0;   // switches between the two screens
// Which of the two screens is on the glass: -1 unknown, 0 bars, 1 face. Counted
// here rather than from showingFace_, which wake() resets on every carousel
// entry and which therefore cannot tell a flip from an arrival.
static int8_t   s_shownFace = -1;
uint16_t usageBarsFullCount() { return s_nBarsFull; }
uint16_t usageFaceFullCount() { return s_nFaceFull; }
uint16_t usageFlipCount()     { return s_nFlips; }

// Last-drawn state of the accent flag, so a routine update can toggle just the
// dot instead of a full-screen clear.
static bool s_flagShown = false;

// `fullRepaint` clears the static layout only on a real transition — cards and
// the flag dot always repaint their own area, so routine updates skip it.
static void drawUsage(const UsageData& u, bool fullRepaint) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  if (s_shownFace == 1) s_nFlips++;
  s_shownFace = 0;

  if (fullRepaint) {
    s_nBarsFull++;
    s_facePrimed = false;   // force a full redraw next time the idle face shows
    gfx->fillScreen(C_BLACK);

    gfxDrawCentered("USAGE", HDR_TITLE_Y, 3, C_DIM);
    s_flagShown = false;
  }

  if (!u.valid) {
    if (fullRepaint) gfxDrawCentered(u.error ? "daemon error" : "waiting...", 120, 2, C_DIM);
    return;
  }

  // A status that is not calm gets a small accent flag; erasing with black is
  // safe here since nothing else ever draws at x=228. The daemon has shipped two
  // status vocabularies: the old rate-limit headers said "allowed" /
  // "allowed_warning" / "rejected", the usage endpoint says "normal" / "warning"
  // / "rejected". Both calm values clear the flag. Compare exactly, because the
  // old 7-char prefix match on "allowed" also swallowed "allowed_warning".
  bool showFlag = u.status[0]
               && strcmp(u.status, "allowed") != 0
               && strcmp(u.status, "normal")  != 0;
  if (showFlag != s_flagShown || fullRepaint) {
    gfx->fillCircle(228, 18, 5, showFlag ? C_ACCENT : C_BLACK);
    s_flagShown = showFlag;
  }

  drawMeter(gfx, CARD1_TOP, "5h", u.sessionPct, u.sessionResetMin);
  drawMeter(gfx, CARD2_TOP, "7d", u.weeklyPct,  u.weeklyResetMin);
}

// Idle screen: the Claude face on a full-screen terra-cotta field. `restart`
// repaints the whole field; otherwise only the band the eyes occupy is redrawn,
// which is what keeps the animation flicker-free on a panel with no framebuffer.
static void drawFace(bool restart) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  GfxFaceCanvas fc(gfx);
  if (s_shownFace == 0) s_nFlips++;
  s_shownFace = 1;
  const bool full = restart || !s_facePrimed;
  if (full) s_nFaceFull++;
  faceSetViewport(0, 0, 100);        // back to the whole panel
  faceRender(fc, C_ACCENT, C_BLACK, full);
  s_facePrimed = true;
}

// The daemon re-POSTs on a fixed timer even when nothing changed, and
// drawUsage() does a full fillScreen — without this check every push flashes.
bool UsageMode::contentChanged(const UsageData& u) const {
  if (!contentPrimed_) return true;
  return u.valid != lastValid_
      || u.error != lastError_
      || u.sessionPct != lastSessionPct_
      || u.weeklyPct != lastWeeklyPct_
      || u.sessionResetMin != lastSessionResetMin_
      || u.weeklyResetMin != lastWeeklyResetMin_
      || strncmp(u.status, lastStatus_, sizeof(lastStatus_)) != 0;
}

void UsageMode::rememberContent(const UsageData& u) {
  contentPrimed_ = true;
  lastValid_ = u.valid;
  lastError_ = u.error;
  lastSessionPct_ = u.sessionPct;
  lastWeeklyPct_ = u.weeklyPct;
  lastSessionResetMin_ = u.sessionResetMin;
  lastWeeklyResetMin_ = u.weeklyResetMin;
  strlcpy(lastStatus_, u.status, sizeof(lastStatus_));
}

// Worth a carousel stop only with fresh *and* parseable numbers on hand. Both
// halves matter: stale data gives the idle face, and a reading that arrived but
// did not parse gives a near-empty screen reading "waiting...", and neither is
// something to rotate onto.
bool usageHasNumbers(const Settings& s) {
  const uint32_t staleMs = (uint32_t)s.usage.pollSec * 1000UL * 6UL + USAGE_STALE_GRACE_MS;
  return usageFresh(staleMs) && usageGet().valid;
}

// ---- DisplayMode ----------------------------------------------------------
void UsageMode::begin(const Settings& s) {
  usageInit(s);
  faceReset(millis(), micros());
  usageRenderedOk_ = 0xFFFFFFFF;
  showingFace_ = false;
  needRender_ = true;
  contentPrimed_ = false;
  layoutPrimed_ = false;
}

void UsageMode::invalidate(const Settings& s) {
  needRender_ = true;
  showingFace_ = false;
  usageRenderedOk_ = 0xFFFFFFFF;
  contentPrimed_ = false;
  layoutPrimed_ = false;
  usageInit(s);
  usageForceRefresh();
}

void UsageMode::service(const Settings& s) {
  // Pull mode: poll the daemon when a Usage URL is set. Push mode: leave it blank
  // and the daemon POSTs to /api/usage (for networks where the device can't reach
  // the PC). Either way usageGet() drives the render below.
  if (s.usage.usageUrl.length() >= 8) usageService(s);

  const UsageData& u = usageGet();

  // Considered stale after ~6 missed polls (plus a grace) — then show the face.
  //
  // Six, not two, because of what actually drives this boundary: the daemon
  // lives on a laptop, and a laptop sleeps. Every crossing costs a full-screen
  // clear in each direction, and at two missed polls a lid closed for a couple
  // of minutes was enough to make the panel flash twice. The counters above
  // measured 63 crossings in about ninety minutes against daemon log gaps of
  // 8, 7 and 18 minutes — the flicker was the boundary, not the animation.
  // Three minutes of silence at the default poll is a real absence rather than
  // a nap, and the other direction stays immediate: fresh numbers still appear
  // on the very next poll.
  uint32_t staleMs = (uint32_t)s.usage.pollSec * 1000UL * 6UL + USAGE_STALE_GRACE_MS;
  const bool wantFace = !usageFresh(staleMs);

  // Swapping bars for the face, or back, replaces everything on the screen —
  // the same visible clear a carousel switch makes, so it asks for the same
  // dip and paints nothing now: main.cpp wakes this mode at the bottom of the
  // dip and the repaint happens in the dark. A refused request means we are
  // already inside one, and then the paint below is exactly what it is waiting
  // for.
  if (wantFace != showingFace_ && appRequestDip()) return;

  if (!wantFace) {
    bool fullRepaint = !layoutPrimed_;
    if (showingFace_) { showingFace_ = false; needRender_ = true; fullRepaint = true; }
    if (u.lastOkMs != usageRenderedOk_) {
      usageRenderedOk_ = u.lastOkMs;
      if (contentChanged(u)) {
        if (u.valid != lastValid_) fullRepaint = true;   // layout itself changes shape
        rememberContent(u);
        needRender_ = true;
      }
    }
    if (needRender_) {
      drawUsage(u, fullRepaint);
      layoutPrimed_ = true;
      needRender_ = false;
    }
  } else {
    if (!showingFace_) {
      showingFace_ = true;
      usageRenderedOk_ = 0xFFFFFFFF;
      faceReset(millis(), micros());
      drawFace(/*restart=*/true);
    } else {
      faceSetPace(1, /*calm=*/false);   // the idle screen is the face's own
      if (faceTick(millis())) drawFace(/*restart=*/false);
    }
  }
}
