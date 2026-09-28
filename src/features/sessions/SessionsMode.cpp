#include "SessionsMode.h"
#if WITH_SESSIONS

#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Sessions.h"

SessionsMode g_sessionsMode;

#define S_ACCENT  gfxTint(0xDBAA)   // terra-cotta, as on the usage screen
#define S_DIM     gfxTint(0xB574)
#define S_PANEL   gfxTint(0x18E3)
#define S_WORK    gfxTint(0xF9A6)   // red   — thinking or answering
#define S_WAIT    gfxTint(0x2E6C)   // green — finished, waiting on you

#define ROW_TOP    44
#define ROW_H      30
#define DOT_X      22
#define DOT_R       7
#define BLINK_MS  520

// Ids and states folded together. Only a change here is worth a repaint: the
// ages tick every second and redrawing for those would make the panel busy for
// nothing.
static uint32_t signature(const SessionRow* rows, uint8_t* count) {
  uint32_t h = 2166136261u;
  uint8_t n = 0;
  for (uint8_t i = 0; i < SESSION_MAX; i++) {
    if (!rows[i].used) continue;
    n++;
    for (const char* p = rows[i].id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ rows[i].state) * 16777619u;
  }
  *count = n;
  return h ? h : 1;
}

static uint16_t stateColor(uint8_t st) {
  return st == SESSION_WORKING ? S_WORK : st == SESSION_WAITING ? S_WAIT : S_DIM;
}

void SessionsMode::repaint() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  sessionsExpire();
  const SessionRow* rows = sessionsAll();

  gfx->fillScreen(C_BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(S_ACCENT);
  gfx->setCursor(10, 12);
  gfx->print("SESSIONS");

  uint8_t n = 0;
  for (uint8_t i = 0; i < SESSION_MAX; i++) if (rows[i].used) n++;
  rows_ = n;

  if (!n) {
    gfxDrawCentered("no sessions", 112, 2, S_DIM);
    gfxDrawCentered("install the hooks", 140, 1, S_DIM);
    return;
  }

  uint8_t slot = 0;
  // Working first, then waiting, then idle: what needs you is at the top.
  for (uint8_t pass = 0; pass < 3; pass++) {
    const uint8_t want = pass == 0 ? SESSION_WORKING : pass == 1 ? SESSION_WAITING : SESSION_IDLE;
    for (uint8_t i = 0; i < SESSION_MAX && slot < SESSION_MAX; i++) {
      if (!rows[i].used || rows[i].state != want) continue;
      const int y = ROW_TOP + slot * ROW_H;
      gfx->fillRoundRect(6, y - 2, 228, ROW_H - 4, 6, S_PANEL);
      gfx->fillCircle(DOT_X, y + 11, DOT_R, stateColor(rows[i].state));

      const char* name = rows[i].label[0] ? rows[i].label : rows[i].id;
      gfx->setTextSize(gfxFitSize(name, 150, 2));
      gfx->setTextColor(C_WHITE);
      gfx->setCursor(40, y + 5);
      gfx->print(name);

      const char* word = want == SESSION_WORKING ? ".." : want == SESSION_WAITING ? "ok" : "--";
      gfx->setTextSize(1);
      gfx->setTextColor(stateColor(want));
      gfx->setCursor(228 - gfxTextW(word, 1), y + 9);
      gfx->print(word);
      slot++;
    }
  }
}

// Only the working dots blink, and only they are redrawn — one small circle
// each, so the rest of the screen is never touched.
void SessionsMode::blinkDots() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const SessionRow* rows = sessionsAll();
  uint8_t slot = 0;
  for (uint8_t pass = 0; pass < 3; pass++) {
    const uint8_t want = pass == 0 ? SESSION_WORKING : pass == 1 ? SESSION_WAITING : SESSION_IDLE;
    for (uint8_t i = 0; i < SESSION_MAX && slot < SESSION_MAX; i++) {
      if (!rows[i].used || rows[i].state != want) continue;
      if (want == SESSION_WORKING) {
        const int y = ROW_TOP + slot * ROW_H;
        gfx->fillCircle(DOT_X, y + 11, DOT_R, blinkOn_ ? S_WORK : S_PANEL);
      }
      slot++;
    }
  }
}

void SessionsMode::begin(const Settings& s) { (void)s; primed_ = false; drawn_ = 0xFFFFFFFF; }

void SessionsMode::invalidate(const Settings& s) { begin(s); }

void SessionsMode::wake(const Settings& s) { begin(s); }

void SessionsMode::service(const Settings& s) {
  (void)s;
  sessionsExpire();
  uint8_t n = 0;
  const uint32_t sig = signature(sessionsAll(), &n);

  if (!primed_ || sig != drawn_) {
    repaint();
    drawn_ = sig;
    primed_ = true;
    blinkOn_ = true;
    lastBlinkMs_ = millis();
    return;
  }

  const uint32_t now = millis();
  if ((uint32_t)(now - lastBlinkMs_) >= BLINK_MS) {
    lastBlinkMs_ = now;
    blinkOn_ = !blinkOn_;
    blinkDots();
  }
}

#endif  // WITH_SESSIONS
