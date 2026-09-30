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

// The band the rows and the empty-list message share, cleared as a unit when
// the screen changes shape.
#define BODY_TOP  (ROW_TOP - 2)
#define BODY_H    (TFT_HEIGHT - BODY_TOP)

static inline int rowTop(uint8_t slot) { return ROW_TOP + slot * ROW_H - 2; }
static inline int rowMid(uint8_t slot) { return rowTop(slot) + PANEL_H / 2; }

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

// The same hash for one row, so a screen of six sessions where one turned green
// repaints one row rather than all of them. Redrawing a row is cheap, but it
// blanks its own panel first, and doing that to five rows that did not change
// is exactly the "fast refresh" flicker this screen used to have.
static uint32_t rowSignature(const SessionRow& r) {
  uint32_t h = 2166136261u;
  for (const char* p = r.id; *p; p++)    h = (h ^ (uint8_t)*p) * 16777619u;
  for (const char* p = r.label; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  h = (h ^ r.state) * 16777619u;
  return h ? h : 1;
}

static uint16_t stateColor(uint8_t st) {
  return st == SESSION_WORKING ? S_WORK : st == SESSION_WAITING ? S_WAIT : S_DIM;
}

void SessionsMode::drawRow(uint8_t slot, const SessionRow& r) {
  Arduino_GFX* gfx = gfxDev();
  const int ry = rowTop(slot);          // panel top
  const int cy = rowMid(slot);          // the line the row centres on
  gfx->fillRoundRect(6, ry, 228, PANEL_H, 6, S_PANEL);
  gfx->fillCircle(DOT_X, cy, DOT_R, stateColor(r.state));

  // The name stays at size 2 and is clipped to the room it has, rather than
  // shrunk to size 1 to fit whole — a legible "clawdmeter-dae" beats an
  // unreadable "clawdmeter-daemon".
  const char* name = r.label[0] ? r.label : r.id;
  char shown[NAME_MAX_CHARS + 1];
  strlcpy(shown, name, sizeof(shown));
  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(NAME_X, cy - TEXT2_H / 2);
  gfx->print(shown);

  const char* word = r.state == SESSION_WORKING ? "run"
                   : r.state == SESSION_WAITING ? "you" : "idle";
  gfx->setTextSize(2);
  gfx->setTextColor(stateColor(r.state));
  gfx->setCursor(230 - gfxTextW(word, 2), cy - TEXT2_H / 2);
  gfx->print(word);
}

// Painted by difference. The only full clear is on entering the mode — from
// then on a row that changed repaints over itself, a row that went away is
// blanked, and everything else is left alone. Nothing here ever clears the
// screen to redraw the same thing.
void SessionsMode::repaint() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  sessionsExpire();
  const SessionRow* rows = sessionsAll();

  if (needFull_) {
    gfx->fillScreen(C_BLACK);
    gfxDrawCentered("SESSIONS", HDR_TITLE_Y, 3, S_DIM);
    for (uint8_t i = 0; i < SESSION_MAX; i++) slotSig_[i] = 0;
    shownRows_ = 0;
    shownEmpty_ = false;
    needFull_ = false;
  }

  // Order: working first, then waiting, then idle — what needs you is at the top.
  uint8_t order[SESSION_MAX];
  uint8_t n = 0;
  for (uint8_t pass = 0; pass < 3; pass++) {
    const uint8_t want = pass == 0 ? SESSION_WORKING : pass == 1 ? SESSION_WAITING : SESSION_IDLE;
    for (uint8_t i = 0; i < SESSION_MAX && n < SESSION_MAX; i++)
      if (rows[i].used && rows[i].state == want) order[n++] = i;
  }
  rows_ = n;

  if (!n) {
    if (!shownEmpty_) {
      gfx->fillRect(0, BODY_TOP, TFT_WIDTH, BODY_H, C_BLACK);
      gfxDrawCentered("no sessions", 112, 2, S_DIM);
      gfxDrawCentered("install the hooks", 142, 2, S_DIM);
      for (uint8_t i = 0; i < SESSION_MAX; i++) slotSig_[i] = 0;
      shownRows_ = 0;
      shownEmpty_ = true;
    }
    blinkMask_ = 0;
    return;
  }

  if (shownEmpty_) {                       // the message is where row 0 goes
    gfx->fillRect(0, BODY_TOP, TFT_WIDTH, BODY_H, C_BLACK);
    shownEmpty_ = false;
  }

  blinkMask_ = 0;
  for (uint8_t slot = 0; slot < n; slot++) {
    const SessionRow& r = rows[order[slot]];
    const uint32_t sig = rowSignature(r);
    if (sig != slotSig_[slot]) {
      drawRow(slot, r);
      slotSig_[slot] = sig;
    }
    if (r.state == SESSION_WORKING) blinkMask_ |= (uint8_t)(1u << slot);
  }
  for (uint8_t slot = n; slot < shownRows_; slot++) {   // rows that went away
    gfx->fillRect(0, rowTop(slot), TFT_WIDTH, ROW_H, C_BLACK);
    slotSig_[slot] = 0;
  }
  shownRows_ = n;
}

// Only the working dots blink, and only they are redrawn — one small circle
// each, so the rest of the screen is never touched.
void SessionsMode::blinkDots() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx || !blinkMask_) return;
  for (uint8_t slot = 0; slot < SESSION_MAX; slot++) {
    if (!(blinkMask_ & (1u << slot))) continue;
    gfx->fillCircle(DOT_X, rowMid(slot), DOT_R, blinkOn_ ? S_WORK : S_PANEL);
  }
}

bool SessionsMode::carouselReady(const Settings& s) const {
  (void)s;
  return sessionsCount() > 0;
}

void SessionsMode::begin(const Settings& s) {
  (void)s;
  primed_ = false;
  drawn_ = 0xFFFFFFFF;
  needFull_ = true;
  blinkMask_ = 0;
}

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
