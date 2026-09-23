// face_sim.cpp — host-side preview of the idle Claude face.
//
// Links features/usage/ClaudeFace.cpp unchanged and drives it with simulated
// time, so the poses and the hold times reviewed here are the ones the device
// will draw. Nothing Arduino is involved: FaceCanvas is the whole interface
// between the animation and the panel, and this file implements it twice —
// once into a pixel buffer (PPM per pose, to look at) and once as a recorder
// (JSON of the draw calls, replayed by the HTML preview).
//
// Build and run from the repo root:
//     c++ -std=c++11 -O2 -Isrc/features/usage \
//         tools/face_sim.cpp src/features/usage/ClaudeFace.cpp -o /tmp/face_sim
//     /tmp/face_sim <outdir>
//
// This is a review tool, not part of any firmware image.
#include "ClaudeFace.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- RGB565 -> 8:8:8, the way the panel expands it ------------------------
static void rgb565(uint16_t c, unsigned char& r, unsigned char& g, unsigned char& b) {
  r = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
  g = (unsigned char)(((c >> 5)  & 0x3F) * 255 / 63);
  b = (unsigned char)(( c        & 0x1F) * 255 / 31);
}

// ---- canvas 1: a pixel buffer ---------------------------------------------
struct PixelCanvas : FaceCanvas {
  unsigned char px[FACE_W * FACE_H * 3];

  PixelCanvas() { memset(px, 0, sizeof(px)); }

  void put(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || y < 0 || x >= FACE_W || y >= FACE_H) return;
    unsigned char r, g, b;
    rgb565(color, r, g, b);
    unsigned char* p = px + (y * FACE_W + x) * 3;
    p[0] = r; p[1] = g; p[2] = b;
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    for (int16_t j = 0; j < h; j++)
      for (int16_t i = 0; i < w; i++) put(x + i, y + j, color);
  }

  // Bresenham, matching Arduino_GFX's own line walk closely enough that the
  // chevron's stacked strokes land on the same pixels.
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
      put((int16_t)x0, (int16_t)y0, color);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 = (int16_t)(x0 + sx); }
      if (e2 <= dx) { err += dx; y0 = (int16_t)(y0 + sy); }
    }
  }

  void writePpm(const std::string& path) const {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", FACE_W, FACE_H);
    fwrite(px, 1, sizeof(px), f);
    fclose(f);
  }
};

// ---- canvas 2: a recorder --------------------------------------------------
// Captures the ops so the HTML preview replays the device's actual draw calls
// rather than a picture of them.
struct RecordCanvas : FaceCanvas {
  std::string ops;

  void add(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!ops.empty()) ops += ",";
    ops += buf;
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    add("[0,%d,%d,%d,%d,%u]", x, y, w, h, (unsigned)color);
  }
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override {
    add("[1,%d,%d,%d,%d,%u]", x0, y0, x1, y1, (unsigned)color);
  }
};

// ---- the two colours the firmware passes in -------------------------------
// C_ACCENT in UsageMode.cpp: terra-cotta #d97757 as RGB565, and black eyes.
static const uint16_t kBg  = 0xDBAA;
static const uint16_t kInk = 0x0000;

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : ".";

  const uint8_t steps = faceStepCount();
  uint32_t now = 0;

  // Pose 0 is drawn with full=true (entering the screen); every later pose with
  // full=false, exactly as UsageMode drives it. The pixel canvas therefore also
  // proves the band repaint leaves nothing of the previous pose behind: a stale
  // pixel would simply persist in the buffer across frames.
  PixelCanvas pix;
  std::string json = "{\"w\":";
  json += std::to_string(FACE_W) + ",\"h\":" + std::to_string(FACE_H) + ",\"frames\":[";

  faceReset(now);
  for (uint8_t i = 0; i < steps; i++) {
    const bool full = (i == 0);

    // Style before the tick below moves the script on to the next pose.
    const std::string style = faceStyleName();

    RecordCanvas rec;
    faceRender(rec, kBg, kInk, full);
    faceRender(pix, kBg, kInk, full);

    char name[512];
    snprintf(name, sizeof(name), "%s/pose_%02u.ppm", out.c_str(), (unsigned)i);
    pix.writePpm(name);

    // Walk simulated time forward until faceTick reports the next pose, which
    // also measures the hold rather than trusting the table.
    uint32_t held = 0;
    while (!faceTick(now)) { now += 1; held += 1; }

    if (i) json += ",";
    json += "{\"style\":\"" + style + "\",\"hold\":" +
            std::to_string(held) + ",\"full\":" + (full ? "true" : "false") +
            ",\"ops\":[" + rec.ops + "]}";

    printf("pose %2u  %-6s  hold %4u ms  %s\n", (unsigned)i,
           style.c_str(), (unsigned)held, full ? "(full repaint)" : "");
  }
  json += "]}";

  const std::string jp = out + "/face_frames.json";
  FILE* f = fopen(jp.c_str(), "wb");
  if (!f) { perror(jp.c_str()); return 1; }
  fwrite(json.data(), 1, json.size(), f);
  fclose(f);

  printf("\n%u poses, one cycle. frames -> %s\n", (unsigned)steps, jp.c_str());
  return 0;
}
