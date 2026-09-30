// PanelMode — draws one pushed panel (see Panel.h).
//
// One instance per panel slot, all registered in main.cpp's mode table, so each
// live panel gets its own carousel stop with a full dwell rather than several
// panels sharing one. A slot with nothing pushed to it reports itself not ready
// and the carousel skips it, which is what keeps an unused slot invisible.
#pragma once
#include "Mode.h"
#include "features/panel/Panel.h"

#if WITH_PANEL

class PanelMode : public DisplayMode {
 public:
  void bind(uint8_t slot) { slot_ = slot; }

  const char* id() const override;
  uint8_t     modeConst() const override { return MODE_PANEL; }

  // Live only while something is pushing to this slot.
  bool carouselReady(const Settings& s) const override;

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override;

 private:
  void repaint(const PanelData& p);
  void drawRow(uint8_t slot, const PanelRow& r);

  uint8_t  slot_ = 0;
  uint32_t drawn_ = 0;            // signature of what is on the glass
  uint32_t rowSig_[PANEL_ROWS_MAX] = {0};
  uint8_t  shownRows_ = 0;
  bool     needFull_ = true;
  mutable char idBuf_[PANEL_ID_LEN + 1] = {0};
};

extern PanelMode g_panelModes[PANEL_MAX];

// Called once at boot to tell each instance which slot it draws.
void panelModesBind();

#endif  // WITH_PANEL
