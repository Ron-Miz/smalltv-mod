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

#define ROW_TOP    50
#define ROW_H      31
#define DOT_X      22
#define DOT_R       7
#define BLINK_MS  520

// Row: dot, then the name, then the state word hard against the right edge.
// The name gets what is left — 40..175 at size 2, which is 11 characters.
#define NAME_X          40
#define NAME_MAX_CHARS  11
#define PANEL_H         (ROW_H - 4)   // the row's filled panel
#define TEXT2_H         16            // the default font at size 2, in pixels

// The header is the title alone, centred, matching the usage screen.
#define HDR_TITLE_Y  10


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
  gfxDrawCentered("SESSIONS", HDR_TITLE_Y, 3, S_DIM);

  uint8_t n = 0;
  for (uint8_t i = 0; i < SESSION_MAX; i++) if (rows[i].used) n++;
  rows_ = n;

  if (!n) {
    gfxDrawCentered("no sessions", 112, 2, S_DIM);
    gfxDrawCentered("install the hooks", 142, 2, S_DIM);
    return;
  }

  uint8_t slot = 0;
  // Working first, then waiting, then idle: what needs you is at the top.
  for (uint8_t pass = 0; pass < 3; pass++) {
    const uint8_t want = pass == 0 ? SESSION_WORKING : pass == 1 ? SESSION_WAITING : SESSION_IDLE;
    for (uint8_t i = 0; i < SESSION_MAX && slot < SESSION_MAX; i++) {
      if (!rows[i].used || rows[i].state != want) continue;
      const int ry = ROW_TOP + slot * ROW_H - 2;   // panel top
      const int cy = ry + PANEL_H / 2;             // the line the row centres on
      gfx->fillRoundRect(6, ry, 228, PANEL_H, 6, S_PANEL);
      gfx->fillCircle(DOT_X, cy, DOT_R, stateColor(rows[i].state));

      // The name stays at size 2 and is clipped to the room it has, rather than
      // shrunk to size 1 to fit whole — a legible "clawdmeter-dae" beats an
      // unreadable "clawdmeter-daemon".
      const char* name = rows[i].label[0] ? rows[i].label : rows[i].id;
      char shown[NAME_MAX_CHARS + 1];
      strlcpy(shown, name, sizeof(shown));
      gfx->setTextSize(2);
      gfx->setTextColor(C_WHITE);
      gfx->setCursor(NAME_X, cy - TEXT2_H / 2);
      gfx->print(shown);

      const char* word = want == SESSION_WORKING ? "run"
                       : want == SESSION_WAITING ? "you" : "idle";
      gfx->setTextSize(2);
      gfx->setTextColor(stateColor(want));
      gfx->setCursor(230 - gfxTextW(word, 2), cy - TEXT2_H / 2);
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
        const int cy = ROW_TOP + slot * ROW_H - 2 + PANEL_H / 2;
        gfx->fillCircle(DOT_X, cy, DOT_R, blinkOn_ ? S_WORK : S_PANEL);
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
