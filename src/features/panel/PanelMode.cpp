#include "PanelMode.h"
#if WITH_PANEL

#include <Arduino_GFX_Library.h>
#include "Gfx.h"

PanelMode g_panelModes[PANEL_MAX];

void panelModesBind() {
  for (uint8_t i = 0; i < PANEL_MAX; i++) g_panelModes[i].bind(i);
}

// The same palette the usage and sessions screens use, so a panel looks like it
// belongs rather than like something bolted on.
#define P_DIM     gfxTint(0xB574)
#define P_PANEL   gfxTint(0x18E3)
#define P_ACCENT  gfxTint(0xDBAA)
#define P_GREEN   gfxTint(0x262B)
#define P_AMBER   gfxTint(0xF4E1)
#define P_RED     gfxTint(0xE908)
#define P_TRACK   gfxTint(0x2945)   // the unfilled part of a bar

#define HDR_TITLE_Y  10
#define ROW_TOP      50
#define ROW_H        31
#define PANEL_H      (ROW_H - 4)
#define TEXT2_H      16
#define DOT_X        22
#define DOT_R         7
#define LABEL_DOT_X  40    // label start on a dot row, clear of the dot
#define LABEL_BAR_X  14    // label start on a bar row, no dot to clear
#define RIGHT_EDGE  230
#define BAR_H         4

#define BODY_TOP  (ROW_TOP - 2)
#define BODY_H    (TFT_HEIGHT - BODY_TOP)

static inline int rowTop(uint8_t slot) { return ROW_TOP + slot * ROW_H - 2; }
static inline int rowMid(uint8_t slot) { return rowTop(slot) + PANEL_H / 2; }

// A bar with no colour of its own reads its colour off its own value, on the
// same thresholds as the usage meter: the point of a bar is how much room is
// left, and 60/85 is where that stops being comfortable.
static uint16_t autoBarColor(uint8_t pct) {
  if (pct >= 85) return P_RED;
  if (pct >= 60) return P_AMBER;
  return P_GREEN;
}

static uint16_t colorOf(const PanelRow& r) {
  switch (r.color) {
    case PANEL_C_GREEN:  return P_GREEN;
    case PANEL_C_AMBER:  return P_AMBER;
    case PANEL_C_RED:    return P_RED;
    case PANEL_C_DIM:    return P_DIM;
    case PANEL_C_ACCENT: return P_ACCENT;
    case PANEL_C_WHITE:  return C_WHITE;
    default:
      return r.kind == PANEL_ROW_BAR ? autoBarColor(r.bar) : P_DIM;
  }
}

static uint32_t rowSignature(const PanelRow& r) {
  uint32_t h = 2166136261u;
  for (const char* p = r.label; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  for (const char* p = r.value; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  h = (h ^ r.kind) * 16777619u;
  h = (h ^ r.bar) * 16777619u;
  h = (h ^ r.color) * 16777619u;
  return h ? h : 1;
}

static uint32_t panelSignature(const PanelData& p) {
  uint32_t h = 2166136261u;
  for (const char* c = p.title; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
  for (uint8_t i = 0; i < p.nRows; i++) h = (h ^ rowSignature(p.rows[i])) * 16777619u;
  h = (h ^ p.nRows) * 16777619u;
  return h ? h : 1;
}

const char* PanelMode::id() const {
  const PanelData* p = panelAt(slot_);
  if (!p) return "panel";
  strlcpy(idBuf_, p->id, sizeof(idBuf_));
  return idBuf_;
}

bool PanelMode::carouselReady(const Settings& s) const {
  (void)s;
  return panelAt(slot_) != nullptr;
}

// One row. The label is clipped to the room the value leaves it rather than
// shrunk, for the same reason the session names are: a cut-off word at a
// readable size beats a whole one that cannot be read across a desk.
void PanelMode::drawRow(uint8_t slot, const PanelRow& r) {
  Arduino_GFX* gfx = gfxDev();
  const int ry = rowTop(slot);
  const uint16_t col = colorOf(r);
  gfx->fillRoundRect(6, ry, 228, PANEL_H, 6, P_PANEL);

  const bool bar = (r.kind == PANEL_ROW_BAR);
  const int labelX = bar ? LABEL_BAR_X : LABEL_DOT_X;
  // A bar row stacks its text over its bar, so the text sits high in the panel;
  // a dot row has the whole panel to itself and centres.
  const int textY = bar ? (ry + 3) : (rowMid(slot) - TEXT2_H / 2);

  const int valueW = r.value[0] ? gfxTextW(r.value, 2) : 0;
  if (r.value[0]) {
    gfx->setTextSize(2);
    gfx->setTextColor(bar ? C_WHITE : col);
    gfx->setCursor(RIGHT_EDGE - valueW, textY);
    gfx->print(r.value);
  }

  if (!bar) gfx->fillCircle(DOT_X, rowMid(slot), DOT_R, col);

  // Whatever is left between the label's start and the value, less a gap.
  const int room = RIGHT_EDGE - valueW - 8 - labelX;
  const int fits = room > 0 ? room / (GFX_FONT_W * 2) : 0;
  char shown[PANEL_LABEL_LEN + 1];
  strlcpy(shown, r.label, sizeof(shown));
  if (fits >= 0 && fits < (int)strlen(shown)) shown[fits] = 0;
  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(labelX, textY);
  gfx->print(shown);

  if (bar) {
    const int bx = LABEL_BAR_X, by = ry + PANEL_H - BAR_H - 2, bw = 226 - bx;
    gfx->fillRoundRect(bx, by, bw, BAR_H, BAR_H / 2, P_TRACK);
    const int fw = bw * r.bar / 100;
    if (fw >= BAR_H)   gfx->fillRoundRect(bx, by, fw, BAR_H, BAR_H / 2, col);
    else if (fw > 0)   gfx->fillRect(bx, by, fw, BAR_H, col);
  }
}

// Painted by difference, like the sessions board: the only full clear is on
// arriving at the page, and after that a row that changed repaints over itself.
void PanelMode::repaint(const PanelData& p) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  if (needFull_) {
    gfx->fillScreen(C_BLACK);
    gfxDrawCentered(p.title, HDR_TITLE_Y, 3, P_DIM);
    for (uint8_t i = 0; i < PANEL_ROWS_MAX; i++) rowSig_[i] = 0;
    shownRows_ = 0;
    needFull_ = false;
  }

  for (uint8_t i = 0; i < p.nRows; i++) {
    const uint32_t sig = rowSignature(p.rows[i]);
    if (sig != rowSig_[i]) {
      drawRow(i, p.rows[i]);
      rowSig_[i] = sig;
    }
  }
  for (uint8_t i = p.nRows; i < shownRows_; i++) {   // rows the push dropped
    gfx->fillRect(0, rowTop(i), TFT_WIDTH, ROW_H, C_BLACK);
    rowSig_[i] = 0;
  }
  shownRows_ = p.nRows;
}

void PanelMode::begin(const Settings& s) {
  (void)s;
  drawn_ = 0;
  needFull_ = true;
}

void PanelMode::invalidate(const Settings& s) { begin(s); }
void PanelMode::wake(const Settings& s)       { begin(s); }

void PanelMode::service(const Settings& s) {
  const PanelData* p = panelAt(slot_);
  if (!p) {
    // Only reachable when this mode was picked by hand rather than by the
    // carousel, which skips a slot with nothing in it.
    if (needFull_) {
      Arduino_GFX* gfx = gfxDev();
      if (!gfx) return;
      gfx->fillScreen(C_BLACK);
      gfxDrawCentered("PANEL", HDR_TITLE_Y, 3, P_DIM);
      gfxDrawCentered("nothing pushed", 112, 2, P_DIM);
      gfxDrawCentered("POST /api/panel", 142, 2, P_DIM);
      needFull_ = false;
      drawn_ = 0;
    }
    return;
  }
  (void)s;
  const uint32_t sig = panelSignature(*p);
  if (needFull_ || sig != drawn_) {
    repaint(*p);
    drawn_ = sig;
  }
}

#endif  // WITH_PANEL
