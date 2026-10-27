/*
 * Ship of Harkinian for the PlayStation 5 - the debug server's side of the game
 * (PS5_DEVTOOLS builds only). Everything here runs on the game thread, between two
 * frames, called from ps5_devtools_frame (devtools_server.cpp).
 *
 * Warps go the way SoH's own "entrance" console command does; loading a save the
 * way the file select does (FileChoose_LoadGame); saving the way the pause menu does.
 *
 * SPDX-License-Identifier: MIT
 */
#include "devtools.h"

#include <cstring>
#include <string>

#include "soh/ActorDB.h"
#include "soh/OTRGlobals.h"
#include "soh/SaveManager.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"

#define Path _Path
#define PATH_HACK
#include <ship/Context.h>
#include <ship/debug/Console.h>
#undef PATH_HACK
#undef Path
#include <libultraship/bridge/consolevariablebridge.h>

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
extern GameState* gGameState;
extern EntranceInfo gEntranceTable[];
}

namespace devtools {
namespace {

const char* const kEntranceNames[] = {
#define DEFINE_ENTRANCE(e, ...) #e,
#include "tables/entrance_table.h"
#undef DEFINE_ENTRANCE
};
constexpr int kEntranceCount = sizeof(kEntranceNames) / sizeof(kEntranceNames[0]);

const char* const kSceneNames[] = {
#define DEFINE_SCENE(_0, _1, e, ...) #e,
#include "tables/scene_table.h"
#undef DEFINE_SCENE
};
constexpr int kSceneCount = sizeof(kSceneNames) / sizeof(kSceneNames[0]);

std::string Upper(std::string s) {
    for (char& c : s)
        c = (char)toupper((unsigned char)c);
    return s;
}

int Lookup(const json& j, const char* const* names, int count, const char* prefix, const char* what) {
    if (j.is_number_integer())
        return j.get<int>();
    if (!j.is_string())
        throw Error(std::string(what) + ": a number or a name");
    std::string s = Upper(j.get<std::string>());
    if (s.rfind("0X", 0) == 0)
        return (int)strtol(s.c_str(), nullptr, 16);
    if (s.rfind(prefix, 0) != 0)
        s = prefix + s;
    for (int i = 0; i < count; i++)
        if (s == names[i])
            return i;
    throw Error(std::string("unknown ") + what + " " + j.get<std::string>());
}

const char* EntranceName(int e) {
    return e >= 0 && e < kEntranceCount ? kEntranceNames[e] : "?";
}

const char* SceneName(int s) {
    return s >= 0 && s < kSceneCount ? kSceneNames[s] : "?";
}

const char* StateName() {
    GameState* gs = gGameState;
    if (gs == nullptr)
        return "none";
    if (gs->destroy == Play_Destroy)
        return gSaveContext.gameMode == GAMEMODE_TITLE_SCREEN ? "title" : "play";
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
    return "unknown";
}

PlayState* Play() {
    if (gGameState == nullptr || gGameState->destroy != Play_Destroy || gPlayState == nullptr)
        return nullptr;
    return gPlayState;
}

PlayState* NeedPlay() {
    PlayState* play = Play();
    if (play == nullptr || gSaveContext.gameMode != GAMEMODE_NORMAL)
        throw Error(std::string("needs a game in play (now: ") + StateName() + ")");
    return play;
}

Player* Link(PlayState* play) {
    return play ? (Player*)play->actorCtx.actorLists[ACTORCAT_PLAYER].head : nullptr;
}

json Vec(const Vec3f& v) {
    return { v.x, v.y, v.z };
}

json HexWords(const u16* w, int n) {
    json a = json::array();
    for (int i = 0; i < n; i++) {
        char b[8];
        snprintf(b, sizeof b, "%04x", w[i]);
        a.push_back(b);
    }
    return a;
}

int TimeOfDay(const json& t) {
    if (t.is_number())
        return std::clamp(t.get<int>(), 0, 0xFFFF);
    std::string s = t.get<std::string>();
    if (s == "day" || s == "morning")
        return 0x4555; /* 6:30 */
    if (s == "noon")
        return 0x8000;
    if (s == "evening")
        return 0xB000;
    if (s == "night")
        return 0xD000;
    if (s == "midnight")
        return 0x0000;
    size_t colon = s.find(':'); /* HH:MM */
    if (colon != std::string::npos) {
        int h = atoi(s.c_str()), m = atoi(s.c_str() + colon + 1);
        return (int)(((h * 60 + m) * 0x10000L) / (24 * 60)) & 0xFFFF;
    }
    throw Error("time: 0..65535, HH:MM, day, noon, evening, night or midnight");
}

} // namespace

void FillSnapshot(Snapshot& s) {
    s.state = StateName();
    s.state_frames = gGameState ? gGameState->frames : 0;
    s.age = gSaveContext.linkAge;
    s.health = gSaveContext.health;
    s.health_max = gSaveContext.healthCapacity;
    s.file = gSaveContext.fileNum;
    s.game_mode = gSaveContext.gameMode;
    s.entrance = (u16)gSaveContext.entranceIndex;
    PlayState* play = Play();
    if (play) {
        s.scene = play->sceneNum;
        s.room = play->roomCtx.curRoom.num;
        s.next_entrance = (u16)play->nextEntranceIndex;
        s.transition_trigger = play->transitionTrigger;
        s.transition_mode = play->transitionMode;
        s.gameplay_frames = play->gameplayFrames;
        s.loaded = play->transitionTrigger == TRANS_TRIGGER_OFF && play->transitionMode == TRANS_MODE_OFF &&
                   gGameState->frames >= 2;
        if (Player* link = Link(play)) {
            s.pos[0] = link->actor.world.pos.x;
            s.pos[1] = link->actor.world.pos.y;
            s.pos[2] = link->actor.world.pos.z;
            s.yaw = (u16)link->actor.shape.rot.y;
        }
    }
}

json Globals() {
    return { { "gSaveContext", (uint64_t)(uintptr_t)&gSaveContext },
             { "gPlayState", (uint64_t)(uintptr_t)gPlayState },
             { "gGameState", (uint64_t)(uintptr_t)gGameState },
             { "gEntranceTable", (uint64_t)(uintptr_t)gEntranceTable },
             { "player", (uint64_t)(uintptr_t)Link(Play()) } };
}

json State(const json& req) {
    Snapshot snap;
    FillSnapshot(snap);
    json out = SnapshotJson(snap);
    out["state"] = StateName();
    out["entrance_name"] = EntranceName(snap.entrance);
    out["save"] = { { "day_time", gSaveContext.dayTime },
                    { "night", gSaveContext.nightFlag != 0 },
                    { "total_days", gSaveContext.totalDays },
                    { "rupees", gSaveContext.rupees },
                    { "magic", gSaveContext.magic },
                    { "deaths", gSaveContext.deaths },
                    { "cutscene_index", gSaveContext.cutsceneIndex },
                    { "saved_scene", gSaveContext.savedSceneNum } };
    PlayState* play = Play();
    if (play) {
        out["scene_name"] = SceneName(play->sceneNum);
        if (Player* link = Link(play)) {
            out["link"] = { { "pos", Vec(link->actor.world.pos) },
                            { "rot", { link->actor.shape.rot.x, link->actor.shape.rot.y, link->actor.shape.rot.z } },
                            { "speed", link->actor.speedXZ },
                            { "state_flags", { link->stateFlags1, link->stateFlags2 } } };
        }
        if (req.value("actors", true)) {
            json actors = json::array();
            for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
                int guard = 0;
                for (Actor* a = play->actorCtx.actorLists[cat].head; a && guard < 512; a = a->next, guard++) {
                    json e = { { "id", a->id },
                               { "category", cat },
                               { "params", a->params },
                               { "room", a->room },
                               { "pos", Vec(a->world.pos) },
                               { "yaw", (u16)a->shape.rot.y },
                               { "addr", (uint64_t)(uintptr_t)a } };
                    if (ActorDB::Instance)
                        e["name"] = ActorDB::Instance->RetrieveEntry(a->id).name;
                    actors.push_back(e);
                }
            }
            out["actors"] = actors;
        }
    }
    return out;
}

json Flags(const json& req) {
    json out = { { "event_chk_inf", HexWords(gSaveContext.eventChkInf, ARRAY_COUNT(gSaveContext.eventChkInf)) },
                 { "item_get_inf", HexWords(gSaveContext.itemGetInf, ARRAY_COUNT(gSaveContext.itemGetInf)) },
                 { "inf_table", HexWords(gSaveContext.infTable, ARRAY_COUNT(gSaveContext.infTable)) },
                 { "event_inf", HexWords(gSaveContext.eventInf, ARRAY_COUNT(gSaveContext.eventInf)) } };
    PlayState* play = Play();
    if (play) {
        auto& f = play->actorCtx.flags;
        out["scene"] = { { "swch", f.swch },   { "temp_swch", f.tempSwch }, { "chest", f.chest },
                         { "clear", f.clear }, { "temp_clear", f.tempClear }, { "collect", f.collect },
                         { "temp_collect", f.tempCollect } };
    }
    if (req.contains("scene")) {
        int s = Lookup(req["scene"], kSceneNames, kSceneCount, "SCENE_", "scene");
        if (s < 0 || s >= (int)ARRAY_COUNT(gSaveContext.sceneFlags))
            throw Error("scene out of range");
        auto& f = gSaveContext.sceneFlags[s];
        out["saved_scene_flags"] = { { "scene", s }, { "chest", f.chest }, { "swch", f.swch }, { "clear", f.clear },
                                     { "collect", f.collect }, { "rooms", f.rooms }, { "floors", f.floors } };
    }
    return out;
}

json Warp(const json& req) {
    PlayState* play = NeedPlay();
    int entrance;
    if (req.contains("entrance")) {
        entrance = Lookup(req["entrance"], kEntranceNames, kEntranceCount, "ENTR_", "entrance");
    } else if (req.contains("scene")) {
        int scene = Lookup(req["scene"], kSceneNames, kSceneCount, "SCENE_", "scene");
        int spawn = req.value("spawn", 0);
        entrance = -1;
        for (int i = 0; i < kEntranceCount && entrance < 0; i++)
            if (gEntranceTable[i].scene == scene && gEntranceTable[i].spawn == spawn)
                entrance = i;
        if (entrance < 0)
            throw Error("no entrance for that scene and spawn");
    } else {
        throw Error("give entrance, or scene (and spawn)");
    }
    if (entrance < 0 || entrance >= kEntranceCount)
        throw Error("entrance out of range");
    if (req.contains("age")) {
        std::string age = req["age"];
        if (age != "child" && age != "adult")
            throw Error("age: child or adult");
        play->linkAgeOnLoad = age == "child" ? LINK_AGE_CHILD : LINK_AGE_ADULT;
    }
    if (req.contains("time")) {
        const int t = TimeOfDay(req["time"]);
        gSaveContext.dayTime = gSaveContext.skyboxTime = (u16)t;
        gSaveContext.nightFlag = !(t >= 0x4555 && t <= 0xC001);
    }
    gSaveContext.respawnFlag = 0;
    play->nextEntranceIndex = entrance;
    play->transitionTrigger = TRANS_TRIGGER_START;
    play->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.nextTransitionType = TRANS_TYPE_INSTANT;
    return { { "entrance", entrance },
             { "entrance_name", EntranceName(entrance) },
             { "scene", gEntranceTable[entrance].scene },
             { "scene_name", SceneName(gEntranceTable[entrance].scene) },
             { "spawn", gEntranceTable[entrance].spawn } };
}

json CVar(const json& req) {
    const std::string name = req.value("name", std::string());
    if (name.empty())
        throw Error("name");
    if (req.value("clear", false)) {
        CVarClear(name.c_str());
    } else if (req.contains("value")) {
        const json& v = req["value"];
        const std::string type = req.value("type", std::string());
        if (type == "float" || (type.empty() && v.is_number_float()))
            CVarSetFloat(name.c_str(), v.get<float>());
        else if (type == "string" || (type.empty() && v.is_string()))
            CVarSetString(name.c_str(), v.get<std::string>().c_str());
        else if (v.is_boolean())
            CVarSetInteger(name.c_str(), v.get<bool>());
        else
            CVarSetInteger(name.c_str(), v.get<int>());
    }
    if (req.value("save", false))
        CVarSave();
    json out = { { "name", name } };
    auto cvar = CVarGet(name.c_str());
    if (!cvar) {
        out["exists"] = false;
        return out;
    }
    out["exists"] = true;
    switch (cvar->Type) {
        case Ship::ConsoleVariableType::Integer:
            out["type"] = "int";
            out["value"] = cvar->Integer;
            break;
        case Ship::ConsoleVariableType::Float:
            out["type"] = "float";
            out["value"] = cvar->Float;
            break;
        case Ship::ConsoleVariableType::String:
            out["type"] = "string";
            out["value"] = cvar->String ? cvar->String : "";
            break;
        case Ship::ConsoleVariableType::Color:
            out["type"] = "color";
            out["value"] = { cvar->Color.r, cvar->Color.g, cvar->Color.b, cvar->Color.a };
            break;
        case Ship::ConsoleVariableType::Color24:
            out["type"] = "color24";
            out["value"] = { cvar->Color24.r, cvar->Color24.g, cvar->Color24.b };
            break;
    }
    return out;
}

json Save(const json& req) {
    PlayState* play = NeedPlay();
    const int slot = req.value("slot", (int)gSaveContext.fileNum);
    if (slot < 0 || slot > 2)
        throw Error("slot: 0, 1 or 2");
    const int previous = gSaveContext.fileNum;
    Play_SaveSceneFlags(play);
    gSaveContext.savedSceneNum = play->sceneNum;
    gSaveContext.fileNum = slot;
    SaveManager::Instance->SaveFile(slot);
    SaveManager::Instance->ThreadPoolWait();
    gSaveContext.fileNum = previous;
    return { { "slot", slot }, { "scene", play->sceneNum }, { "entrance", (u16)gSaveContext.entranceIndex } };
}

json Load(const json& req) {
    const int slot = req.value("slot", 0);
    if (slot < 0 || slot > 2)
        throw Error("slot: 0, 1 or 2");
    const std::string state = StateName();
    if (state != "play" && state != "title" && state != "file_select")
        throw Error("load from play, the title screen or the file select (now: " + state + ")");
    if (!SaveManager::Instance->SaveFile_Exist(slot))
        throw Error("slot " + std::to_string(slot) + " is empty");

    /* as FileChoose_LoadGame does it, without the sound and the debug map select */
    gSaveContext.fileNum = slot;
    gSaveContext.gameMode = GAMEMODE_NORMAL;
    Sram_OpenSave();
    SET_NEXT_GAMESTATE(gGameState, Play_Init, PlayState);
    gGameState->running = false;

    gSaveContext.respawn[0].entranceIndex = ENTR_LOAD_OPENING;
    gSaveContext.respawnFlag = 0;
    gSaveContext.seqId = (u8)NA_BGM_DISABLED;
    gSaveContext.natureAmbienceId = 0xFF;
    gSaveContext.showTitleCard = true;
    if (!CVarGetInteger(CVAR_ENHANCEMENT("DogFollowsEverywhere"), 0))
        gSaveContext.dogParams = 0;
    gSaveContext.timerState = TIMER_STATE_OFF;
    gSaveContext.subTimerState = SUBTIMER_STATE_OFF;
    gSaveContext.eventInf[0] = gSaveContext.eventInf[1] = gSaveContext.eventInf[2] = gSaveContext.eventInf[3] = 0;
    gSaveContext.prevHudVisibilityMode = 0x32;
    gSaveContext.nayrusLoveTimer = 0;
    gSaveContext.healthAccumulator = 0;
    gSaveContext.magicState = MAGIC_STATE_IDLE;
    gSaveContext.prevMagicState = MAGIC_STATE_IDLE;
    gSaveContext.forcedSeqId = NA_BGM_GENERAL_SFX;
    gSaveContext.skyboxTime = 0;
    gSaveContext.nextTransitionType = TRANS_NEXT_TYPE_DEFAULT;
    gSaveContext.nextCutsceneIndex = 0xFFEF;
    gSaveContext.cutsceneTrigger = 0;
    gSaveContext.chamberCutsceneNum = 0;
    gSaveContext.nextDayTime = 0xFFFF;
    gSaveContext.retainWeatherMode = 0;
    for (size_t i = 0; i < ARRAY_COUNT(gSaveContext.buttonStatus); i++)
        gSaveContext.buttonStatus[i] = BTN_ENABLED;
    gSaveContext.forceRisingButtonAlphas = gSaveContext.nextHudVisibilityMode = gSaveContext.hudVisibilityMode =
        gSaveContext.hudVisibilityModeTimer = gSaveContext.magicCapacity = 0;
    gSaveContext.magicFillTarget = gSaveContext.magic;
    gSaveContext.magic = 0;
    gSaveContext.magicLevel = gSaveContext.magic;
    gSaveContext.naviTimer = 0;
    GameInteractor_ExecuteOnLoadGame(gSaveContext.fileNum);
    return { { "slot", slot }, { "entrance", (u16)gSaveContext.entranceIndex }, { "from", state } };
}

json Console(const json& req) {
    const std::string line = req.value("line", std::string());
    if (line.empty())
        throw Error("line: the console command");
    auto console = Ship::Context::GetRawInstance()->GetConsole();
    std::string output;
    const int32_t rc = console->Run(line, &output);
    return { { "result", rc }, { "output", output } };
}

void ApplyPad(void* pads, uint16_t buttons, int8_t sx, int8_t sy, int8_t rx, int8_t ry, bool override_pad,
              bool set_stick, bool set_right) {
    OSContPad* pad = (OSContPad*)pads; /* player 1 */
    if (override_pad) {
        pad->button = buttons;
        pad->stick_x = sx;
        pad->stick_y = sy;
        pad->right_stick_x = rx;
        pad->right_stick_y = ry;
        return;
    }
    pad->button |= buttons;
    if (set_stick) {
        pad->stick_x = sx;
        pad->stick_y = sy;
    }
    if (set_right) {
        pad->right_stick_x = rx;
        pad->right_stick_y = ry;
    }
}

} // namespace devtools
