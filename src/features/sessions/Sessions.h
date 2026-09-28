// Sessions — which Claude Code sessions are live, and what each is doing.
//
// The device cannot see any of this by itself. Claude Code hooks on the PC
// POST a line per session to /api/session; this module is only the store the
// web UI reads back. That keeps the failure mode obvious: no hooks installed
// means an empty list, never a wrong one.
//
// Deliberately not wired to the display. Driving the panel from session state
// is what made the idle face take the usage screen hostage before, so this
// feature ends at the web UI.
#pragma once
#include <Arduino.h>
#include "config.h"

// A session is one Claude Code conversation, keyed by its own session id.
#define SESSION_MAX        6    // oldest is evicted when a seventh appears
#define SESSION_ID_LEN     8    // first 8 chars of the uuid identify it well enough
#define SESSION_LABEL_LEN 23    // usually the project directory's name

enum SessionState : uint8_t {
  SESSION_IDLE    = 0,   // known, but neither working nor waiting on you
  SESSION_WORKING = 1,   // thinking or answering  -> red, blinking
  SESSION_WAITING = 2,   // finished, wants your reply -> green, solid
};

struct SessionRow {
  char     id[SESSION_ID_LEN + 1];
  char     label[SESSION_LABEL_LEN + 1];
  uint8_t  state;
  uint32_t seenMs;       // millis() of the last update for this session
  bool     used;
};

// Record (or update) one session. Returns false only if the id is unusable.
bool sessionsTouch(const char* id, const char* label, uint8_t state);

// Forget one session — the hook sends this when a session ends.
void sessionsDrop(const char* id);

// Forget every session.
void sessionsClear();

// Read-only view for the web UI. Rows with used=false are free slots.
const SessionRow* sessionsAll();

// Drop rows nothing has reported for their state's lifetime (see config.h).
// Called when the list is read rather than from the main loop: it only matters
// at the point of being shown.
void sessionsExpire();

// How many sessions are live right now. The carousel asks before including this
// screen: rotating onto an empty board is worse than not rotating onto it.
uint8_t sessionsCount();
