/*
 * Ship of Harkinian for the PlayStation 5 - the debug server (PS5_DEVTOOLS builds only).
 *
 * Starts only when <data>/devtools.enable exists. Listens on TCP 9200 (the file's
 * "port=" line changes it) and answers peers on private networks only (10/8,
 * 172.16/12, 192.168/16, 100.64/10 as Tailscale uses, 169.254/16, loopback). With
 * a "token=" line in the file a client must send {"cmd":"auth","token":...} first.
 *
 * One JSON object per line each way: {"id":1,"cmd":"ping"} gets {"id":1,"ok":true,...}
 * or {"id":1,"ok":false,"error":"..."}. Commands that touch the game are queued for
 * the game thread and run at the start of its next frame (ps5_devtools_frame), so a
 * warp, a CVar or a memory write never races the frame. DEVTOOLS.md lists them.
 *
 * SPDX-License-Identifier: MIT
 */
#include "devtools.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crash.h"
#include "platform.h"

extern "C" int sceKernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);

namespace devtools {
namespace {

constexpr int kProtocol = 1;
constexpr int kMaxClients = 4;
constexpr size_t kMaxLine = 1 << 20;
constexpr size_t kMaxMem = 64 * 1024;

std::atomic<bool> gRunning{ false };
std::atomic<int> gClients{ 0 };
int gPort = 9200;
std::string gToken;
std::string gConnect;               /* host:port to dial out to (reverse connection) */
bool gUseListen = true, gUseConnect = true, gUseMailbox = true;
std::string gDataDir = "/data/soh";

double Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string ReadFile(const std::string& path, size_t max = 1 << 16) {
    std::string out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return out;
    char buf[4096];
    size_t n;
    while (out.size() < max && (n = fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    fclose(f);
    return out;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

/* What each transport managed, with errno: klog, and <data>/devtools/status.txt (FTP-readable) */
void Status(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Status(const char* format, ...) {
    static std::mutex lock;
    char line[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    say("devtools: %s", line);
    std::lock_guard<std::mutex> guard(lock);
    std::string dir = gDataDir + "/devtools";
    mkdir(dir.c_str(), 0777);
    FILE* f = fopen((dir + "/status.txt").c_str(), "a");
    if (f) {
        char stamp[32];
        time_t t = time(nullptr);
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", gmtime(&t));
        fprintf(f, "%s UTC  %s\n", stamp, line);
        fclose(f);
    }
}

/* ------------------------------------------------------------------ the game thread's queue */

struct Job {
    std::function<json()> fn;
    json result;
    std::string error;
    bool done = false;
};
std::mutex gJobLock;
std::condition_variable gJobDone;
std::deque<std::shared_ptr<Job>> gJobs;

json OnGameThread(std::function<json()> fn, double timeout = 10.0) {
    auto job = std::make_shared<Job>();
    job->fn = std::move(fn);
    std::unique_lock<std::mutex> lock(gJobLock);
    gJobs.push_back(job);
    if (!gJobDone.wait_for(lock, std::chrono::duration<double>(timeout), [&] { return job->done; })) {
        auto it = std::find(gJobs.begin(), gJobs.end(), job);
        if (it != gJobs.end()) {
            gJobs.erase(it);
            throw Error("the game thread ran no frame for " + std::to_string((int)timeout) + " s");
        }
        gJobDone.wait(lock, [&] { return job->done; }); /* it is running now */
    }
    if (!job->error.empty())
        throw Error(job->error);
    return job->result;
}

/* ------------------------------------------------------------------ the per-frame snapshot */

std::mutex gSnapLock;
std::condition_variable gSnapChanged;
Snapshot gSnap;
uint64_t gFrame;

/* ------------------------------------------------------------------ input injection */

struct InputStep {
    uint16_t buttons = 0;
    int8_t sx = 0, sy = 0, rx = 0, ry = 0;
    bool override_pad = false, set_stick = false, set_right = false;
    uint64_t frames = 1;
};
std::mutex gInputLock;
std::deque<InputStep> gInput;
uint64_t gInputFrames;

const struct {
    const char* name;
    uint16_t mask;
} kButtons[] = {
    { "A", 0x8000 },      { "B", 0x4000 },      { "Z", 0x2000 },      { "START", 0x1000 },  { "DUP", 0x0800 },
    { "DDOWN", 0x0400 },  { "DLEFT", 0x0200 },  { "DRIGHT", 0x0100 }, { "L", 0x0020 },      { "R", 0x0010 },
    { "CUP", 0x0008 },    { "CDOWN", 0x0004 },  { "CLEFT", 0x0002 },  { "CRIGHT", 0x0001 },
};

uint16_t ButtonMask(const json& j) {
    if (j.is_null())
        return 0;
    if (j.is_number_integer())
        return (uint16_t)j.get<int>();
    std::vector<std::string> names;
    if (j.is_string())
        names.push_back(j.get<std::string>());
    else if (j.is_array())
        for (auto& n : j)
            names.push_back(n.get<std::string>());
    else
        throw Error("buttons: a mask, a name or a list of names");
    uint16_t mask = 0;
    for (auto name : names) {
        std::transform(name.begin(), name.end(), name.begin(), ::toupper);
        bool found = false;
        for (auto& b : kButtons)
            if (name == b.name) {
                mask |= b.mask;
                found = true;
            }
        if (!found)
            throw Error("unknown button " + name);
    }
    return mask;
}

int8_t Axis(const json& j) {
    if (j.is_number_float()) /* -1..1 */
        return (int8_t)std::clamp((int)(j.get<double>() * 80), -128, 127);
    return (int8_t)std::clamp(j.get<int>(), -128, 127);
}

json CmdInput(const json& req) {
    std::vector<InputStep> steps;
    const json& list = req.contains("steps") ? req["steps"] : json::array({ req });
    for (auto& s : list) {
        InputStep step;
        step.buttons = ButtonMask(s.value("buttons", json()));
        if (s.contains("stick")) {
            step.set_stick = true;
            step.sx = Axis(s["stick"][0]);
            step.sy = Axis(s["stick"][1]);
        }
        if (s.contains("rstick")) {
            step.set_right = true;
            step.rx = Axis(s["rstick"][0]);
            step.ry = Axis(s["rstick"][1]);
        }
        step.override_pad = s.value("mode", std::string("or")) == "override";
        step.frames = std::clamp<int64_t>(s.value("frames", 1), 1, 100000);
        steps.push_back(step);
    }
    std::lock_guard<std::mutex> lock(gInputLock);
    if (req.value("clear", false)) {
        gInput.clear();
        gInputFrames = 0;
    }
    for (auto& s : steps) {
        gInput.push_back(s);
        gInputFrames += s.frames;
    }
    return { { "queued_frames", gInputFrames } };
}

/* ------------------------------------------------------------------ frame timing */

constexpr double kBuckets[] = { 8, 12, 16, 17, 20, 25, 33.4, 50, 100 }; /* upper bounds, ms */
constexpr int kBucketCount = sizeof(kBuckets) / sizeof(kBuckets[0]) + 1;

struct Perf {
    double since = 0, last = 0;
    uint64_t frames = 0, game_frame_start = 0;
    double sum = 0, max = 0;
    uint64_t max_at_frame = 0;
    uint64_t hist[kBucketCount] = {};
    uint64_t over17 = 0, over20 = 0, over33 = 0, over50 = 0;
    size_t vtx_peak = 0, vtx_cap = 0, uni_peak = 0, uni_cap = 0;
    uint32_t width = 0, height = 0;
    std::vector<float> recent; /* ring of the last 1024 */
    size_t recent_at = 0;
};
std::mutex gPerfLock;
Perf gPerf;

void ResetPerf(Perf& p) {
    double last = p.last;
    p = Perf();
    p.last = last;
    p.since = Now();
    p.game_frame_start = gFrame;
}

json CmdPerf(const json& req) {
    std::lock_guard<std::mutex> lock(gPerfLock);
    Perf& p = gPerf;
    json out;
    const double span = Now() - p.since;
    std::vector<float> sorted(p.recent);
    std::sort(sorted.begin(), sorted.end());
    out["seconds"] = span;
    out["frames"] = p.frames;
    out["game_frames"] = gFrame - p.game_frame_start;
    out["fps"] = span > 0 ? p.frames / span : 0;
    out["ms_avg"] = p.frames ? p.sum / p.frames : 0;
    out["ms_max"] = p.max;
    out["ms_max_game_frame"] = p.max_at_frame;
    out["ms_p99_recent"] = sorted.empty() ? 0.0 : sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
    out["over_17ms"] = p.over17;
    out["over_20ms"] = p.over20;
    out["over_33ms"] = p.over33;
    out["over_50ms"] = p.over50;
    json hist = json::array();
    for (int i = 0; i < kBucketCount; i++)
        hist.push_back({ { "le_ms", i < kBucketCount - 1 ? json(kBuckets[i]) : json("inf") }, { "count", p.hist[i] } });
    out["histogram"] = hist;
    out["rings"] = { { "vertex_peak_kb", p.vtx_peak >> 10 },
                     { "vertex_kb", p.vtx_cap >> 10 },
                     { "uniform_peak_kb", p.uni_peak >> 10 },
                     { "uniform_kb", p.uni_cap >> 10 } };
    out["resolution"] = { p.width, p.height };
    if (req.value("recent", false)) {
        json r = json::array();
        const size_t n = p.recent.size();
        for (size_t i = 0; i < n; i++)
            r.push_back(std::round(p.recent[(p.recent_at + i) % n] * 100) / 100);
        out["recent_ms"] = r;
    }
    if (req.value("reset", false))
        ResetPerf(p);
    return out;
}

/* ------------------------------------------------------------------ screenshots */

std::mutex gShotLock;
std::condition_variable gShotDone;
std::atomic<bool> gShotWanted{ false };
std::atomic<int> gShotMaxWidth{ 1280 };
std::vector<uint8_t> gShotPng;
int gShotW, gShotH;
uint64_t gShotSerial;

std::string Base64(const uint8_t* data, size_t size) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = data[i] << 16 | (i + 1 < size ? data[i + 1] << 8 : 0) | (i + 2 < size ? data[i + 2] : 0);
        out += t[v >> 18 & 63];
        out += t[v >> 12 & 63];
        out += i + 1 < size ? t[v >> 6 & 63] : '=';
        out += i + 2 < size ? t[v & 63] : '=';
    }
    return out;
}

json CmdScreenshot(const json& req) {
    std::unique_lock<std::mutex> lock(gShotLock);
    const uint64_t serial = gShotSerial;
    gShotMaxWidth = std::clamp(req.value("max_width", 1280), 0, 8192);
    gShotWanted = true;
    if (!gShotDone.wait_for(lock, std::chrono::seconds(5), [&] { return gShotSerial != serial; })) {
        gShotWanted = false;
        throw Error("no frame was presented in 5 s");
    }
    json out = { { "width", gShotW }, { "height", gShotH }, { "bytes", gShotPng.size() } };
    if (req.value("save", false) || req.contains("name")) {
        std::string dir = gDataDir + "/devtools";
        mkdir(dir.c_str(), 0777);
        std::string name = req.value("name", std::string());
        if (name.empty()) {
            char stamp[64];
            time_t t = time(nullptr);
            strftime(stamp, sizeof stamp, "shot-%Y%m%d-%H%M%S", gmtime(&t));
            name = std::string(stamp) + "-" + std::to_string(serial);
        }
        for (char& c : name)
            if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.')
                c = '_';
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".png") != 0)
            name += ".png";
        std::string path = dir + "/" + name;
        FILE* f = fopen(path.c_str(), "wb");
        if (!f)
            throw Error("cannot write " + path);
        fwrite(gShotPng.data(), 1, gShotPng.size(), f);
        fclose(f);
        chmod(path.c_str(), 0666);
        out["path"] = path;
    }
    if (req.value("inline", true))
        out["png_base64"] = Base64(gShotPng.data(), gShotPng.size());
    return out;
}

/* ------------------------------------------------------------------ memory */

bool Accessible(uintptr_t addr, size_t size, bool write) {
    uintptr_t at = addr;
    while (at < addr + size) {
        void *s, *e;
        int prot = 0;
        if (at < 0x10000 || sceKernelQueryMemoryProtection((void*)at, &s, &e, &prot) != 0)
            return false;
        if (!(prot & 1) || (write && !(prot & 2)) || (uintptr_t)e <= at)
            return false;
        at = (uintptr_t)e;
    }
    return true;
}

uintptr_t Address(const json& req) {
    uintptr_t addr = 0;
    if (req.contains("global")) {
        json g = Globals();
        std::string name = req["global"];
        if (!g.contains(name))
            throw Error("unknown global " + name + " (have: " + g.dump() + ")");
        addr = g[name].get<uint64_t>();
    } else if (req.contains("eboot")) { /* an offset into the ELF, as llvm-nm prints it */
        addr = ps5_image_base() + req["eboot"].get<uint64_t>();
    } else if (req.contains("addr")) {
        const json& a = req["addr"];
        addr = a.is_string() ? strtoull(a.get<std::string>().c_str(), nullptr, 0) : a.get<uint64_t>();
    } else {
        throw Error("give addr, eboot (offset from the image base) or global");
    }
    return addr + req.value("offset", (int64_t)0);
}

std::string Hex(const uint8_t* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

json CmdMemRead(const json& req) {
    const uintptr_t addr = Address(req);
    const size_t len = req.value("len", (size_t)16);
    if (len == 0 || len > kMaxMem)
        throw Error("len: 1 to 65536");
    return OnGameThread([=]() -> json {
        if (!Accessible(addr, len, false))
            throw Error("not readable");
        return { { "addr", addr }, { "len", len }, { "hex", Hex((const uint8_t*)addr, len) } };
    });
}

json CmdMemWrite(const json& req) {
    const uintptr_t addr = Address(req);
    std::string hex = req.value("hex", std::string());
    if (hex.empty() || hex.size() % 2 || hex.size() / 2 > kMaxMem)
        throw Error("hex: an even number of hex digits, at most 64 KB");
    std::vector<uint8_t> bytes(hex.size() / 2);
    for (size_t i = 0; i < bytes.size(); i++) {
        char* end;
        std::string b = hex.substr(i * 2, 2);
        bytes[i] = (uint8_t)strtoul(b.c_str(), &end, 16);
        if (*end)
            throw Error("hex: not a hex digit in " + b);
    }
    return OnGameThread([=]() -> json {
        if (!Accessible(addr, bytes.size(), true))
            throw Error("not writable");
        memcpy((void*)addr, bytes.data(), bytes.size());
        return { { "addr", addr }, { "written", bytes.size() } };
    });
}

/* ------------------------------------------------------------------ waiting */

bool Matches(const Snapshot& s, const json& c) {
    if (c.contains("state") && s.state != c["state"].get<std::string>())
        return false;
    if (c.contains("scene") && s.scene != c["scene"].get<int>())
        return false;
    if (c.contains("room") && s.room != c["room"].get<int>())
        return false;
    if (c.contains("entrance") && s.entrance != c["entrance"].get<int>())
        return false;
    if (c.contains("loaded") && s.loaded != c["loaded"].get<bool>())
        return false;
    if (c.contains("state_frames") && s.state_frames < c["state_frames"].get<uint32_t>())
        return false;
    if (c.value("input_idle", false) && s.input_frames != 0)
        return false;
    return true;
}

json CmdWait(const json& req) {
    const double timeout = std::clamp(req.value("timeout", 30.0), 0.0, 600.0);
    const double start = Now();
    const double seconds = req.value("seconds", 0.0);
    std::unique_lock<std::mutex> lock(gSnapLock);
    const uint64_t first = gSnap.frame;
    const uint64_t frames = req.value("frames", (uint64_t)0);
    for (;;) {
        const double now = Now();
        const bool fresh = gSnap.frame > first; /* conditions are read from a frame after the request */
        if (fresh && gSnap.frame - first >= frames && now - start >= seconds && Matches(gSnap, req))
            return { { "elapsed", now - start }, { "frames", gSnap.frame - first }, { "snapshot", SnapshotJson(gSnap) } };
        if (now - start >= timeout) {
            json e = { { "error", "timeout" }, { "elapsed", now - start }, { "snapshot", SnapshotJson(gSnap) } };
            throw Error(e.dump());
        }
        gSnapChanged.wait_for(lock, std::chrono::milliseconds(20));
    }
}

/* ------------------------------------------------------------------ the rest */

json CmdVersion() {
    std::string build = Trim(ReadFile("/app0/build_id.txt", 512));
    std::string param = ReadFile("/app0/sce_sys/param.json");
    std::string version = "?";
    size_t at = param.find("\"contentVersion\"");
    if (at != std::string::npos) {
        size_t q1 = param.find('"', param.find(':', at) + 1);
        size_t q2 = param.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos)
            version = param.substr(q1 + 1, q2 - q1 - 1);
    }
    return { { "protocol", kProtocol },
             { "build", build.empty() ? "unknown" : build },
             { "content_version", version },
             { "image_base", ps5_image_base() },
             { "data_dir", gDataDir },
             { "port", gPort },
             { "game_frame", gFrame } };
}

json CmdLog(const json& req) {
    const int lines = std::clamp(req.value("lines", 50), 1, 2000);
    std::string path = gDataDir + "/logs/Ship of Harkinian.log";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        throw Error("no " + path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = std::max(0L, size - (long)lines * 400);
    fseek(f, from, SEEK_SET);
    std::string text(size - from, '\0');
    text.resize(fread(&text[0], 1, text.size(), f));
    fclose(f);
    std::vector<std::string> all;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos)
            nl = text.size();
        all.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (from > 0 && !all.empty())
        all.erase(all.begin()); /* a partial first line */
    if ((int)all.size() > lines)
        all.erase(all.begin(), all.end() - lines);
    return { { "lines", all } };
}

json Dispatch(const json& req, bool& authed) {
    const std::string cmd = req.value("cmd", std::string());
    if (cmd == "auth") {
        if (gToken.empty() || req.value("token", std::string()) == gToken) {
            authed = true;
            return json::object();
        }
        throw Error("wrong token");
    }
    if (!authed)
        throw Error("auth first: {\"cmd\":\"auth\",\"token\":...}");
    if (cmd == "ping")
        return { { "pong", true }, { "game_frame", gFrame } };
    if (cmd == "version")
        return CmdVersion();
    if (cmd == "snapshot") {
        std::lock_guard<std::mutex> lock(gSnapLock);
        return SnapshotJson(gSnap);
    }
    if (cmd == "input")
        return CmdInput(req);
    if (cmd == "input_clear") {
        std::lock_guard<std::mutex> lock(gInputLock);
        gInput.clear();
        gInputFrames = 0;
        return json::object();
    }
    if (cmd == "perf")
        return CmdPerf(req);
    if (cmd == "screenshot")
        return CmdScreenshot(req);
    if (cmd == "wait")
        return CmdWait(req);
    if (cmd == "log")
        return CmdLog(req);
    if (cmd == "mem_read")
        return CmdMemRead(req);
    if (cmd == "mem_write")
        return CmdMemWrite(req);
    if (cmd == "globals")
        return Globals();
    if (cmd == "state")
        return OnGameThread([&] { return State(req); });
    if (cmd == "flags")
        return OnGameThread([&] { return Flags(req); });
    if (cmd == "warp")
        return OnGameThread([&] { return Warp(req); });
    if (cmd == "cvar")
        return OnGameThread([&] { return CVar(req); });
    if (cmd == "save")
        return OnGameThread([&] { return Save(req); });
    if (cmd == "load")
        return OnGameThread([&] { return Load(req); });
    if (cmd == "console")
        return OnGameThread([&] { return Console(req); });
    if (cmd == "crash") { /* tests the crash handler: the game dies */
        if (!req.value("confirm", false))
            throw Error("crash ends the game: add \"confirm\": true");
        OnGameThread([]() -> json {
            say("devtools: crashing on request");
            *(volatile int*)(uintptr_t)8 = 1;
            return json::object();
        });
        return json::object();
    }
    throw Error("unknown cmd \"" + cmd + "\"");
}

/* ------------------------------------------------------------------ sockets */

bool PrivatePeer(const sockaddr_in& a) {
    const uint32_t ip = ntohl(a.sin_addr.s_addr);
    return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == (172 << 4 | 1) || (ip >> 16) == (192 << 8 | 168) ||
           (ip >> 22) == ((100 << 2) | 1) /* 100.64/10 */ || (ip >> 16) == (169 << 8 | 254);
}

/* no resolver or inet_* in the title's libc: dotted quads by hand */
bool ParseIp(const std::string& text, in_addr_t* out) {
    unsigned a, b, c, d;
    char extra;
    if (sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    *out = htonl(a << 24 | b << 16 | c << 8 | d);
    return true;
}

std::string IpText(in_addr_t addr) {
    const uint32_t ip = ntohl(addr);
    char buf[20];
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
    return buf;
}

bool SendAll(int fd, const std::string& s) {
    size_t at = 0;
    while (at < s.size()) {
        ssize_t n = send(fd, s.data() + at, s.size() - at, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        at += n;
    }
    return true;
}

/* One request line in, one reply line out (with its newline): every transport's core */
std::string HandleLine(const std::string& line, bool& authed) {
    json reply;
    json id;
    try {
        json req = json::parse(line);
        if (!req.is_object())
            throw Error("a request is a JSON object");
        id = req.value("id", json());
        reply = Dispatch(req, authed);
        if (!reply.is_object())
            reply = { { "value", reply } };
        reply["ok"] = true;
    } catch (const std::exception& e) {
        reply = { { "ok", false } };
        json detail = json::parse(e.what(), nullptr, false);
        if (detail.is_object() && detail.contains("error"))
            reply.update(detail);
        else
            reply["error"] = e.what();
    }
    if (!id.is_null())
        reply["id"] = id;
    return reply.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
}

/* A connected socket, either way round (accepted, or dialled out): lines until it closes */
void Serve(int fd, std::string peer) {
    Status("client %s connected", peer.c_str());
    bool authed = gToken.empty();
    std::string buf;
    char chunk[16384];
    for (;;) {
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = Trim(buf.substr(0, nl));
            buf.erase(0, nl + 1);
            if (line.empty())
                continue;
            if (!SendAll(fd, HandleLine(line, authed)))
                goto out;
        }
        if (buf.size() > kMaxLine)
            break;
        ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n < 0)
                Status("client %s: recv errno %d (%s)", peer.c_str(), errno, strerror(errno));
            break;
        }
        buf.append(chunk, n);
    }
out:
    close(fd);
    Status("client %s gone", peer.c_str());
}

/* (a) the title listens */
void Listen() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        Status("listen: socket() failed, errno %d (%s)", errno, strerror(errno));
        return;
    }
    Status("listen: socket() = %d", s);
    int one = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0)
        Status("listen: SO_REUSEADDR errno %d (%s)", errno, strerror(errno));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(gPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0) {
        Status("listen: bind(0.0.0.0:%d) failed, errno %d (%s)", gPort, errno, strerror(errno));
        close(s);
        return;
    }
    Status("listen: bind(0.0.0.0:%d) ok", gPort);
    if (listen(s, 4) != 0) {
        Status("listen: listen() failed, errno %d (%s)", errno, strerror(errno));
        close(s);
        return;
    }
    Status("listen: debug server on TCP %d (private addresses only%s)", gPort, gToken.empty() ? "" : ", token required");
    int failures = 0;
    for (;;) {
        sockaddr_in peer = {};
        socklen_t len = sizeof peer;
        int c = accept(s, (sockaddr*)&peer, &len);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (failures++ < 5)
                Status("listen: accept() failed, errno %d (%s)", errno, strerror(errno));
            sleep(1);
            continue;
        }
        const std::string ip = IpText(peer.sin_addr.s_addr);
        if (!PrivatePeer(peer)) {
            Status("listen: refused %s (not a private address)", ip.c_str());
            close(c);
            continue;
        }
        if (gClients >= kMaxClients) {
            SendAll(c, "{\"ok\":false,\"error\":\"too many clients\"}\n");
            close(c);
            continue;
        }
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        gClients++;
        std::thread([c, ip, peer] {
            Serve(c, std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port)));
            gClients--;
        }).detach();
    }
}

/* (b) the title dials out to connect=HOST:PORT (the test machine listens), and again
 * whenever that connection ends */
void ConnectOut() {
    const size_t colon = gConnect.rfind(':');
    const std::string host = gConnect.substr(0, colon);
    const std::string port = colon == std::string::npos ? "9201" : gConnect.substr(colon + 1);
    int logged = 0;
    for (;;) {
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(atoi(port.c_str()));
        if (!ParseIp(host, &to.sin_addr.s_addr)) {
            Status("connect: %s is not an IPv4 address (connect=A.B.C.D:PORT)", host.c_str());
            return;
        }
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            Status("connect: socket() failed, errno %d (%s)", errno, strerror(errno));
            return;
        }
        if (connect(s, (sockaddr*)&to, sizeof to) != 0) {
            if (logged++ < 5)
                Status("connect: connect(%s:%s) failed, errno %d (%s)", host.c_str(), port.c_str(), errno,
                       strerror(errno));
            close(s);
            sleep(3);
            continue;
        }
        logged = 0;
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        Status("connect: connected to %s:%s", host.c_str(), port.c_str());
        Serve(s, "out:" + gConnect);
        sleep(1);
    }
}

/* (c) files: <data>/devtools/mailbox/in/NAME.req (request lines; write NAME.tmp and
 * rename it) gets out/NAME.resp (the reply lines, renamed into place when complete), and
 * the request is deleted. For when no socket gets through; FTP carries the files. */
void Mailbox() {
    const std::string base = gDataDir + "/devtools/mailbox";
    const std::string in = base + "/in", out = base + "/out";
    mkdir((gDataDir + "/devtools").c_str(), 0777);
    mkdir(base.c_str(), 0777);
    mkdir(in.c_str(), 0777);
    mkdir(out.c_str(), 0777);
    chmod(base.c_str(), 0777);
    chmod(in.c_str(), 0777);
    chmod(out.c_str(), 0777);
    Status("mailbox: watching %s", in.c_str());
    for (;;) {
        std::vector<std::string> names;
        if (DIR* d = opendir(in.c_str())) {
            while (dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".req") == 0)
                    names.push_back(n);
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        for (auto& n : names) {
            const std::string path = in + "/" + n;
            std::string text = ReadFile(path, kMaxLine);
            unlink(path.c_str());
            bool authed = gToken.empty();
            std::string replies;
            size_t pos = 0;
            while (pos < text.size()) {
                size_t nl = text.find('\n', pos);
                std::string line = Trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
                pos = nl == std::string::npos ? text.size() : nl + 1;
                if (!line.empty())
                    replies += HandleLine(line, authed);
            }
            const std::string stem = n.substr(0, n.size() - 4);
            const std::string tmp = out + "/" + stem + ".part", done = out + "/" + stem + ".resp";
            if (FILE* f = fopen(tmp.c_str(), "wb")) {
                fwrite(replies.data(), 1, replies.size(), f);
                fclose(f);
                chmod(tmp.c_str(), 0666);
                rename(tmp.c_str(), done.c_str());
            }
        }
        usleep(50 * 1000);
    }
}

} // namespace

json SnapshotJson(const Snapshot& s) {
    return { { "frame", s.frame },
             { "state", s.state },
             { "state_frames", s.state_frames },
             { "scene", s.scene },
             { "room", s.room },
             { "entrance", s.entrance },
             { "next_entrance", s.next_entrance },
             { "transition", { s.transition_trigger, s.transition_mode } },
             { "loaded", s.loaded },
             { "pos", { s.pos[0], s.pos[1], s.pos[2] } },
             { "yaw", s.yaw },
             { "age", s.age ? "child" : "adult" },
             { "health", s.health },
             { "health_max", s.health_max },
             { "file", s.file },
             { "game_mode", s.game_mode },
             { "gameplay_frames", s.gameplay_frames },
             { "input_frames", s.input_frames } };
}

} // namespace devtools

using namespace devtools;

extern "C" void ps5_devtools_start(void) {
    gDataDir = ps5_data_dir();
    const std::string flag = gDataDir + "/devtools.enable";
    struct stat st;
    if (stat(flag.c_str(), &st) != 0) {
        say("devtools: built in, off (no %s)", flag.c_str()); /* and nothing listens */
        return;
    }
    std::string conf = ReadFile(flag, 4096);
    size_t pos = 0;
    while (pos < conf.size()) {
        size_t nl = conf.find('\n', pos);
        std::string line = Trim(conf.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? conf.size() : nl + 1;
        if (line.rfind("port=", 0) == 0)
            gPort = std::clamp(atoi(line.c_str() + 5), 1024, 65535);
        else if (line.rfind("token=", 0) == 0)
            gToken = line.substr(6);
        else if (line.rfind("connect=", 0) == 0)
            gConnect = line.substr(8);
        else if (line.rfind("transport=", 0) == 0) { /* any of listen,connect,mailbox */
            const std::string t = line.substr(10);
            gUseListen = t.find("listen") != std::string::npos;
            gUseConnect = t.find("connect") != std::string::npos;
            gUseMailbox = t.find("mailbox") != std::string::npos;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    {
        std::lock_guard<std::mutex> lock(gPerfLock);
        ResetPerf(gPerf);
    }
    gRunning = true;
    Status("starting (build %s): listen %s, connect %s, mailbox %s", Trim(ReadFile("/app0/build_id.txt", 256)).c_str(),
           gUseListen ? "on" : "off", gUseConnect && !gConnect.empty() ? gConnect.c_str() : "off",
           gUseMailbox ? "on" : "off");
    if (gUseListen)
        std::thread(Listen).detach();
    if (gUseConnect && !gConnect.empty())
        std::thread(ConnectOut).detach();
    if (gUseMailbox)
        std::thread(Mailbox).detach();
}

extern "C" void ps5_devtools_frame(void) {
    if (!gRunning)
        return;
    gFrame++;
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lock(gJobLock);
            if (gJobs.empty())
                break;
            job = gJobs.front();
            gJobs.pop_front();
        }
        json result;
        std::string error;
        try {
            result = job->fn();
        } catch (const std::exception& e) {
            error = e.what();
            if (error.empty())
                error = "failed";
        }
        std::lock_guard<std::mutex> lock(gJobLock);
        job->result = std::move(result);
        job->error = std::move(error);
        job->done = true;
        gJobDone.notify_all();
    }
    Snapshot s;
    FillSnapshot(s);
    s.frame = gFrame;
    s.time = Now();
    {
        std::lock_guard<std::mutex> lock(gInputLock);
        s.input_frames = gInputFrames;
    }
    std::lock_guard<std::mutex> lock(gSnapLock);
    gSnap = std::move(s);
    gSnapChanged.notify_all();
}

extern "C" void ps5_devtools_pad(void* pads) {
    if (!gRunning)
        return;
    std::lock_guard<std::mutex> lock(gInputLock);
    if (gInput.empty())
        return;
    InputStep& s = gInput.front();
    ApplyPad(pads, s.buttons, s.sx, s.sy, s.rx, s.ry, s.override_pad, s.set_stick, s.set_right);
    gInputFrames--;
    if (--s.frames == 0)
        gInput.pop_front();
}

extern "C" void ps5_devtools_present(uint32_t width, uint32_t height, size_t vertex_peak, size_t vertex_cap,
                                     size_t uniform_peak, size_t uniform_cap) {
    if (!gRunning)
        return;
    const double now = Now();
    std::lock_guard<std::mutex> lock(gPerfLock);
    Perf& p = gPerf;
    if (p.last > 0 && p.last >= p.since) {
        const double ms = (now - p.last) * 1000.0;
        p.frames++;
        p.sum += ms;
        if (ms > p.max) {
            p.max = ms;
            p.max_at_frame = gFrame;
        }
        int b = 0;
        while (b < kBucketCount - 1 && ms > kBuckets[b])
            b++;
        p.hist[b]++;
        p.over17 += ms > 17.0;
        p.over20 += ms > 20.0;
        p.over33 += ms > 33.4;
        p.over50 += ms > 50.0;
        if (p.recent.size() < 1024)
            p.recent.push_back((float)ms);
        else {
            p.recent[p.recent_at] = (float)ms;
            p.recent_at = (p.recent_at + 1) % 1024;
        }
    }
    p.last = now;
    p.vtx_peak = std::max(p.vtx_peak, vertex_peak);
    p.uni_peak = std::max(p.uni_peak, uniform_peak);
    p.vtx_cap = vertex_cap;
    p.uni_cap = uniform_cap;
    p.width = width;
    p.height = height;
}

extern "C" bool ps5_devtools_shot_wanted(void) {
    return gShotWanted.load();
}

extern "C" int ps5_devtools_shot_max_width(void) {
    return gShotMaxWidth.load();
}

extern "C" void ps5_devtools_shot_png(const uint8_t* png, size_t size, int width, int height) {
    std::lock_guard<std::mutex> lock(gShotLock);
    gShotPng.assign(png, png + size);
    gShotW = width;
    gShotH = height;
    gShotSerial++;
    gShotWanted = false;
    gShotDone.notify_all();
}
