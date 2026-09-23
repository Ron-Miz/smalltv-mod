// face_sim.cpp — host-side preview of the idle Claude face.
//
// Links features/usage/ClaudeFace.cpp unchanged and drives it with simulated
// time and a fixed seed, so the routines, the poses and the hold times reviewed
// here are the ones the device will draw. Nothing Arduino is involved:
// FaceCanvas is the whole interface between the animation and the panel, and
// this file implements it twice — once into a pixel buffer (PPM, to look at)
// and once as a recorder (JSON of the draw calls, replayed by the preview).
//
// The animation picks its routines at random, so there is no cycle to walk:
// the harness records a fixed number of poses from a given seed instead. Change
// the seed and you get a different, equally valid run.
//
// Build and run from the repo root:
//     c++ -std=c++11 -O2 -Isrc/features/usage \
//         tools/face_sim.cpp src/features/usage/ClaudeFace.cpp -o /tmp/face_sim
//     /tmp/face_sim <outdir> [poses] [seed] [mood]
//
// `mood` is a state name from ClaudeFace's own table (idle / thinking /
// working / waiting / done / error). It is pinned for the whole recording with
// a TTL far longer than the run, so the review shows the mood steadily rather
// than watching it lapse back to idle halfway through.
//
// This is a review tool, not part of any firmware image.
#include "ClaudeFace.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void rgb565(uint16_t c, unsigned char& r, unsigned char& g, unsigned char& b) {
  r = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
  g = (unsigned char)(((c >> 5)  & 0x3F) * 255 / 63);
  b = (unsigned char)(( c        & 0x1F) * 255 / 31);
}

// ---- canvas 1: a pixel buffer ---------------------------------------------
struct PixelCanvas : FaceCanvas {
  unsigned char px[FACE_W * FACE_H * 3];
  PixelCanvas() { memset(px, 0, sizeof(px)); }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    unsigned char r, g, b;
    rgb565(color, r, g, b);
    for (int16_t j = 0; j < h; j++) {
      const int16_t yy = (int16_t)(y + j);
      if (yy < 0 || yy >= FACE_H) continue;
      for (int16_t i = 0; i < w; i++) {
        const int16_t xx = (int16_t)(x + i);
        if (xx < 0 || xx >= FACE_W) continue;
        unsigned char* p = px + (yy * FACE_W + xx) * 3;
        p[0] = r; p[1] = g; p[2] = b;
      }
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
// Captures the ops so the preview replays the device's actual draw calls rather
// than a picture of them.
struct RecordCanvas : FaceCanvas {
  std::string ops;
  int count = 0;

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    char buf[96];
    snprintf(buf, sizeof(buf), "[%d,%d,%d,%d,%u]", x, y, w, h, (unsigned)color);
    if (!ops.empty()) ops += ",";
    ops += buf;
    count++;
  }
};

// The two colours the firmware passes in: C_ACCENT from UsageMode.cpp
// (terra-cotta #d97757 as RGB565), and black eyes.
static const uint16_t kBg  = 0xDBAA;
static const uint16_t kInk = 0x0000;

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : ".";
  const int   poses = argc > 2 ? atoi(argv[2]) : 100;
  const uint32_t seed = argc > 3 ? (uint32_t)strtoul(argv[3], 0, 10) : 20260923u;
  const char* moodName = argc > 4 ? argv[4] : "idle";

  const int mood = faceMoodFind(moodName);
  if (mood < 0) { fprintf(stderr, "unknown mood: %s\n", moodName); return 2; }

  uint32_t now = 0;
  PixelCanvas pix;

  std::string json = "{\"w\":" + std::to_string(FACE_W) +
                     ",\"h\":" + std::to_string(FACE_H) +
                     ",\"seed\":" + std::to_string(seed) +
                     ",\"mood\":\"" + moodName + "\",\"frames\":[";

  // Pose 0 is drawn with full=true (entering the screen); every later pose with
  // full=false, exactly as UsageMode drives it. The pixel canvas therefore also
  // proves the per-eye erase leaves nothing of the previous pose behind — a
  // stale pixel would simply persist in the buffer from frame to frame.
  faceReset(now, seed);
  faceSetMood((uint8_t)mood, now, 3600000UL);   // pinned: outlast the recording
  int maxOps = 0, totalOps = 0;
  for (int i = 0; i < poses; i++) {
    const bool full = (i == 0);
    const std::string routine = faceRoutineName();

    RecordCanvas rec;
    faceRender(rec, kBg, kInk, full);
    faceRender(pix, kBg, kInk, full);
    if (rec.count > maxOps) maxOps = rec.count;
    totalOps += rec.count;

    if (i < 24) {   // a contact sheet's worth, to look at directly
      char name[512];
      snprintf(name, sizeof(name), "%s/pose_%02d.ppm", out.c_str(), i);
      pix.writePpm(name);
    }

    // Walk simulated time until faceTick reports the next pose, which measures
    // the hold rather than trusting the table.
    uint32_t held = 0;
    while (!faceTick(now)) { now += 1; held += 1; }

    if (i) json += ",";
    json += "{\"routine\":\"" + routine + "\",\"hold\":" + std::to_string(held) +
            ",\"ops\":[" + rec.ops + "]}";
  }
  json += "]}";

  const std::string jp = out + "/face_frames.json";
  FILE* f = fopen(jp.c_str(), "wb");
  if (!f) { perror(jp.c_str()); return 1; }
  fwrite(json.data(), 1, json.size(), f);
  fclose(f);

  printf("%-9s %d poses, seed %u, %u ms of animation\n", moodName, poses, seed, now);
  printf("fills per pose: %d max, %d mean\n", maxOps, totalOps / poses);
  printf("frames -> %s\n", jp.c_str());
  return 0;
}
