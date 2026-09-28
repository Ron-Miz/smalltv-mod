#include "Sessions.h"
#if WITH_SESSIONS

#include <string.h>

// A fixed table rather than a growing list: six rows is more sessions than
// anyone watches at once, and a fixed table cannot fragment the ESP8266's
// shared static/heap arena no matter how often the hooks fire.
static SessionRow s_rows[SESSION_MAX];

static SessionRow* find(const char* id) {
  for (uint8_t i = 0; i < SESSION_MAX; i++)
    if (s_rows[i].used && strncmp(s_rows[i].id, id, SESSION_ID_LEN) == 0) return &s_rows[i];
  return nullptr;
}

// A free slot, else the row nobody has reported for longest. Evicting the
// stalest keeps the live sessions visible when a seventh turns up.
static SessionRow* slotFor(const char* id) {
  if (SessionRow* r = find(id)) return r;
  SessionRow* oldest = &s_rows[0];
  for (uint8_t i = 0; i < SESSION_MAX; i++) {
    if (!s_rows[i].used) return &s_rows[i];
    if ((int32_t)(s_rows[i].seenMs - oldest->seenMs) < 0) oldest = &s_rows[i];
  }
  return oldest;
}

bool sessionsTouch(const char* id, const char* label, uint8_t state) {
  if (!id || !id[0]) return false;
  if (state > SESSION_WAITING) state = SESSION_IDLE;
  SessionRow* r = slotFor(id);
  strlcpy(r->id, id, sizeof(r->id));
  // A blank label leaves whatever the session was already called, so a hook
  // that omits it cannot wipe the name a previous one set.
  if (label && label[0]) strlcpy(r->label, label, sizeof(r->label));
  else if (!r->used) r->label[0] = 0;
  r->state  = state;
  r->seenMs = millis() | 1;     // never 0: 0 reads as "never seen"
  r->used   = true;
  return true;
}

void sessionsDrop(const char* id) {
  if (!id || !id[0]) return;
  if (SessionRow* r = find(id)) memset(r, 0, sizeof(*r));
}

void sessionsClear() { memset(s_rows, 0, sizeof(s_rows)); }

const SessionRow* sessionsAll() { return s_rows; }

static uint32_t lifetimeFor(uint8_t state) {
  return state == SESSION_WORKING ? SESSION_STALE_WORKING_MS
       : state == SESSION_WAITING ? SESSION_STALE_WAITING_MS
                                  : SESSION_STALE_IDLE_MS;
}

void sessionsExpire() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < SESSION_MAX; i++) {
    if (!s_rows[i].used) continue;
    if ((uint32_t)(now - s_rows[i].seenMs) >= lifetimeFor(s_rows[i].state))
      memset(&s_rows[i], 0, sizeof(s_rows[i]));
  }
}

uint8_t sessionsCount() {
  sessionsExpire();
  uint8_t n = 0;
  for (uint8_t i = 0; i < SESSION_MAX; i++) if (s_rows[i].used) n++;
  return n;
}

#endif  // WITH_SESSIONS
