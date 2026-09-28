// smalltv-mod — custom firmware for the GeekMagic SmallTV (ESP-12F / ESP8266)
//
// Three features, each a self-contained DisplayMode (see Mode.h), picked in the
// web UI and dispatched from the registry below:
//   - Ticker (features/ticker):  stock/crypto price, % change, sparkline.
//   - Usage  (features/usage):   Claude 5h/7d usage bars + animated idle face.
//   - Radar  (features/radar):   live ADS-B plane radar (compiled in when WITH_RADAR).
// Shared plumbing (WiFi, web UI, OTA, display core, settings) lives at src root.
//
// License: WTFPL
#include <Arduino.h>
#include "Platform.h"
#include "config.h"
#include "Settings.h"
#include "Net.h"
#include "Gfx.h"
#include "WebPortal.h"
#include "OtaUpdate.h"
#include "Mode.h"
#include "Clock.h"
#include "WgClient.h"
#if WITH_THEME
#include "features/theme/ThemeMode.h"
#endif
#if WITH_NOTIFY
#include "NotifyMode.h"
#endif

#if WITH_TICKER
#include "TickerMode.h"
#endif
#if WITH_USAGE
#include "UsageMode.h"
#endif
#if WITH_RADAR
#include "RadarMode.h"
#endif
#if WITH_HA
#include "HaMode.h"
#include "MqttClient.h"
#endif
#if WITH_SESSIONS
#include "features/sessions/SessionsMode.h"
#include "features/sessions/Sessions.h"
#endif

// ---- mode registry --------------------------------------------------------
// The compiled-in features, in display order. main.cpp holds no per-feature
// state of its own — each mode owns its fetch/render/dirty tracking.
static DisplayMode* kModes[] = {
#if WITH_TICKER
  &g_tickerMode,
#endif
#if WITH_USAGE
  &g_usageMode,
#endif
#if WITH_RADAR
  &g_radarMode,
#endif
#if WITH_HA
  &g_haMode,
#endif
#if WITH_SESSIONS
  &g_sessionsMode,
#endif
#if WITH_THEME
  &g_themeMode,
#endif
};
static const size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

// ---- carousel -------------------------------------------------------------
// MODE_CAROUSEL rotates through the ticked features. Switches call wake() on
// the incoming mode: repaint from cached data, no refetch.
static size_t   g_carIdx = 0;
static uint32_t g_carSwitch = 0;

static bool carouselHas(const Settings& s, const DisplayMode* m) {
  switch (m->modeConst()) {
    case MODE_STOCKS: return s.carouselTicker;
#if WITH_USAGE
    // Ticked *and* holding numbers. With the daemon quiet the usage screen is
    // the idle face, and a rotation that stops on a screensaver for fifteen
    // seconds before moving on reads as a third tab that nobody asked for.
    case MODE_USAGE:  return s.carouselUsage && usageHasNumbers(s);
#else
    case MODE_USAGE:  return s.carouselUsage;
#endif
#if WITH_SESSIONS
    // Ticked *and* non-empty: a carousel stop on "no sessions" is dead air.
    case MODE_SESSIONS: return s.carouselSessions && sessionsCount() > 0;
#endif
    case MODE_RADAR:  return s.carouselRadar;
#if WITH_HA
    case MODE_HA:     return s.carouselHa;
#endif
    case MODE_THEME:  return false; // explicitly selected, not in the existing carousel
    default:          return true;
  }
}

// Where to park when nothing ticked has anything to show: the feature that owns
// the idle screen, rather than whichever empty one we happened to be on. An
// idle face is a fair thing to look at; "no sessions" for an hour is not.
static size_t carouselIdle() {
  for (size_t i = 0; i < kModeCount; i++)
    if (kModes[i]->modeConst() == MODE_USAGE) return i;
  return 0;
}

// The next ticked mode after the current one, or the idle screen if nothing
// else has anything to show.
static size_t carouselPick(const Settings& s) {
  for (size_t hop = 1; hop <= kModeCount; hop++) {
    size_t cand = (g_carIdx + hop) % kModeCount;
    if (carouselHas(s, kModes[cand])) return cand;
  }
  return carouselIdle();
}

// ---- mode transition ------------------------------------------------------
// A switch repaints the whole panel, and with no framebuffer that repaint is
// visible: the old screen is cleared to black, then the new one draws itself in
// over a few tens of milliseconds. Dipping the backlight around it turns that
// flash into a deliberate cross-fade through black — the only kind of
// transition this panel can afford, since the 115 KB a slide would need does
// not exist on the part.
//
// One owner. The first cut let a feature run its own blocking dip as well, and
// two owners of one backlight is how a panel ends up dark for a whole carousel
// dwell: whoever ramps last wins, and if the wrong one wins there is nothing to
// put the light back. A mode that is about to replace everything on screen now
// asks for the dip instead (appRequestDip) and paints at the bottom of it, and
// two invariants below make a stranded backlight impossible — outside a dip the
// light is forced to full every tick, and a dip that somehow overruns is ended.
//
// Runs from loop() rather than blocking, so the web UI stays answerable through
// the dip and nothing stalls the fetches.
#define TRANS_OUT_MS 130
#define TRANS_IN_MS  220
#define TRANS_MAX_MS 3000    // a dip may never outlast this, whatever happens

enum { TR_IDLE = 0, TR_OUT = 1, TR_IN = 2 };
static uint8_t  g_trPhase  = TR_IDLE;
static uint32_t g_trStart  = 0;
static uint8_t  g_trFrom   = 100;    // level the dip started from
static size_t   g_trTarget = 0;      // carousel slot to land on
static bool     g_trSwitch = false;  // switching modes, or repainting this one

// Which mode the settings point at right now — no side effects, so it is safe
// to call from inside the transition.
static DisplayMode* resolveMode(const Settings& s) {
  if (s.mode == MODE_CAROUSEL && kModeCount > 0) return kModes[g_carIdx];
  for (size_t i = 0; i < kModeCount; i++)
    if (kModes[i]->modeConst() == s.mode) return kModes[i];
  return kModeCount ? kModes[0] : nullptr;   // fall back to the first compiled mode
}

static void transitionBegin(bool modeSwitch, size_t target) {
  g_trSwitch = modeSwitch;
  g_trTarget = target;
  g_trFrom   = gfxFadeLevel();
  g_trPhase  = TR_OUT;
  g_trStart  = millis();
}

// Asked for by a mode that is about to replace the whole screen (the usage
// meter swapping its bars for the idle face, say), so that repaint gets the
// same dip a carousel switch gets. False means a dip is already in flight —
// the caller is being serviced from inside one and should just paint.
bool appRequestDip() {
  if (g_trPhase != TR_IDLE) return false;
  transitionBegin(false, g_carIdx);
  return true;
}

static DisplayMode* activeMode(const Settings& s) {
  if (s.mode == MODE_CAROUSEL && kModeCount > 0) {
    if (g_carSwitch == 0) g_carSwitch = millis();
    if (g_trPhase == TR_IDLE) {
      size_t want = g_carIdx;
      if (!carouselHas(s, kModes[g_carIdx])) {
        want = carouselPick(s);                  // settings changed under us
      } else if (millis() - g_carSwitch >= (uint32_t)s.carouselSec * 1000UL) {
        g_carSwitch = millis();
        want = carouselPick(s);
      }
      if (want != g_carIdx) transitionBegin(true, want);
    }
  }
  return resolveMode(s);
}

// Advance a dip in flight. Returns true while the caller should skip the normal
// service() — the outgoing screen is frozen, and at the bottom the incoming
// mode paints itself once, unseen.
static bool transitionService(const Settings& s) {
  if (g_trPhase == TR_IDLE) {
    // The fade belongs to a dip. With none in flight the panel is at full,
    // whatever happened on the way here.
    if (gfxFadeLevel() != 100) gfxSetFade(100);
    return false;
  }

  const uint32_t el = millis() - g_trStart;
  if (el > TRANS_MAX_MS) {          // nothing may hold the glass dark
    gfxSetFade(100);
    g_trPhase = TR_IDLE;
    return false;
  }

  if (g_trPhase == TR_OUT) {
    if (el < TRANS_OUT_MS) {
      gfxSetFade((uint8_t)(g_trFrom - (uint32_t)g_trFrom * el / TRANS_OUT_MS));
      return true;
    }
    gfxSetFade(0);
    if (g_trSwitch) {
      g_carIdx    = g_trTarget;
      g_carSwitch = millis();       // the new feature gets its full dwell
    }
    DisplayMode* in = resolveMode(s);
    if (in) {
      in->wake(s);
      in->service(s);               // the repaint nobody sees
    }
    g_trPhase = TR_IN;
    g_trStart = millis();
    return true;
  }

  // TR_IN: back up to full, and let the mode service normally underneath.
  if (el >= TRANS_IN_MS) {
    gfxSetFade(100);
    g_trPhase = TR_IDLE;
  } else {
    gfxSetFade((uint8_t)(100UL * el / TRANS_IN_MS));
  }
  return false;
}

static Settings g_settings;
static String   g_resetReason;        // why the chip last reset (diagnostics)
static bool     g_safeMode = false;   // last reset was an exception -> don't re-enter the crash
static char     g_epcStr[16] = "";
static char     g_addrStr[16] = "";
static int g_lastBr = -1;        // last effective brightness written (-1 = none yet)
#if HAS_LDR
static uint32_t g_lastAutoBr = 0;
static uint8_t  g_ldrCache   = DEFAULT_BRIGHTNESS;   // last LDR reading (2 s cadence)
#endif

// Single brightness resolver: night mode overrides auto-brightness overrides the
// manual level. Only writes the PWM when the effective target changes.
static uint8_t appEffectiveBrightness() {
  if (clockNightActive()) return g_settings.clock.nightLevel;
#if HAS_LDR
  if (g_settings.autoBrightness) {
    if (millis() - g_lastAutoBr > 2000) {
      g_lastAutoBr = millis();
      int raw = analogRead(LDR_PIN);
      g_ldrCache = (uint8_t)constrain(raw * 100 / ADC_MAX, 5, 100);
    }
    return g_ldrCache;
  }
#endif
  return g_settings.brightness;
}

void appApplyBrightness() {
  uint8_t t = appEffectiveBrightness();
  if ((int)t != g_lastBr) {
    g_lastBr = t;
    gfxSetBrightness(t, g_settings.backlightInverted);
  }
}

// Exposed to the web portal (/api/status) so the last reset reason is visible.
const char* appResetReason() { return g_resetReason.c_str(); }

// Also for /api/status: what is actually on the glass, and how lit it is. A
// screen that looks blank is either a mode that painted nothing or a backlight
// that never came back, and these two tell those apart without guesswork.
const char* appScreenId() {
  DisplayMode* m = resolveMode(g_settings);
  return m ? m->id() : "-";
}
uint8_t appScreenFade() { return gfxFadeLevel(); }

// Called by the web portal after settings are applied: re-init every mode and
// force a fresh repaint so a mode/URL/symbol change takes effect immediately.
void appInvalidate() {
  for (size_t i = 0; i < kModeCount; i++) kModes[i]->invalidate(g_settings);
}

static void bootProgress(const char* msg) {
  gfxBoot("SmallTV", msg);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(FW_NAME " " FW_VERSION);

  // Capture why we (re)booted. On a reboot loop this is the key clue, and the
  // device's UART isn't exposed — so we also show it on screen below. On the
  // ESP8266 we also keep the crash PC (epc1) for addr2line decoding; the
  // ESP32-C2 (RISC-V) doesn't expose it, so epc/addr come back empty there.
  PlatformReset pr = platformResetInfo();
  Serial.print("[boot] reset reason: ");
  Serial.println(pr.reason);

  if (pr.wasCrash) {
    g_safeMode = true;                   // crashed last boot -> stay out of the crash path
    strlcpy(g_epcStr,  pr.epc,  sizeof(g_epcStr));
    strlcpy(g_addrStr, pr.addr, sizeof(g_addrStr));
    char rich[80];
    snprintf(rich, sizeof(rich), "%s epc %s addr %s", pr.reason.c_str(),
             g_epcStr[0] ? g_epcStr : "-", g_addrStr[0] ? g_addrStr : "-");
    g_resetReason = rich;
  } else {
    g_resetReason = pr.reason;
  }

  Serial.println("[boot] settings");
  settingsBegin();
  loadSettings(g_settings);

  Serial.println("[boot] display");
  gfxBegin(g_settings);
  gfxBoot(g_safeMode ? "Crashed" : "SmallTV", FW_VERSION);

  Serial.println("[boot] net");
  netBegin(g_settings, bootProgress);
  // Arm SNTP now that WiFi (STA) is up — but only if night mode is enabled, so a
  // ticker-only device doesn't pay the SNTP heap cost (which can starve the cash.ch
  // TLS handshake on the ESP8266). clockReapply arms it iff needed. Skipped after a
  // crash so a fault in here can't boot-loop before the web server starts (the
  // device then comes up in safe mode, OTA-recoverable, instead of needing UART).
  // ...unless a WireGuard tunnel is configured, which needs the clock and only
  // exists on an ESP32 where the heap argument for the skip does not apply.
  if (!g_safeMode || wgNeedsClock(g_settings)) clockReapply(g_settings);

  // Optional WireGuard tunnel (ESP32 targets). Arms the state machine only;
  // the bring-up itself runs from loop(), so nothing here can delay the web
  // server. A crash last boot feeds the three-strikes hold that keeps a bad
  // tunnel config from locking the device out of its own web UI.
  wgBegin(g_settings, g_safeMode);

  // A GitHub update queued from the web UI runs now, before the features claim
  // the heap (the download needs a 16 KB TLS buffer that only fits at boot).
  // On success it reboots into the new image; a no-op stub on the ESP32 targets.
  if (otaBootRequested()) {
    Serial.println("[boot] github update");
    gfxBoot("SmallTV", "updating...");
    otaBootUpdate(g_settings);
    gfxBoot("SmallTV", "update failed");   // still here -> failed; details in the web UI
    delay(1200);
  }

  Serial.println("[boot] web");
  webPortalBegin(g_settings);

#if WITH_HA
  mqttBegin(g_settings);   // arms the client; the connect itself runs from loop()
#endif

  Serial.println("[boot] modes");
  for (size_t i = 0; i < kModeCount; i++) kModes[i]->begin(g_settings);
  Serial.println("[boot] done");

  if (netMode() == NET_AP) {
    gfxApInfo(g_settings.apSsid.c_str(), g_settings.apPass.c_str(), netIP().c_str());
  } else if (g_safeMode) {
    // Last boot crashed: show the crash address (persistent) and keep the web
    // server up for OTA recovery — don't enter the render path that crashed.
    gfxCrash(g_epcStr, g_addrStr, netIP().c_str());
  } else {
    // Show which network we joined and how to reach the web UI, long enough to read.
    gfxStaInfo(netSSID().c_str(), netIP().c_str(), g_settings.hostname.c_str());
    delay(3500);
  }
}

void loop() {
  netLoop();
  webPortalLoop();

  if (webPortalRebootDue()) {
    delay(120);
    ESP.restart();
  }

  // Before the safe-mode return on purpose: if the crash had nothing to do with
  // the tunnel, remote access survives it, and if it did, the three-strikes hold
  // stops the retries by itself.
  wgService(g_settings);

  if (g_safeMode) {
    delay(5);
    return;  // crashed last boot: web UI stays up for OTA recovery, no rendering
  }

#if WITH_HA
  // After the safe-mode return on purpose: a fault in here (e.g. a malformed
  // retained screen that re-arrives on every connect) must not boot-loop the
  // device past its own recovery page. Self-gates on WiFi STA + broker config.
  mqttLoop();
#endif

  if (netMode() == NET_AP) {
    delay(5);
    return;  // setup mode: AP info stays on screen
  }

  // --- STA mode: the active feature fetches + renders itself ---

  // Night-mode state machine (NTP-trust gate), then apply the effective brightness
  // (night override / auto-brightness / manual level).
  clockService(g_settings);
  appApplyBrightness();

  // On expiry the carousel dwell is credited back the time it was hidden, so it
  // resumes on the same feature with the same remaining slice. heldMs() spans
  // the whole run rather than the last request in it, so a queue that chained
  // four overlays credits back all four.
#if WITH_NOTIFY
  static bool wasNotifying = false;
  if (g_notifyMode.active()) {
    wasNotifying = true;
    g_notifyMode.service(g_settings);
    delay(5);
    return;
  }
  bool restore = wasNotifying;
  if (wasNotifying) {
    wasNotifying = false;
    if (g_carSwitch) g_carSwitch += g_notifyMode.heldMs();
  }
#else
  const bool restore = false;
#endif

  DisplayMode* m = activeMode(g_settings);
  if (transitionService(g_settings)) {
    delay(5);
    return;   // mid-dip: the glass is dark, nothing to draw
  }
  if (m) {
    if (restore) m->wake(g_settings);
    m->service(g_settings);
  }

  delay(5);
}
