// UsageMode.h — Claude usage meter feature.
//
// Shows 5h/7d usage bars while data is flowing, and the idle Claude face — two
// eyes that wiggle, blink and squint — once the daemon goes quiet. Owns its
// fetch (UsageClient), the face animation (ClaudeFace) and its render state.
#pragma once
#include "Mode.h"
#include "config.h"
#include "UsageData.h"

class UsageMode : public DisplayMode {
 public:
  const char* id() const override { return "usage"; }
  uint8_t     modeConst() const override { return MODE_USAGE; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  // repaint only (no refetch) — another mode may have drawn over the screen since,
  // so force the full layout back in, not just a value diff. Dropping
  // showingFace_ does the same for the idle face: it comes back through the
  // full-field repaint rather than ticking a band over the outgoing mode's screen.
  void wake(const Settings& s) override {
    needRender_ = true;
    layoutPrimed_ = false;
    showingFace_ = false;
  }

 private:
  bool contentChanged(const UsageData& u) const;
  void rememberContent(const UsageData& u);

  uint32_t usageRenderedOk_ = 0xFFFFFFFF;
  bool     showingFace_ = false;           // the idle face owns the screen
  bool     needRender_ = true;

  // Last content actually painted — compared in contentChanged() instead of
  // lastOkMs, since the daemon re-POSTs unchanged data too.
  bool     contentPrimed_ = false;

  // Whether the static layout (black bg + header) is currently painted, so a
  // routine update can skip the full-screen clear.
  bool     layoutPrimed_ = false;
  float    lastSessionPct_ = -1;
  float    lastWeeklyPct_ = -1;
  int      lastSessionResetMin_ = -1;
  int      lastWeeklyResetMin_ = -1;
  char     lastStatus_[16] = {0};
  bool     lastValid_ = false;
  bool     lastError_ = false;
};

extern UsageMode g_usageMode;
