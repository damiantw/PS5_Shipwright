/*
 * Ship of Harkinian for the PlayStation 5 - the game's part of a crash report.
 *
 * Called by ps5/src/crash.c after the backtrace, registers and log are on disk:
 * where the game was (game state, scene, room, entrance, Link's position, the
 * frame counters). The memory may be what crashed, so every pointer is checked
 * against the kernel's map first and nothing here allocates.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>

#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"

#include "crash.h"

extern PlayState* gPlayState;
extern GameState* gGameState;

int sceKernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);

static int
readable(const void* p, size_t size)
{
    void *s, *e;
    int prot = 0;

    if ((uintptr_t)p < 0x10000 || sceKernelQueryMemoryProtection((void*)p, &s, &e, &prot) != 0)
        return 0;
    return (prot & 1) && (uintptr_t)p + size <= (uintptr_t)e;
}

static const char*
state_name(const GameState* gs)
{
    if (gs->destroy == Play_Destroy)
        return gSaveContext.gameMode == GAMEMODE_TITLE_SCREEN ? "title (play)" : "play";
    if (gs->destroy == FileChoose_Destroy)
        return "file_select";
    if (gs->destroy == Title_Destroy)
        return "logo";
    if (gs->destroy == TitleSetup_Destroy)
        return "setup";
    if (gs->destroy == Opening_Destroy)
        return "opening";
    if (gs->destroy == Select_Destroy)
        return "map_select";
    return "?";
}

static void
context(char* out, size_t size)
{
    int n = 0;

#define ADD(...)                                                              \
    do {                                                                      \
        if (n >= 0 && (size_t)n < size)                                       \
            n += snprintf(out + n, size - n, __VA_ARGS__);                    \
    } while (0)

    if (readable(gGameState, sizeof(GameState)))
        ADD("  state %s, frames %u\n", state_name(gGameState), (unsigned)gGameState->frames);
    ADD("  save: file %d, entrance 0x%x, age %s, time 0x%04x, health %d/%d, mode %d\n", (int)gSaveContext.fileNum,
        (unsigned)gSaveContext.entranceIndex, gSaveContext.linkAge ? "child" : "adult", gSaveContext.dayTime,
        gSaveContext.health, gSaveContext.healthCapacity, (int)gSaveContext.gameMode);
    if (readable(gPlayState, sizeof(PlayState))) {
        PlayState* play = gPlayState;
        ADD("  play: scene 0x%02x, room %d, next entrance 0x%x, transition %d/%d, gameplay frames %u\n",
            play->sceneNum, play->roomCtx.curRoom.num, (unsigned)(u16)play->nextEntranceIndex,
            play->transitionTrigger, play->transitionMode, (unsigned)play->gameplayFrames);
        Actor* link = play->actorCtx.actorLists[ACTORCAT_PLAYER].head;
        if (readable(link, sizeof(Actor)))
            ADD("  link: pos %.1f %.1f %.1f, yaw 0x%04x\n", link->world.pos.x, link->world.pos.y, link->world.pos.z,
                (unsigned)(u16)link->shape.rot.y);
    }
#undef ADD
}

void
Ps5CrashContextInstall(void)
{
    ps5_crash_set_context(context);
}
