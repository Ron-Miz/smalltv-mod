// SessionsMode — the session board on the panel itself.
//
// Same data as the web UI's Sessions tab: a dot per live Claude Code session,
// red while it is working and green once it is waiting on you. The dot blinks
// while working, which is the only thing on this screen that redraws at any
// rate — everything else is repainted only when the list actually changes, so
// a screen full of idle sessions costs nothing.
#pragma once
#include "Mode.h"

class SessionsMode : public DisplayMode {
 public:
  const char* id() const override { return "sessions"; }
  uint8_t modeConst() const override { return MODE_SESSIONS; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override;

 private:
  void repaint();
  void blinkDots();

  // Signature of what is currently drawn: ids and states hashed together, so a
  // change of either forces a repaint and nothing else does.
  uint32_t drawn_ = 0xFFFFFFFF;
  uint8_t  rows_ = 0;            // rows actually on the glass
  bool     blinkOn_ = true;
  uint32_t lastBlinkMs_ = 0;
  bool     primed_ = false;
};

extern SessionsMode g_sessionsMode;
