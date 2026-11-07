// SPDX-License-Identifier: MIT
// Manual pit lane. Port of the F1 hub's "Drive the pit lane yourself" (tools/keepcontrol.py, MODLOG "Manual pit lane").
//
// Every car has a ControllerSet: +0x20 the human controller, +0x28 the AI controller, +0x30 a flags dword. When a
// flag is set the AI drives the car; the game sets PIT_LANE (bit 0) as you enter the pit lane, so the AI drives you
// through it. This plugin clears PIT_LANE on your car the moment it appears and, if no other flag is set, switches
// the car back to human control (mode 0; the mode dword is stored xor-ed with a key dword next to it).
// No hooks and no bindings: the car table and the flag names are found in the game's code at run time.
//
// Pit stop "manual": the game's own manual pit stop (feature manual_pit_stops, bit 34 of the
// feature bitset the getter at +0x13c91e0 returns) for your car only. The pit code asks the getter before each bit-34
// test; 22 of those calls are redirected to pit_features(car), which answers "on" only for your vehicle id. The
// global switch stays off, so the session loads normally (with the pit crew) and the AI keeps normal stops.
// Found 2026-10-22 with a hardware watchpoint on the bitset (memory: manual-pit-stops-research).
//
// Ctrl+F7 in game turns it on and off (two beeps on, one off, and a message in the race notification).
#include <atomic>
#include <cstring>
#include <cmath>
#include <string>
#include <thread>
#include <vector>
#include "kizi_scan.hpp"
#include "osd.hpp"

SWOFTY_SETTING(bool, enabled, true);
SWOFTY_SETTING(bool, osd_message, true);
SWOFTY_SETTING(std::string_view, stop_mode, "manual");
SWOFTY_SETTING(bool, no_qte, true);

namespace {

constexpr uint32_t FLAGS = 0x30;          // ControllerSet flags dword

struct CarTable {
    uintptr_t global;                     // manager pointer
    uint8_t vec, mode, key;               // manager+vec = vector of ControllerSet*; set+mode ^ set+key = mode
};
std::vector<CarTable> g_tables;           // candidates from the code; the live one is picked once a race exists
uint32_t g_pit_bit = 0;

uint64_t q(uintptr_t a)
{
    uint64_t v = 0;
    return kizi::read(a, v) ? v : 0;
}

uint32_t u32(uintptr_t a)
{
    uint32_t v = 0;
    return kizi::read(a, v) ? v : 0;
}

// Vehicle input code: mov rcx,[rip+G] ... mov rcx,[rcx+V]; mov rcx,[rcx+rdx*8]; mov edx,[rcx+M]; xor edx,[rcx+K]
void find_tables(const std::vector<kizi::Range> &code)
{
    kizi::find_all(code, kizi::Pattern("48 8B 49 ?? 48 8B 0C D1 8B 51 ?? 33 51 ??"), [&](uintptr_t a) {
        for (uintptr_t s = a - 8; s >= a - 7 - 24; s--) {             // nearest `mov rcx,[rip+G]` 1..24 bytes before
            if (kizi::Pattern("48 8B 0D").at(reinterpret_cast<const uint8_t *>(s))) {
                CarTable t{s + 7 + kizi::rel32(s + 3), *reinterpret_cast<uint8_t *>(a + 3),
                           *reinterpret_cast<uint8_t *>(a + 10), *reinterpret_cast<uint8_t *>(a + 13)};
                bool dup = false;
                for (auto &o : g_tables)
                    dup |= o.global == t.global && o.vec == t.vec && o.mode == t.mode && o.key == t.key;
                if (!dup && kizi::in_image(t.global))
                    g_tables.push_back(t);
                break;
            }
        }
        return true;
    });
}

// ControllerSet flag names: a pointer array whose entry N names bit N; check that PIT_LANE is where we expect.
void find_pit_bit(const std::vector<kizi::Range> &data)
{
    uintptr_t first = kizi::find_string(data, "PIT_LANE");
    if (!first)
        return;
    for (auto r : data)
        for (uintptr_t a = (r.a + 7) & ~uintptr_t(7); a + 8 <= r.b; a += 8) {
            if (*reinterpret_cast<const uint64_t *>(a) != first)
                continue;
            for (uintptr_t b = a; b >= r.a && b + 8 * 32 > a; b -= 8) {    // the table may start before PIT_LANE
                std::vector<std::string> names;
                for (int i = 0; i < 32 && b + 8 * i + 8 <= r.b; i++) {
                    std::string s = kizi::image_str(*reinterpret_cast<const uint64_t *>(b + 8 * i), 32);
                    if (s.size() < 2 || s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ_") != std::string::npos)
                        break;
                    names.push_back(s);
                }
                bool td = false;
                for (auto &n : names)
                    td |= n == "TERMINAL_DAMAGE";
                if (td && names.size() >= 10)
                    for (size_t i = 0; i < names.size(); i++)
                        if (names[i] == "PIT_LANE") {
                            g_pit_bit = uint32_t(i);
                            swofty::log("[pitlane] %zu controller flags; PIT_LANE = bit %u", names.size(), g_pit_bit);
                            return;
                        }
                if (b == r.a)
                    break;
            }
        }
    swofty::log("[pitlane] flag name table not found; using PIT_LANE = bit 0");
}

std::vector<uintptr_t> car_sets(CarTable &t)
{
    std::vector<uintptr_t> out;
    uintptr_t mgr = q(t.global);
    uint64_t b = mgr ? q(mgr + t.vec) : 0, e = mgr ? q(mgr + t.vec + 8) : 0;
    if (!b || e <= b || (e - b) / 8 > 64 || (e - b) / 8 < 8)
        return out;
    for (uint64_t p = b; p < e; p += 8)
        out.push_back(q(p));                         // position = vehicle id; empty slots stay 0
    return out;
}

// Your car (its position in the table is its vehicle id): its human controller's vtable differs from the AI one
// every car shares.
uintptr_t player(const std::vector<uintptr_t> &sets, int &vid)
{
    vid = -1;
    std::vector<std::pair<uint64_t, int>> ai;
    for (auto s : sets)
        if (uint64_t v = s ? q(q(s + 0x28)) : 0) {
            bool f = false;
            for (auto &x : ai)
                if (x.first == v)
                    x.second++, f = true;
            if (!f)
                ai.push_back({v, 1});
        }
    if (ai.empty())
        return 0;
    uint64_t ai_vt = std::max_element(ai.begin(), ai.end(), [](auto &a, auto &b) { return a.second < b.second; })->first;
    uintptr_t found = 0;
    int n = 0;
    for (size_t i = 0; i < sets.size(); i++) {
        uintptr_t s = sets[i];
        uint64_t hp = s ? q(s + 0x20) : 0, ap = s ? q(s + 0x28) : 0;
        uint64_t hv = hp ? q(hp) : 0;
        if (hp && ap && q(ap) == ai_vt && hv && hv != ai_vt && kizi::in_image(hv))
            found = s, vid = int(i), n++;
    }
    if (n != 1)
        vid = -1;
    return n == 1 ? found : 0;                        // one human car only (offline single player)
}

std::atomic<int> g_key_state{-1};                     // -1 = from settings; 0/1 set with Ctrl+F7

bool on()
{
    int k = g_key_state;
    return k >= 0 ? k == 1 : enabled.get();
}

// --- the game's manual pit stop, for your car only ---------------------------------------------------------------
enum class Src { STACK50, BL, BPL, R13, R12_10A, SHARED };
struct Site {
    const char *pattern;                  // the code around the `call getter`, rel32 / rip displacements wildcarded
    int at;                               // offset of the call in the pattern
    Src src;                              // where that code keeps the vehicle id it is handling
};
// Each pattern matches exactly once in the game's code (made on build 1783424274, comment = its rva there).
// Main pit loop: vehicle id at [rsp+50h]; per-car functions: in bl or bpl (the ones that decide to hand a car to the
// AI for its stop, call +0x3894c40(car, 1) there); one loop over all ids: r13; +0x34afb2b: the car object in r12 (id
// at +10Ah); +0x34b0b05 fetches the pointer once before the loop (it then reads the last car's answer).
// Left alone: +0x13c94d9/+0x13c9d49 read the switch directly (session setup, e.g. the pit crew), +0x34b4d70 (no car),
// +0x35c1876 (HUD text).
const Site SITES[] = {
    {"C0 74 A9 80 B8 03 01 00 00 00 75 92 E8 ?? ?? ?? ?? 48 8B 4C 24 60 4C 8B 31", 12, Src::STACK50},  // +34b0c80 (1)
    {"84 3C 2B 00 00 48 8B 9E E8 0A 00 00 E8 ?? ?? ?? ?? 48 85 38 0F 85 ?? ?? ?? ??", 12, Src::STACK50},  // +34b0ca2 (1)
    {"00 00 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 49 89 C1 48 8B 44 24 60", 12, Src::STACK50},  // +34b0d36 (1)
    {"D2 01 C4 E3 79 4A C1 20 E9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 89 C5 48 8B 44 24 60", 13, Src::STACK50},  // +34b0edb (1)
    {"8B 68 58 48 85 ED 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 38 0F 85 ?? ?? ?? ??", 12, Src::STACK50},  // +34b10fc (1)
    {"44 89 E2 C5 F8 28 D6 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 38 75 0B 48 89 F1", 12, Src::STACK50},  // +34b1263 (1)
    {"74 0A 48 89 C1 B2 1F E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 38 74 0F 48 8B 44 24 60", 12, Src::STACK50},  // +34b1600 (1)
    {"05 01 00 00 01 48 8B 9E E8 0A 00 00 E8 ?? ?? ?? ?? 48 85 38 0F 85 ?? ?? ?? ??", 12, Src::STACK50},  // +34b1620 (1)
    {"00 00 45 89 C6 48 8B 9E E8 0A 00 00 E8 ?? ?? ?? ?? 48 BF 00 00 00 00 04 00 00 00", 12, Src::STACK50},  // +34b1eff (1)
    {"BB 00 03 00 00 00 0F 85 5F 0B 00 00 E8 ?? ?? ?? ?? 48 85 38 0F 85 ?? ?? ?? ??", 12, Src::STACK50},  // +34b2fc0 (1)
    {"00 0F 85 30 0B 00 00 48 8B 4C 24 50 48 8B 04 C8 48 83 78 20 00 0F 84 ?? ?? ?? ?? 80 BB 00 03 00 00 00 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 38 0F 85 ?? ?? ?? ?? 48 8B 44 24 58 48 01 D8 48 83 C0 68 48 8B 00 80 78 11 02", 40, Src::STACK50},  // +34b3127 (1)
    {"20 00 00 EB 48 48 8B 9E E8 0A 00 00 E8 ?? ?? ?? ?? 48 85 38 75 37 48 8B 44 24 58", 12, Src::STACK50},  // +34b37eb (1)
    {"00 00 49 89 DE 48 8B 9E E8 0A 00 00 E8 ?? ?? ?? ?? 48 85 38 75 40 48 8B 44 24 58", 12, Src::STACK50},  // +34b38c9 (1)
    {"0F 82 ?? ?? ?? ?? 48 89 DD 48 8B 9E E8 0A 00 00 E8 ?? ?? ?? ?? 48 85 38 0F 85 ?? ?? ?? ??", 16, Src::STACK50},  // +34b3bbd (1)
    {"89 DA 41 B8 0E 00 00 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 00 80 FB 1A 77 77", 13, Src::BL},  // +34c41bc (1)
    {"FE 83 F8 0E 0F 85 B9 00 00 00 48 8B 8E B0 02 00 00 48 8B 41 30 48 8B 49 38 48 29 C1 48 C1 F9 03 48 39 F9 0F 86 ?? ?? ?? ?? 48 8B 04 F8 48 83 78 20 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 00 80 FB 1A 77 77 48 B9 00 00 00 00 04 00 00 00 48 21 C1 75 68 4A 8D 04 36 48 83 C0 68 48 8B 00 80 78 11 02", 56, Src::BL},  // +34c4416 (1)
    {"48 89 F1 89 DA E8 ?? ?? ?? ?? 48 8B 8E B0 02 00 00 48 8B 41 30 48 8B 49 38 48 29 C1 48 C1 F9 03 48 39 F9 0F 86 ?? ?? ?? ?? 48 8B 04 F8 48 83 78 20 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 00 80 FB 1A 77 77 48 B9 00 00 00 00 04 00 00 00 48 21 C1 75 68 4A 8D 04 36 48 83 C0 68 48 8B 00 80 78 11 02", 56, Src::BL},  // +34c862c (1)
    {"04 F0 48 83 78 20 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 00 80 FB 1A 77 73", 13, Src::BL},  // +34c8935 (1)
    {"00 4A 8B 44 36 68 80 78 08 00 74 7E E8 ?? ?? ?? ?? F6 40 04 04 75 73 4A 8D 04 36", 12, Src::BL},  // +34c8dbf (1)
    {"52 48 8B 04 F8 48 83 78 20 00 74 47 E8 ?? ?? ?? ?? F6 40 04 04 75 3C 4A 8D 04 36", 12, Src::BL},  // +34b7edc (1)
    {"09 00 00 80 B8 05 01 00 00 00 74 3D E8 ?? ?? ?? ?? F6 40 04 04 75 32 4B 8B 44 3E 68", 12, Src::BL},  // +325864c (1)
    {"49 C1 E7 04 4A 8B 44 38 68 8B 78 04 E8 ?? ?? ?? ?? 48 85 18 75 13 83 FF 0F", 12, Src::R13},  // +34c4c03 (1)
    {"89 F2 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 89 84 24 10 02 00 00", 12, Src::SHARED},  // +34b0b05 (1)
    {"24 40 40 88 6C 24 50 66 C7 44 24 52 03 00 48 89 7C 24 58 48 8B 48 08 48 8D 54 24 40 E8 ?? ?? ?? ?? C6 47 19 00 C5 78 29 07 48 C7 47 10 00 00 00 00 4C 8B B6 68 09 00 00 E8 ?? ?? ?? ?? 48 8B 00 40 80 FD 1A 0F 87 ?? ?? ?? ?? 48 89 C1 48 BA 00 00 00 00 04 00 00 00 48 21 D1 0F 85 ?? ?? ?? ??", 56, Src::BPL},  // +1031eec (1)
    {"24 40 40 88 6C 24 50 66 C7 44 24 52 08 00 48 89 7C 24 58 48 8B 48 08 48 8D 54 24 40 E8 ?? ?? ?? ?? C6 47 19 00 C5 78 29 07 48 C7 47 10 00 00 00 00 4C 8B B6 68 09 00 00 E8 ?? ?? ?? ?? 48 8B 00 40 80 FD 1A 0F 87 ?? ?? ?? ?? 48 89 C1 48 BA 00 00 00 00 04 00 00 00 48 21 D1 0F 85 ?? ?? ?? ??", 56, Src::BPL},  // +1032050 (1)
    {"C0 00 00 00 01 4C 8B AE 68 09 00 00 E8 ?? ?? ?? ?? 48 8B 00 40 80 FD 1A", 12, Src::BPL},  // +1032291 (1)
    {"24 40 40 88 6C 24 50 66 C7 44 24 52 07 00 48 89 7C 24 58 48 8B 48 08 48 8D 54 24 40 E8 ?? ?? ?? ?? C6 47 19 00 C5 78 29 07 48 C7 47 10 00 00 00 00 4C 8B B6 68 09 00 00 E8 ?? ?? ?? ?? 48 8B 00 40 80 FD 1A 0F 87 ?? ?? ?? ?? 48 89 C1 48 BA 00 00 00 00 04 00 00 00 48 21 D1 0F 85 ?? ?? ?? ??", 56, Src::BPL},  // +1032671 (1)
    {"84 AF 00 00 00 4C 8B B6 68 09 00 00 E8 ?? ?? ?? ?? 48 8B 00 40 80 FD 1A", 12, Src::BPL},  // +1032738 (1)
    {"59 B7 38 02 45 89 E8 ?? ?? ?? ?? FD E8 ?? ?? ?? ?? F6 40 04 04 75 21 45 85 ED", 12, Src::R12_10A},  // +34afb2b (1)
};
constexpr int MANUAL_BIT = 34;

using Getter = const uint64_t *(*)();
Getter g_getter;
std::atomic<int> g_player_vid{-1};
std::atomic<bool> g_manual{false};       // addon on and pit stop mode "manual"
uint64_t g_copy[2];
// kizi.ai-drive (AI Drives Your Car) signals this event while the AI races your car.
bool ai_drives()
{
    static HANDLE ev = nullptr;
    if (!ev)
        ev = OpenEventA(SYNCHRONIZE, FALSE, "kizi.ai-drive.active");
    return ev && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;
}

bool g_last;                              // the answer for the car the pit code handles now

// Called by the redirected pit code instead of the getter: the game's features, with manual_pit_stops on only for
// your car.
const uint64_t *pit_features(uint64_t car)
{
    const uint64_t *real = g_getter();
    g_copy[0] = real[0], g_copy[1] = real[1];
    if (car != 0xff) {
        int p = g_player_vid;
        g_last = g_manual && p >= 0 && int(car & 0xff) == p;
    }
    if (g_last)
        g_copy[0] |= 1ull << MANUAL_BIT;
    return g_copy;
}

bool install_manual()
{
    auto code = kizi::image_ranges(true);
    constexpr size_t N = sizeof SITES / sizeof SITES[0];
    uintptr_t at[N] = {}, getter = 0;
    for (size_t i = 0; i < N; i++) {
        int hits = 0;
        kizi::find_all(code, kizi::Pattern(SITES[i].pattern), [&](uintptr_t a) {
            at[i] = a + SITES[i].at;
            return ++hits < 2;
        });
        uint8_t op = 0;
        int32_t rel = 0;
        uintptr_t tgt = hits == 1 && kizi::read(at[i], op) && op == 0xE8 && kizi::read(at[i] + 1, rel) ? at[i] + 5 + rel : 0;
        if (!tgt || (getter && tgt != getter)) {
            swofty::log("[pitlane] manual pit stop: pit check %zu of %zu not found (%d match(es)) - not installed (game "
                        "update?)", i + 1, N, hits);
            return false;
        }
        getter = tgt;
    }
    uint8_t *mem = kizi::alloc_near(at[0], 4096);
    if (!mem)
        return false;
    g_getter = reinterpret_cast<Getter>(getter);
    static const uint8_t STACK50[] = {0x48, 0x8B, 0x4C, 0x24, 0x58};   // mov rcx,[rsp+58h] (the caller's [rsp+50h])
    static const uint8_t BL[] = {0x0F, 0xB6, 0xCB};                    // movzx ecx,bl
    static const uint8_t BPL[] = {0x40, 0x0F, 0xB6, 0xCD};             // movzx ecx,bpl
    static const uint8_t R13[] = {0x4C, 0x89, 0xE9};                   // mov rcx,r13
    static const uint8_t R12_10A[] = {0x41, 0x0F, 0xB6, 0x8C, 0x24, 0x0A, 0x01, 0x00, 0x00}; // movzx ecx,byte [r12+10Ah]
    static const uint8_t SHARED[] = {0xB9, 0xFF, 0x00, 0x00, 0x00};    // mov ecx,0FFh
    static const uint8_t JMP[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}; // jmp [rip+0]
    uint64_t target = reinterpret_cast<uint64_t>(&pit_features);
    uint8_t *p = mem;
    for (size_t i = 0; i < N; i++) {
        auto &st = SITES[i];
        uint8_t *stub = p;
        auto put = [&](const uint8_t *b, size_t n) { memcpy(p, b, n), p += n; };
        switch (st.src) {
        case Src::STACK50: put(STACK50, sizeof STACK50); break;
        case Src::BL: put(BL, sizeof BL); break;
        case Src::BPL: put(BPL, sizeof BPL); break;
        case Src::R13: put(R13, sizeof R13); break;
        case Src::R12_10A: put(R12_10A, sizeof R12_10A); break;
        case Src::SHARED: put(SHARED, sizeof SHARED); break;
        }
        put(JMP, sizeof JMP);
        put(reinterpret_cast<const uint8_t *>(&target), 8);
        p = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(p) + 15) & ~uintptr_t(15));
        int32_t r = int32_t(reinterpret_cast<intptr_t>(stub) - intptr_t(at[i] + 5));
        kizi::patch_code(at[i] + 1, &r, 4);
    }
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    swofty::log("[pitlane] manual pit stop: %zu pit checks found and redirected", N);
    return true;
}

// Pit button = the pit stop quick-time event. With the Immersive pit stop setting the game runs a countdown in the pit
// lane (state machine in the player's pit input code: 1 = countdown) and, while it runs, asks for the "PitStop" input
// action (pad A): `lea rdx,[rip+ACTION]; mov r8b,1; call pressed; test al,al; je`. A press in time gives a time
// penalty that is 0 under manual pit stops (feature bit 34, which the redirected checks give your car); the countdown
// running out marks the event missed. The input code makes the action once (lea rcx,[rip+ACTION]; lea rdx,[rip+
// "PitStop"]; call make). That call goes to pressed_hook, which answers "pressed" at once while the window after you
// entered the pit lane is open - so the event is done perfectly the moment it starts, and never asked out on track.
using Pressed = bool (*)(void *, void *, bool);
Pressed g_pressed;
std::atomic<DWORD> g_press_until{0};       // GetTickCount() until which the pit button counts as pressed
std::atomic<int> g_presses{0};             // answers given (logged)
std::atomic<DWORD> g_qte_until{0};         // GetTickCount() until which the quick-time event is removed for your car

bool pressed_hook(void *input, void *action, bool flag)
{
    bool real = g_pressed(input, action, flag);
    if (real || int(g_press_until - GetTickCount()) <= 0)
        return real;
    g_presses++;
    return true;
}

bool install_button(const std::vector<kizi::Range> &code, const std::vector<kizi::Range> &data)
{
    uintptr_t name = kizi::find_string(data, "PitStop"), action = 0, site = 0;
    if (!name)
        return false;
    kizi::find_all(code, kizi::Pattern("48 8D 0D ?? ?? ?? ?? 48 8D 15"), [&](uintptr_t a) {
        if (a + 14 + kizi::rel32(a + 10) != name)
            return true;
        action = a + 7 + kizi::rel32(a + 3);
        return false;
    });
    int hits = 0;
    kizi::find_all(code, kizi::Pattern("48 8D 15 ?? ?? ?? ?? 41 B0 01 E8 ?? ?? ?? ?? 84 C0 0F 84"), [&](uintptr_t a) {
        if (action && a + 7 + kizi::rel32(a + 3) == action)
            site = a + 10, hits++;
        return true;
    });
    if (hits != 1)
        return false;
    g_pressed = reinterpret_cast<Pressed>(site + 5 + kizi::rel32(site + 1));   // set before the call can reach us
    return kizi::redirect_call(site, reinterpret_cast<void *>(&pressed_hook)) != 0;
}

// No quick-time event at all. The countdown starts in one block of the same pit input code: it notes the event in
// the car's record (so it starts once), posts two messages (the event's HUD) and sets the state to 1 (countdown):
//   mov rax,[rip+BUS]; ... mov [rsp+50h],bpl (vehicle id) ... call post ... vmovss [rsp+54h],xmm15;
//   mov dword [rsp+58h],1.0; mov rcx,[rax+8]; mov rdx,r14; call post; mov byte [rdi+19h],1
// The first `mov rax,[rip+BUS]` jumps to a stub: for your car while you are in the pit lane (g_noqte_flag) it goes
// straight past the state write, so no HUD and no countdown; the record stays marked, so it does not start again.
uint8_t *g_noqte_flag, *g_noqte_vid;

bool install_no_qte(const std::vector<kizi::Range> &code)
{
    uintptr_t post2 = 0;
    int hits = 0;
    kizi::find_all(code,
                   kizi::Pattern("C5 7A 11 7C 24 54 C7 44 24 58 00 00 80 3F 48 8B 48 08 4C 89 F2 E8 ?? ?? ?? ?? "
                                 "C6 47 19 01"),
                   [&](uintptr_t a) {
                       post2 = a, hits++;
                       return true;
                   });
    if (hits != 1)
        return false;
    uintptr_t site = post2 - 0x5f, after = post2 + 30;
    const uint8_t *b = reinterpret_cast<const uint8_t *>(site);
    if (!kizi::Pattern("48 8B 05 ?? ?? ?? ?? 48 C7 44 24 48 00 00 00 00 48 8D 0D ?? ?? ?? ?? 48 89 4C 24 40 "
                       "40 88 6C 24 50").at(b))
        return false;
    uint64_t bus = site + 7 + kizi::rel32(site + 3);           // the slot the game's mov reads
    uint8_t *m = kizi::alloc_near(site, 4096);
    if (!m)
        return false;
    uint8_t *flag = m + 0xf00, *vid = m + 0xf01, *p = m;
    auto put32 = [&](int32_t v) { memcpy(p, &v, 4), p += 4; };
    auto put64 = [&](uint64_t v) { memcpy(p, &v, 8), p += 8; };
    *p++ = 0x80, *p++ = 0x3D, put32(int32_t(flag - (p + 5))), *p++ = 0x00;   // cmp byte [rip+flag],0
    *p++ = 0x74, *p++ = 0x17;                                  // je orig (skip cmp 7 + jne 2 + jmp 14)
    *p++ = 0x40, *p++ = 0x3A, *p++ = 0x2D, put32(int32_t(vid - (p + 4)));    // cmp bpl,[rip+vid]
    *p++ = 0x75, *p++ = 0x0E;                                  // jne orig (skip jmp 14)
    *p++ = 0xFF, *p++ = 0x25, put32(0), put64(after);          // jmp [rip+0] -> past the countdown start
    *p++ = 0x48, *p++ = 0xB8, put64(bus);                      // orig: mov rax,BUS
    *p++ = 0x48, *p++ = 0x8B, *p++ = 0x00;                     //       mov rax,[rax]
    *p++ = 0xFF, *p++ = 0x25, put32(0), put64(site + 7);       //       jmp [rip+0] -> back
    *flag = 0, *vid = 0xff;
    FlushInstructionCache(GetCurrentProcess(), m, 4096);
    g_noqte_flag = flag, g_noqte_vid = vid;
    uint8_t jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    int32_t r = int32_t(reinterpret_cast<intptr_t>(m) - intptr_t(site + 5));
    memcpy(jump + 1, &r, 4);
    return kizi::patch_code(site, jump, 7);
}

// Stuck in the box after the tyres. Sometimes the game never lets your car leave the box after the crew has changed
// the tyres, while the AI drives out at once. So: once your tyres have been changed (their wear drops back towards 0,
// or the game puts new tyre objects on), your car standing still with the throttle pressed for 0.5 s -> the car is
// handed to the AI (PIT_LANE flag, AI mode) until it is moving (5 km/h) or 3 s have passed, then you get it back.
// Speed: car body ([[veh+0x558]+0x340]+0x18, +0x220 velocity). Tyres: [veh+0x580] +0x70..+0x88 = the four tyre
// objects, +0x88 in each = wear (what the telemetry damage packet sends). Throttle: your human controller's input
// record ([[set+0x20]+8], +0x0c, the same layout as the AI's record).
uintptr_t g_vehicles;                      // vehicle table (index = vehicle id)

uint64_t player_vehicle()
{
    int vid = g_player_vid;
    uint64_t veh = 0;
    return g_vehicles && vid >= 0 && vid < 32 && kizi::read(g_vehicles + vid * 8, veh) ? veh : 0;
}

float car_kmh()
{
    uint64_t veh = player_vehicle(), phys = 0, rb = 0, body = 0;
    float v[3];
    if (!veh || !kizi::read(veh + 0x558, phys) || !phys || !kizi::read(phys + 0x340, rb) || !rb ||
        !kizi::read(rb + 0x18, body) || !body || !kizi::read(body + 0x220, v, sizeof v))
        return -1;
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * 3.6f;
}

struct Tyres {
    uint64_t obj[4] = {};
    float wear = -1;                           // sum of the four
};
Tyres tyres()
{
    Tyres t;
    uint64_t veh = player_vehicle(), set = 0;
    if (!veh || !kizi::read(veh + 0x580, set) || !set)
        return t;
    float sum = 0;
    for (int i = 0; i < 4; i++) {
        float w = 0;
        if (!kizi::read(set + 0x70 + 8 * i, t.obj[i]) || !t.obj[i] || !kizi::read(t.obj[i] + 0x88, w))
            return Tyres{};
        sum += w;
    }
    t.wear = sum;
    return t;
}

float throttle(uintptr_t set)
{
    uint64_t human = 0, out = 0;
    float t = 0;
    return kizi::read(set + 0x20, human) && human && kizi::read(human + 8, out) && out && kizi::read(out + 0xc, t)
               ? t
               : 0;
}

void worker()
{
    auto code = kizi::image_ranges(true), data = kizi::image_ranges(false);
    find_tables(code);
    // vehicle table: lea rax,[rip+TABLE]; mov rcx,[rax+rdi*8]; test rcx,rcx; jz ..; mov r14,[rcx+..] (telemetry writer)
    kizi::find_all(code, kizi::Pattern("48 8D 05 ?? ?? ?? ?? 48 8B 0C F8 48 85 C9 0F 84 ?? ?? ?? ?? 4C 8B B1"),
                   [&](uintptr_t a) {
                       g_vehicles = a + 7 + kizi::rel32(a + 3);
                       return false;
                   });
    find_pit_bit(data);
    swofty::log("[pitlane] %zu car table candidate(s) in the code", g_tables.size());
    if (g_tables.empty()) {
        swofty::log("[pitlane] car table code not found on this game build - addon inactive");
        return;
    }
    bool manual_ok = install_manual();
    bool button_ok = install_button(code, data);
    bool noqte_ok = install_no_qte(code);
    swofty::log("[pitlane] remove the quick-time event %s", noqte_ok ? "installed" : "NOT found");
    swofty::log("[pitlane] pit stop quick-time event %s (%s)", button_ok ? "found" : "NOT found - press it yourself",
                no_qte.get() ? "done for you in the pit lane" : "you press it");
    CarTable *live = nullptr;
    uintptr_t car = 0;
    bool in_pits = false, down = false;
    int kept = 0;
    uint64_t car_hp = 0;                              // your human controller when the car was found
    DWORD still_since = 0, rescue_until = 0, window_at = 0, holdoff_until = 0;
    Tyres at_entry;
    bool changed = false;
    // The game rebuilds the controllers on a flashback or Return to Garage. Writing into a stale set, or giving a car
    // the human mode without its human controller, crashes the game: let go of the car and wait until it settles.
    auto drop = [&](const char *why) {
        swofty::log("[pitlane] your car's controller changed (%s): manual control released for 3 s", why);
        car = 0, in_pits = false, rescue_until = 0, window_at = 0, still_since = 0, changed = false;
        g_qte_until = 0, g_press_until = 0;
        if (g_noqte_flag)
            *g_noqte_flag = 0;
        holdoff_until = GetTickCount() + 3000;
    };
    for (int tick = 0;; tick++) {
        Sleep(20);
        bool now = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F7) & 0x8000);
        if (now && !down) {
            bool o = !on();
            g_key_state = o;
            osd::show(o ? "MANUAL PIT LANE: ON" : "MANUAL PIT LANE: OFF", o ? "YOU DRIVE THE PIT LANE" : "AI DRIVES THE PIT LANE");
            for (int i = 0; i < (o ? 2 : 1); i++)
                Beep(900, 120), Sleep(60);
            swofty::log("[pitlane] %s (Ctrl+F7)", o ? "on" : "off");
        }
        down = now;
        g_manual = manual_ok && on() && stop_mode.get() == "manual" && !ai_drives();
        if (int(holdoff_until - GetTickCount()) > 0) {   // a flashback / Return to Garage just rebuilt the controllers
            car = 0, g_player_vid = -1;
            continue;
        }
        if (tick % 25 == 0) {                         // every 0.5 s: (re)find the live table and your car
            std::vector<uintptr_t> sets;
            if (!live || (sets = car_sets(*live)).empty()) {
                live = nullptr;
                for (auto &t : g_tables)
                    if (!(sets = car_sets(t)).empty()) {
                        live = &t;
                        break;
                    }
            }
            int vid = -1;
            uintptr_t p = live ? player(sets, vid) : 0;
            if (p != car && p)
                swofty::log("[pitlane] your car found (%zu cars, vehicle id %d)", sets.size(), vid);
            if (p != car)
                car_hp = p ? q(p + 0x20) : 0;
            car = p;
            g_player_vid = vid;
        }
        if (!car)
            continue;
        {   // every tick: still your live controller set, with both controllers and a valid mode? (The AI controller's vtable may
        // be a heap copy made by AI Racing Tweaks or AI Drive, so only the human one has to be in the game's code.)
            uintptr_t mgr = q(live->global), b = mgr ? q(mgr + live->vec) : 0;
            uint64_t hp = q(car + 0x20), ap = q(car + 0x28);
            uint32_t m = u32(car + live->mode) ^ u32(car + live->key);
            const char *why = !b || g_player_vid < 0 || q(b + 8 * uintptr_t(g_player_vid)) != car ? "car table changed"
                              : !hp || !ap || !kizi::in_image(q(hp)) || !q(ap) ? "controller missing"
                              : hp != car_hp                                                  ? "human controller replaced"
                              : m > 1                                                         ? "control mode not valid"
                                                                                              : nullptr;
            if (why) {
                drop(why);
                continue;
            }
        }
        uint32_t fl = u32(car + FLAGS), bit = 1u << g_pit_bit;
        {   // research: every change of your car's flags / control mode, to see what the game does at the box
            static uint32_t last_fl = ~0u, last_mode = ~0u;
            uint32_t mode = u32(car + live->mode) ^ u32(car + live->key);
            if (fl != last_fl || mode != last_mode)
                swofty::log("[pitlane] your car: flags %#x -> %#x, mode %u -> %u", last_fl, fl, last_mode, mode);
            last_fl = fl, last_mode = mode;
        }
        if (!on())
            continue;
        if (g_noqte_flag) {                            // your car, while the pit lane window is open
            int pv = g_player_vid;
            *g_noqte_vid = pv >= 0 && pv < 0xff ? uint8_t(pv) : 0xff;
            uint8_t want = no_qte.get() && int(g_qte_until - GetTickCount()) > 0 ? 1 : 0;
            if (*g_noqte_flag != want)
                swofty::log("[pitlane] quick-time event %s", want ? "removed (pit lane)" : "back to the game's");
            *g_noqte_flag = want;
        }
        if (int n = g_presses.exchange(0))
            swofty::log("[pitlane] pit stop quick-time event done for you (%d answer(s))", n);
        {   // stuck in the box after the tyres -> the AI pulls the car out, then gives it back
            DWORD now = GetTickCount();
            uint32_t key = u32(car + live->key);
            if (rescue_until) {
                float kmh = car_kmh();
                if (kmh > 5 || int(rescue_until - now) <= 0) {
                    rescue_until = 0;
                    uint32_t f2 = u32(car + FLAGS) & ~bit, mode = 0 ^ key;
                    kizi::write(car + FLAGS, &f2, 4);
                    if (!f2)
                        kizi::write(car + live->mode, &mode, 4);
                    swofty::log("[pitlane] pit box: control back to you (%.0f km/h)", kmh);
                }
                continue;                             // keep the flag while the AI drives
            }
            bool window = int(g_qte_until - now) > 0;
            if (!window) {
                window_at = 0, changed = false, still_since = 0;
            } else {
                if (window_at != g_qte_until) {       // a new pit lane visit: note the tyres as they come in
                    window_at = g_qte_until, at_entry = tyres(), changed = false;
                }
                if (!changed && at_entry.wear >= 0) {
                    Tyres t = tyres();
                    bool new_objects = t.wear >= 0 && memcmp(t.obj, at_entry.obj, sizeof t.obj) != 0;
                    if (t.wear >= 0 && (new_objects || t.wear < at_entry.wear - 0.005f)) {
                        changed = true;
                        swofty::log("[pitlane] pit box: tyres changed (wear %.3f -> %.3f%s)", at_entry.wear, t.wear,
                                    new_objects ? ", new tyres" : "");
                    }
                }
                float kmh = car_kmh(), thr = throttle(car);
                if (changed && kmh >= 0 && kmh < 1.5f && thr > 0.5f) {
                    if (!still_since)
                        still_since = now;
                    else if (now - still_since > 500) {
                        still_since = 0, rescue_until = now + 3000, changed = false;
                        uint32_t f2 = u32(car + FLAGS) | bit, mode = 1 ^ key;
                        kizi::write(car + FLAGS, &f2, 4);
                        kizi::write(car + live->mode, &mode, 4);
                        swofty::log("[pitlane] pit box: stuck after the tyres (throttle %.2f, %.1f km/h) - the AI pulls "
                                    "the car out", thr, kmh);
                        osd::show("PIT RELEASE", "THE AI PULLS YOU OUT", 1500);
                        continue;
                    }
                } else {
                    still_since = 0;
                }
            }
        }
        if (!(fl & bit)) {
            in_pits = false;
            continue;
        }
        uint32_t rest = fl & ~bit;
        if (!kizi::write(car + FLAGS, &rest, 4))
            continue;
        if (!rest) {                                  // nothing else wants the AI: hand the car back to you
            uint32_t key = u32(car + live->key), mode = 0 ^ key;
            kizi::write(car + live->mode, &mode, 4);
        }
        if (!in_pits) {
            in_pits = true;
            kept++;
            swofty::log("[pitlane] pit lane: control kept (%d this session)%s", kept,
                        rest ? " - another flag is set too, so the AI keeps the car for now" : "");
            osd::show("PIT LANE: YOU DRIVE", "USE YOUR PIT LIMITER", 2500);
            if (button_ok && no_qte.get())
                g_press_until = GetTickCount() + 90000;        // the whole pit lane, at most 90 s
            g_qte_until = GetTickCount() + 90000;

        }
    }
}

}  // namespace

SWOFTY_ON_CHANGE(enabled, bool)
{
    g_key_state = -1;
}

SWOFTY_ON_CHANGE(osd_message, bool on)
{
    osd::enabled = on;
}

SWOFTY_ON_LOAD
{
    osd::enabled = osd_message.get();
    swofty::log("[pitlane] %s; on-screen messages: %s; Ctrl+F7 toggles", enabled.get() ? "on" : "off",
                swofty::hud::available() ? "game race notification" : osd::start() ? "box on screen" : "not available on this launcher (beeps only)");
    std::thread(worker).detach();
}

SWOFTY_ADDON()
