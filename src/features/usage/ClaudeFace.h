// ClaudeFace.h — the idle "Claude face": two eyes on a plain field.
//
// A port of the two eye views from clawd-mochi
// (https://github.com/yousifamanuel/clawd-mochi):
//   - normal eyes: upright bars that wiggle side to side, then blink twice
//   - squish eyes: a happy > < squint that opens and closes
// Geometry (eye size, gap, the 40 px upward offset) and the per-step timings are
// the source's, at its shipped "slow" animation speed.
//
// The one structural change: clawd-mochi paces its animations with delay() in a
// blocking loop, which the SmallTV cannot do — service() runs off the main loop
// next to the web server, MQTT and the WiFi stack, so a delay() there stalls all
// three. Both routines are therefore re-expressed as a script of poses that
// faceTick() advances off millis(), returning true only when the pose actually
// changed so the caller redraws no more often than the source did.
//
// Drawing goes through FaceCanvas instead of Arduino_GFX directly, so that the
// host simulator (tools/face_sim.cpp) links this same .cpp and replays the real
// draw calls — the animation can be reviewed without flashing a device.
#pragma once
#include <stdint.h>

// The panel every target carries (config.h fixes TFT_WIDTH/TFT_HEIGHT at 240);
// UsageMode static_asserts that these still agree.
#define FACE_W 240
#define FACE_H 240

// The two primitives the face needs. UsageMode wraps Arduino_GFX in one of these.
class FaceCanvas {
 public:
  virtual ~FaceCanvas() {}
  virtual void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) = 0;
  virtual void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) = 0;
};

// Restart the script from its first pose. `nowMs` is millis() on the device and
// simulated time in the host harness — the animation never reads the clock
// itself, which is what makes it reproducible off-device.
void faceReset(uint32_t nowMs);

// Advance the script. Returns true when the pose changed and a redraw is due.
bool faceTick(uint32_t nowMs);

// Draw the current pose. `full` repaints the whole 240x240 field (entering the
// screen, or after another mode drew over it); otherwise only the band the eyes
// can occupy is repainted, which is what keeps the animation flicker-free.
void faceRender(FaceCanvas& c, uint16_t bg, uint16_t ink, bool full);

// The small header glyph on the usage stats screen: one pair of eyes scaled into
// a 40x40 box at (x,y), drawn in `ink` over whatever is already there.
void faceBadge(FaceCanvas& c, int16_t x, int16_t y, uint16_t ink);

// Which routine is running ("normal" / "squish"), for diagnostics.
const char* faceStyleName();

// Script length, so the host harness can walk exactly one cycle.
uint8_t faceStepCount();
