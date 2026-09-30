// Mode.h — the display-mode interface.
//
// Each feature (ticker / usage / radar) is a self-contained DisplayMode: it owns
// its own data fetch, render, dirty-tracking and settings slice. main.cpp keeps a
// registry of the compiled-in modes and dispatches to whichever one matches the
// active settings.mode — it holds no per-feature state of its own.
#pragma once
#include <Arduino.h>
#include "Settings.h"

// Implemented in main.cpp. A mode about to replace everything on the screen
// asks for a backlight dip so the repaint happens in the dark, and paints when
// it is woken at the bottom of it. False means a dip is already in flight (the
// mode is being serviced from inside one) — paint now instead.
bool appRequestDip();

class DisplayMode {
 public:
  virtual ~DisplayMode() {}

  // Stable string id (also the settings.mode token, e.g. "stocks"/"usage"/"radar").
  virtual const char* id() const = 0;
  // The MODE_* constant this mode answers to (matched against settings.mode).
  virtual uint8_t modeConst() const = 0;

  // Has this feature anything worth a carousel stop right now? The rotation is
  // for pages with something on them: the sessions board with no sessions, the
  // usage meter with no numbers (its idle face is a screensaver, not a feature)
  // and a panel slot nothing is pushing to are all dead air, and a fifteen
  // second stop on dead air reads as a bug. Features that always have something
  // to say keep the default.
  virtual bool carouselReady(const Settings& s) const { (void)s; return true; }

  virtual void begin(const Settings& s) {}          // one-time init at boot
  virtual void service(const Settings& s) {}        // every loop tick: fetch + render
  virtual void invalidate(const Settings& s) {}     // settings changed: re-init + repaint
  // Another mode drew on the screen (carousel switch): repaint from cached data,
  // do NOT refetch. Falls back to invalidate for modes without a light path.
  virtual void wake(const Settings& s) { invalidate(s); }
};
