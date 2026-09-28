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

static uint16_t barColor(float pct) {
  if (pct >= 90) return C_RED;
  if (pct >= 75) return C_ACCENT;
  return C_UGREEN;
}

// One usage card: big %, a 5h/7d label, a fill bar coloured by load, and the
// reset countdown. `top` is the card's top y; the card is 82px tall.
static void drawMeter(Arduino_GFX* gfx, int top, const char* label,
                      float pct, int resetMins) {
  const int x = 8, w = 224, h = 82;
  gfx->fillRoundRect(x, top, w, h, 8, C_PANEL);

  char pc[8];
  snprintf(pc, sizeof(pc), "%d%%", (int)lroundf(constrain(pct, 0.0f, 100.0f)));
  uint8_t sz = gfxFitSize(pc, 150, 5);
  gfx->setTextSize(sz);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(x + 14, top + 10);
  gfx->print(pc);

  int lw = gfxTextW(label, 2);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(x + w - lw - 14, top + 12);
  gfx->print(label);

  int bx = x + 14, by = top + 52, bw = w - 28, bh = 12;
  gfx->fillRoundRect(bx, by, bw, bh, bh / 2, C_BARBG);
  int fw = (int)(bw * constrain(pct, 0.0f, 100.0f) / 100.0f);
  if (fw >= bh)     gfx->fillRoundRect(bx, by, fw, bh, bh / 2, barColor(pct));
  else if (fw > 0)  gfx->fillRect(bx, by, fw, bh, barColor(pct));

  char rs[16], line[28];
  fmtReset(resetMins, rs, sizeof(rs));
  snprintf(line, sizeof(line), "Resets in %s", rs);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(x + 14, top + 64);
  gfx->print(line);
}

// The face above the bars. A quarter scale puts the eye line at y=21 and the
// widest squint inside y=11..31, clear of the 5h meter at y=50; the box is
// centred, so it never reaches the status flag at x=228.
#define FACE_MINI_PCT 32
#define FACE_MINI_X   ((TFT_WIDTH - FACE_W * FACE_MINI_PCT / 100) / 2)
#define FACE_MINI_Y   2

// The same animation as the idle screen, just scaled — so the eyes go on
// blinking and squinting above the numbers instead of sitting there as two
// static bars.
static void drawMini(bool full) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  GfxFaceCanvas fc(gfx);
  faceSetViewport(FACE_MINI_X, FACE_MINI_Y, FACE_MINI_PCT);
  faceRender(fc, C_BLACK, C_ACCENT, full);
}

// Whole-screen repaints are the only thing on this screen that can read as a
// flash: everything else is painted by difference. Counting them makes "it
// flickers sometimes" answerable from /api/status instead of by watching the
// glass and hoping to catch one.
static uint16_t s_nBarsFull = 0;   // stats layout cleared and redrawn
static uint16_t s_nFaceFull = 0;   // idle face cleared and redrawn
static uint16_t s_nFlips    = 0;   // switches between the two screens
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

  if (fullRepaint) {
    s_nBarsFull++;
    s_facePrimed = false;   // force a full redraw next time the idle face shows
    gfx->fillScreen(C_BLACK);

    // Header: the live face, small and centred. Drawn before the meters so its
    // own clear cannot take the top of the 5h bar with it.
    drawMini(/*full=*/true);
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

  drawMeter(gfx, 50,  "5h", u.sessionPct, u.sessionResetMin);
  drawMeter(gfx, 138, "7d", u.weeklyPct,  u.weeklyResetMin);
}

// Idle screen: the Claude face on a full-screen terra-cotta field. `restart`
// repaints the whole field; otherwise only the band the eyes occupy is redrawn,
// which is what keeps the animation flicker-free on a panel with no framebuffer.
static void drawFace(bool restart) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  GfxFaceCanvas fc(gfx);
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

  // Considered stale after ~2 missed polls (plus a grace) — then show the face.
  uint32_t staleMs = (uint32_t)s.usage.pollSec * 1000UL * 2UL + USAGE_STALE_GRACE_MS;

  if (usageFresh(staleMs)) {
    bool fullRepaint = !layoutPrimed_;
    if (showingFace_) { showingFace_ = false; needRender_ = true; fullRepaint = true; s_nFlips++; }
    if (u.lastOkMs != usageRenderedOk_) {
      usageRenderedOk_ = u.lastOkMs;
      if (contentChanged(u)) {
        if (u.valid != lastValid_) fullRepaint = true;   // layout itself changes shape
        rememberContent(u);
        needRender_ = true;
      }
    }
    // The face runs on the stats screen too, so its clock has to advance here
    // as well as on the idle screen.
    const bool posed = faceTick(millis());
    if (needRender_) {
      drawUsage(u, fullRepaint);
      layoutPrimed_ = true;
      needRender_ = false;
    } else if (posed) {
      drawMini(/*full=*/false);
    }
  } else {
    if (!showingFace_) {
      showingFace_ = true;
      s_nFlips++;
      usageRenderedOk_ = 0xFFFFFFFF;
      faceReset(millis(), micros());
      drawFace(/*restart=*/true);
    } else if (faceTick(millis())) {
      drawFace(/*restart=*/false);
    }
  }
}
