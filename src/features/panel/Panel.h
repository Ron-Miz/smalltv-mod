// Panel — a page whose entire content is pushed from the PC.
//
// The device cannot work out how full a session's context is, or which session
// is blocked on a permission prompt. Something on the PC can, and every one of
// those screens used to mean a firmware build, a CI wait and a flash. A panel
// is that screen expressed as data instead: a title and up to six rows, each a
// dot or a bar with a label and a value, pushed to /api/panel.
//
// Deliberately not a renderer per idea. Two row shapes cover what is worth
// glancing at across the room, and anything that needs more than that has
// earned its own mode.
#pragma once
#include <Arduino.h>
#include "config.h"

#if WITH_PANEL

// Row colours are named, so a pusher never sends RGB565. A bar left uncoloured
// takes its colour from its own value, matching the usage meter: plenty of room
// green, most of it spent amber, nearly full red.
enum PanelColor : uint8_t {
  PANEL_C_AUTO = 0,
  PANEL_C_GREEN,
  PANEL_C_AMBER,
  PANEL_C_RED,
  PANEL_C_DIM,
  PANEL_C_ACCENT,
  PANEL_C_WHITE,
};

enum PanelRowKind : uint8_t {
  PANEL_ROW_DOT = 0,   // a state light: label, dot, value
  PANEL_ROW_BAR = 1,   // a proportion: label, value, and a bar under them
};

struct PanelRow {
  char    label[PANEL_LABEL_LEN + 1];
  char    value[PANEL_VALUE_LEN + 1];
  uint8_t kind;
  uint8_t bar;     // 0..100, meaningful when kind == PANEL_ROW_BAR
  uint8_t color;   // PanelColor
};

struct PanelData {
  char     id[PANEL_ID_LEN + 1];
  char     title[PANEL_TITLE_LEN + 1];
  PanelRow rows[PANEL_ROWS_MAX];
  uint8_t  nRows;
  uint32_t seenMs;   // millis() of the last push (never 0 while used)
  uint32_t ttlMs;    // how long this panel stays in the carousel unrefreshed
  bool     used;
};

// Apply a pushed panel. `body` is the JSON from POST /api/panel; err gets a
// short reason on failure. A push replaces the panel with the same id rather
// than adding one, so a script can run on a timer without filling the table.
bool panelApply(const String& body, String* err);

// Forget one panel (POST with {"drop":true}), or all of them.
void panelDrop(const char* id);
void panelClear();

// The fixed table, PANEL_MAX entries, `used` marking the live ones.
const PanelData* panelAll();

// Drop panels nobody has refreshed within their own ttl.
void panelExpire();

// How many are live, and the nth live one (nullptr when there is no such one).
uint8_t          panelCount();
const PanelData* panelAt(uint8_t slot);

#endif  // WITH_PANEL
