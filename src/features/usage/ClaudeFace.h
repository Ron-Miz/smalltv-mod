// ClaudeFace.h — the idle "Claude face": two eyes on a plain field.
//
// The eye shapes come from clawd-mochi
// (https://github.com/yousifamanuel/clawd-mochi): upright bars, a thin blink
// bar, and the > < squint. What sits on top of them is this firmware's own —
// the source plays one fixed routine per key press, whereas an idle screen is
// watched for minutes at a time, so here the two eyes are posed independently
// and a routine is drawn at random from a table, mirrored half the time, with a
// random rest between. Nothing repeats on a fixed cycle.
//
// The other structural change: clawd-mochi paces its animations with delay() in
// a blocking loop, which the SmallTV cannot do — service() runs next to the web
// server, MQTT and the WiFi stack. Every routine is therefore a script of poses
// that faceTick() advances off millis(), returning true only when the pose
// actually changed.
//
// Drawing goes through FaceCanvas rather than Arduino_GFX directly, so the host
// simulator (tools/face_sim.cpp) links this same .cpp and replays the real draw
// calls — the animation can be reviewed without flashing a device.
#pragma once
#include <stdint.h>

// The panel every target carries (config.h fixes TFT_WIDTH/TFT_HEIGHT at 240);
// UsageMode static_asserts that these still agree.
#define FACE_W 240
#define FACE_H 240

// Every shape is axis-aligned, so filled rectangles are the only primitive the
// face needs — the squint included, which is drawn as a run of 1 px columns
// rather than stacked diagonal lines. That matters on an ESP8266: a rectangle
// fill is one address window on the SPI bus, where a line walk is one write per
// pixel, and the difference is the whole of the flicker on a panel with no
// framebuffer. begin()/end() bracket a pose so the driver can hold a single
// bus transaction across it.
class FaceCanvas {
 public:
  virtual ~FaceCanvas() {}
  virtual void begin() {}
  virtual void end() {}
  virtual void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) = 0;
};

// ---------------------------------------------------------------------------
// Moods. The face is an ambient status light as well as an idle animation: a
// mood does not script a pose, it re-weights which routines get picked and how
// long the rests between them run. Claude thinking looks like eyes casting
// about; Claude working looks calm and slow; Claude waiting on you looks
// restless. Nothing about the shapes changes, only what plays and how often —
// which is what keeps it readable across a room without being an alert.
// ---------------------------------------------------------------------------
#define FACE_MOOD_IDLE     0   // nothing pushed: the balanced mix
#define FACE_MOOD_THINKING 1
#define FACE_MOOD_WORKING  2
#define FACE_MOOD_WAITING  3
#define FACE_MOOD_DONE     4
#define FACE_MOOD_ERROR    5
#define FACE_MOOD_COUNT    6

// Name -> mood, -1 when unknown. faceMoodNameAt walks the table so a caller can
// report the valid set without duplicating it.
int         faceMoodFind(const char* name);
const char* faceMoodNameAt(uint8_t mood);

// Set the mood. `ttlMs` 0 takes the mood's own timeout; when it lapses without
// another push the face falls back to FACE_MOOD_IDLE, so a hook script that
// dies mid-session cannot leave the panel stuck looking busy forever.
void        faceSetMood(uint8_t mood, uint32_t nowMs, uint32_t ttlMs);
uint8_t     faceMood();
const char* faceMoodName();

// Start the animation. `seed` picks the sequence of routines: pass something
// that differs run to run on the device, or a constant in a harness to replay
// the same sequence. `nowMs` is millis() on the device, simulated time in the
// harness — the animation never reads a clock itself.
void faceReset(uint32_t nowMs, uint32_t seed);

// Advance. Returns true when the pose changed and a redraw is due.
bool faceTick(uint32_t nowMs);

// Draw the current pose. `full` repaints the whole field (entering the screen,
// or after another mode drew over it); otherwise only the two rectangles the
// eyes vacated or moved into are repainted.
void faceRender(FaceCanvas& c, uint16_t bg, uint16_t ink, bool full);

// The small header glyph on the usage stats screen: one pair of eyes scaled
// into a 40x40 box at (x,y), drawn in `ink` over whatever is already there.
void faceBadge(FaceCanvas& c, int16_t x, int16_t y, uint16_t ink);

// What is playing, and the hold of the pose now showing — diagnostics, and what
// the host harness records alongside each frame.
const char* faceRoutineName();
uint16_t    faceHoldMs();
