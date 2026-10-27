/*
 * Ship of Harkinian for the PlayStation 5 - developer tools (PS5_DEVTOOLS builds only).
 *
 * A debug server on TCP 9200 that a test runner on another machine drives with one
 * JSON object per line (see DEVTOOLS.md). Built only with -DPS5_DEVTOOLS=ON (the dev
 * build: PS5_DEVTOOLS=1 ps5/tools/build.sh); the store build has none of it. Even
 * then it only starts when <data>/devtools.enable exists.
 *
 *   devtools_server.cpp  sockets, the protocol, the queue of jobs for the game
 *                        thread, input injection, frame timing, screenshots
 *   devtools_game.cpp    what reads and changes the game: state, warps, CVars,
 *                        saves, console commands (runs on the game thread)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The hooks the game calls; each is a no-op until the server has started. */
void ps5_devtools_start(void);                /* end of InitOTR */
void ps5_devtools_frame(void);                /* graph.c RunFrame, once per game frame, before the pads */
void ps5_devtools_pad(void* pads);            /* padmgr.c, OSContPad[4] just read */
void ps5_devtools_present(uint32_t width, uint32_t height, size_t vertex_peak, size_t vertex_cap,
                          size_t uniform_peak, size_t uniform_cap); /* gfx_vulkan, every present */
/* devloop_shot.c: a screenshot asked for over the socket; the PNG it made */
bool ps5_devtools_shot_wanted(void);
int ps5_devtools_shot_max_width(void);
void ps5_devtools_shot_png(const uint8_t* png, size_t size, int width, int height);

#ifdef __cplusplus
}

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace devtools {

using json = nlohmann::json;

/* What the server thread may read without the game thread: taken once a frame. */
struct Snapshot {
    uint64_t frame = 0;        /* devtools' own game-frame counter */
    double time = 0;           /* seconds (monotonic) when taken */
    std::string state = "none"; /* play, title, file_select, logo, setup, opening, map_select */
    uint32_t state_frames = 0; /* gGameState->frames: 0 at each game state's start */
    int scene = -1, room = -1, entrance = -1, next_entrance = -1;
    int transition_trigger = 0, transition_mode = 0;
    bool loaded = false;       /* in play, no transition running */
    float pos[3] = {};
    int yaw = 0, age = 0, health = 0, health_max = 0, file = -1, game_mode = 0;
    uint32_t gameplay_frames = 0;
    uint64_t input_frames = 0; /* injected frames still queued */
};

/* devtools_game.cpp, all on the game thread */
void FillSnapshot(Snapshot& s);
json State(const json& req);
json Warp(const json& req);
json CVar(const json& req);
json Save(const json& req);
json Load(const json& req);
json Console(const json& req);
json Flags(const json& req);
json Globals();
void ApplyPad(void* pads, uint16_t buttons, int8_t sx, int8_t sy, int8_t rx, int8_t ry, bool override_pad,
              bool set_stick, bool set_right);

/* devtools_server.cpp */
json SnapshotJson(const Snapshot& s);

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace devtools
#endif
