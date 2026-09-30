#include "Panel.h"
#if WITH_PANEL

#include <ArduinoJson.h>
#include <string.h>

static PanelData s_panels[PANEL_MAX];

static PanelData* find(const char* id) {
  for (uint8_t i = 0; i < PANEL_MAX; i++)
    if (s_panels[i].used && strncmp(s_panels[i].id, id, PANEL_ID_LEN) == 0) return &s_panels[i];
  return nullptr;
}

// The panel with this id, else a free slot, else the one refreshed longest ago.
// Evicting the stalest keeps the panels something is actively pushing.
static PanelData* slotFor(const char* id) {
  if (PanelData* p = find(id)) return p;
  PanelData* oldest = &s_panels[0];
  for (uint8_t i = 0; i < PANEL_MAX; i++) {
    if (!s_panels[i].used) return &s_panels[i];
    if ((int32_t)(s_panels[i].seenMs - oldest->seenMs) < 0) oldest = &s_panels[i];
  }
  return oldest;
}

static uint8_t colorFromName(const char* c) {
  if (!c || !c[0])                 return PANEL_C_AUTO;
  if (!strcmp(c, "green"))         return PANEL_C_GREEN;
  if (!strcmp(c, "amber"))         return PANEL_C_AMBER;
  if (!strcmp(c, "yellow"))        return PANEL_C_AMBER;
  if (!strcmp(c, "red"))           return PANEL_C_RED;
  if (!strcmp(c, "dim"))           return PANEL_C_DIM;
  if (!strcmp(c, "grey"))          return PANEL_C_DIM;
  if (!strcmp(c, "gray"))          return PANEL_C_DIM;
  if (!strcmp(c, "accent"))        return PANEL_C_ACCENT;
  if (!strcmp(c, "white"))         return PANEL_C_WHITE;
  return PANEL_C_AUTO;
}

bool panelApply(const String& body, String* err) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) { if (err) *err = "bad json"; return false; }

  const char* id = doc["id"] | "";
  if (!id[0]) { if (err) *err = "no id"; return false; }

  if (doc["drop"].is<bool>() && doc["drop"].as<bool>()) { panelDrop(id); return true; }

  JsonArrayConst rows = doc["rows"].as<JsonArrayConst>();
  if (rows.isNull()) { if (err) *err = "no rows"; return false; }

  PanelData* p = slotFor(id);
  memset(p, 0, sizeof(*p));
  strlcpy(p->id, id, sizeof(p->id));
  strlcpy(p->title, doc["title"] | id, sizeof(p->title));

  uint32_t ttl = doc["ttlSec"] | (uint32_t)PANEL_TTL_DEF_SEC;
  if (ttl < PANEL_TTL_MIN_SEC) ttl = PANEL_TTL_MIN_SEC;
  if (ttl > PANEL_TTL_MAX_SEC) ttl = PANEL_TTL_MAX_SEC;
  p->ttlMs = ttl * 1000UL;

  for (JsonObjectConst r : rows) {
    if (p->nRows >= PANEL_ROWS_MAX) break;
    PanelRow& row = p->rows[p->nRows];
    strlcpy(row.label, r["label"] | "", sizeof(row.label));
    strlcpy(row.value, r["value"] | "", sizeof(row.value));
    // A row carrying a bar is a proportion; anything else is a state light.
    if (r["bar"].is<int>() || r["bar"].is<float>()) {
      int b = r["bar"].as<int>();
      row.kind = PANEL_ROW_BAR;
      row.bar  = (uint8_t)(b < 0 ? 0 : b > 100 ? 100 : b);
    } else {
      row.kind = PANEL_ROW_DOT;
    }
    // "color" names the row's colour; "dot" is accepted as a shorthand for it,
    // so {"dot":"green"} reads the way a pusher expects it to.
    const char* c = r["color"] | "";
    if (!c[0]) c = r["dot"] | "";
    row.color = colorFromName(c);
    p->nRows++;
  }
  if (!p->nRows) { memset(p, 0, sizeof(*p)); if (err) *err = "empty rows"; return false; }

  p->seenMs = millis() | 1;   // never 0: 0 reads as "never pushed"
  p->used   = true;
  return true;
}

void panelDrop(const char* id) {
  if (!id || !id[0]) return;
  if (PanelData* p = find(id)) memset(p, 0, sizeof(*p));
}

void panelClear() { memset(s_panels, 0, sizeof(s_panels)); }

const PanelData* panelAll() { return s_panels; }

void panelExpire() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < PANEL_MAX; i++) {
    if (!s_panels[i].used) continue;
    if ((uint32_t)(now - s_panels[i].seenMs) >= s_panels[i].ttlMs)
      memset(&s_panels[i], 0, sizeof(s_panels[i]));
  }
}

uint8_t panelCount() {
  panelExpire();
  uint8_t n = 0;
  for (uint8_t i = 0; i < PANEL_MAX; i++) if (s_panels[i].used) n++;
  return n;
}

// The nth live panel, in table order — which is the order pushes claimed slots,
// so a panel keeps its place in the carousel as long as it keeps being pushed.
const PanelData* panelAt(uint8_t slot) {
  for (uint8_t i = 0; i < PANEL_MAX; i++) {
    if (!s_panels[i].used) continue;
    if (slot == 0) return &s_panels[i];
    slot--;
  }
  return nullptr;
}

#endif  // WITH_PANEL
