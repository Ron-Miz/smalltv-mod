#include "Clock.h"
#include "Platform.h"
#include "config.h"
#include "WgClient.h"

static String            s_armedTz;          // last tzPosix armed (clockReapply re-arms only on change)
static String            s_armedNtp;         // last server pair armed, as "ntp1|ntp2"
static bool              s_ntpStarted = false; // SNTP has been started for a clock consumer
static volatile uint32_t s_lastSyncMs = 0;   // millis() of the last successful SNTP sync
static volatile bool     s_haveSync   = false;

// Night-mode state machine (driven by clockService, read by the getters).
static bool     s_nightLatched = false;      // confirmed this window -> stay dimmed till morning
static bool     s_nightActive  = false;      // effective "dim the screen now"
static bool     s_nightHeld    = false;      // in the window but waiting for a fresh NTP sync
static uint32_t s_lastResyncMs = 0;          // last forced re-arm while held (0 = none this window)

// Fired from the platform SNTP notification whenever NTP sets the clock. This is
// the "NTP was just reachable" signal that lets us trust the clock for night mode.
static void onNtpSync() {
  s_lastSyncMs = millis();
  s_haveSync   = true;
}

// Pure window predicate (minutes since local midnight). Handles a window that
// wraps midnight (start > end). An empty window (start == end) is never active.
//   start < end : [start, end)              e.g. 09:00..17:00
//   start > end : [start, 24h) ∪ [0, end)   e.g. 22:00..07:00
static bool nightWindowContains(int now, int start, int end) {
  if (start == end) return false;
  if (start < end)  return now >= start && now < end;
  return now >= start || now < end;
}

// SNTP keeps the POINTER it is handed, not a copy — lwIP's sntp_setservername
// stores the address and the resolver reads it on every query. A String's
// buffer moves when the String is reassigned, so handing over
// settings.clock.ntp1.c_str() would leave SNTP dereferencing freed memory the
// moment a settings save touched the field. These buffers never move.
static char s_ntp1[MAX_NTP_HOST_LEN + 1];
static char s_ntp2[MAX_NTP_HOST_LEN + 1];

// Copy the configured server in, falling back to the compiled-in default when
// the field is blank, so clearing the box in the web UI returns to the public
// pool rather than arming SNTP with a hostname it can never resolve.
static void armHost(char* dst, const String& v, const char* fallback) {
  const char* src = v.length() ? v.c_str() : fallback;
  strlcpy(dst, src, MAX_NTP_HOST_LEN + 1);
}

// Identity of the armed pair, so clockReapply can tell a real change from a save
// that merely re-sent the same values.
static String ntpFingerprint(const Settings& s) {
  return s.clock.ntp1 + "|" + s.clock.ntp2;
}

void clockBegin(const Settings& s) {
  platformOnTimeSync(onNtpSync);            // register before the first sync can land
  const char* tz = s.clock.tzPosix.length() ? s.clock.tzPosix.c_str() : "UTC0";
  armHost(s_ntp1, s.clock.ntp1, NTP_SERVER1);
  armHost(s_ntp2, s.clock.ntp2, NTP_SERVER2);
  platformTimeBegin(tz, s_ntp1, s_ntp2);
  s_armedTz  = s.clock.tzPosix;
  s_armedNtp = ntpFingerprint(s);
  s_ntpStarted = true;
}

void clockReapply(const Settings& s) {
  // SNTP only runs when something needs it. Starting the lwIP SNTP client is a
  // permanent mid-arena heap allocation, and on the memory-tight ESP8266 that can
  // fragment the largest contiguous block below what the cash.ch TLS handshake
  // needs (blanking those tickers). So arm on the first enable, re-arm on a
  // timezone change, and otherwise leave it alone. Night mode is one caller; a
  // WireGuard tunnel and theme clocks also need it. The peer rejects a handshake stamped
  // with a wrong clock (and that build is an ESP32, where the heap cost is moot).
  if (!s.clock.nightEnabled && !wgNeedsClock(s) && !(WITH_THEME && s.mode == MODE_THEME)) return;
  if (!s_ntpStarted || s.clock.tzPosix != s_armedTz ||
      ntpFingerprint(s) != s_armedNtp) clockBegin(s);
}

void clockForceResync(const Settings& s) {
  const char* tz = s.clock.tzPosix.length() ? s.clock.tzPosix.c_str() : "UTC0";
  armHost(s_ntp1, s.clock.ntp1, NTP_SERVER1);
  armHost(s_ntp2, s.clock.ntp2, NTP_SERVER2);
  platformTimeBegin(tz, s_ntp1, s_ntp2);   // re-arm -> fresh query
}

bool clockSynced() { return platformTimeValid(); }

bool clockTrusted() {
  // The clock is believed correct only if NTP set it within the trust window.
  return s_haveSync && platformTimeValid() &&
         (uint32_t)(millis() - s_lastSyncMs) <= NIGHT_NTP_TRUST_MS;
}

bool clockNow(struct tm& out) {
  if (!platformTimeValid()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &out);
  return true;
}

void clockService(const Settings& s) {
  if (!s.clock.nightEnabled) {
    s_nightLatched = s_nightActive = s_nightHeld = false;
    s_lastResyncMs = 0;
    return;
  }
  struct tm t;
  const bool haveClock = clockNow(t);

  // No clock at all: the boot sync never landed. Everything below needs a local
  // time to compare against the window, so without this the function returns at
  // the !inWindow branch and the re-arm further down is unreachable — a device
  // that misses its boot sync would never try again, and night mode would stay
  // dead until someone power-cycled it. Retry on the cold cadence instead, and
  // report held so /api/status says why night mode is not running.
  if (!haveClock) {
    s_nightLatched = s_nightActive = false;
    s_nightHeld    = true;
    if (s_lastResyncMs == 0 ||
        (uint32_t)(millis() - s_lastResyncMs) >= NIGHT_NTP_COLD_RETRY_MS) {
      s_lastResyncMs = millis() | 1;   // never store 0 (0 means "none yet")
      clockForceResync(s);
    }
    return;
  }

  bool inWindow = nightWindowContains(t.tm_hour * 60 + t.tm_min,
                                      (int)s.clock.nightStartMin,
                                      (int)s.clock.nightEndMin);
  if (!inWindow) {
    // Out of the window: reset so the next night re-checks trust from scratch.
    s_nightLatched = s_nightActive = s_nightHeld = false;
    s_lastResyncMs = 0;
    return;
  }
  if (s_nightLatched) {          // already confirmed this window -> stay dimmed till morning
    s_nightActive = true;
    s_nightHeld   = false;
    return;
  }
  if (clockTrusted()) {          // fresh, trusted clock -> switch on and latch for the window
    s_nightLatched = true;
    s_nightActive  = true;
    s_nightHeld    = false;
    return;
  }
  // In the window but the clock is not freshly confirmed: assume it may be wrong,
  // keep the screen on, and re-arm SNTP (immediately on entry, then every
  // NIGHT_NTP_RESYNC_MS) until a fresh sync lands or the window ends.
  s_nightActive = false;
  s_nightHeld   = true;
  if (s_lastResyncMs == 0 || (uint32_t)(millis() - s_lastResyncMs) >= NIGHT_NTP_RESYNC_MS) {
    s_lastResyncMs = millis() | 1;   // never store 0 (0 means "none this window")
    clockForceResync(s);
  }
}

bool clockNightActive() { return s_nightActive; }
bool clockNightHeld()   { return s_nightHeld; }

String clockTimeStr() {
  struct tm t;
  if (!clockNow(t)) return String();
  char b[60];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
  return String(b);
}
