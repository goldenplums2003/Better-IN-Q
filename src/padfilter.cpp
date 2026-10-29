#include "padfilter.h"

#include "hook.h"
#include "log.h"

#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace padfilter {
namespace {

typedef DWORD(WINAPI* GetStateFn)(DWORD, XINPUT_STATE*);

// 进程里可能同时有好几个 XInput 库（游戏用 xinput9_1_0，我们自己的快捷键和 ImGui 加载的
// 是 xinput1_4），各有各的 XInputGetState。每找到一个就占一个槽，钩子按槽调回对应的原函数。
constexpr int kSlots = 12;
GetStateFn g_orig[kSlots] = {};
int g_slots = 0;
std::atomic<bool> g_block{false};
std::atomic<unsigned short> g_chord{0};
bool g_installed = false;

constexpr ULONGLONG kChordWindowMs = 100;
constexpr int kPulsePolls = 4;

struct Pad {
    enum { kIdle, kPending, kChord, kPass } st = kIdle;
    ULONGLONG t0 = 0;
    unsigned short pend = 0;      // 扣住期间按过的组合键
    int pulse = 0;                // 还要补发几次点按
    unsigned short pulseMask = 0;
    bool muted = false;           // 面板开过：等全部松开才恢复
};
Pad g_pad[XUSER_MAX_COUNT];

bool AnyDown(const XINPUT_GAMEPAD& g)
{
    return g.wButtons != 0 || g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
           g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
}

void Filter(DWORD i, XINPUT_STATE* s)
{
    if (i >= XUSER_MAX_COUNT || !s) return;
    Pad& p = g_pad[i];
    XINPUT_GAMEPAD& g = s->Gamepad;

    if (g_block.load()) p.muted = true;
    if (p.muted) {
        if (!g_block.load() && !AnyDown(g)) {
            p.muted = false;
            p.st = Pad::kIdle;
        } else {
            std::memset(&g, 0, sizeof(g));
            return;
        }
    }

    // 只扣视图键、菜单键：它们单按在游戏里是开地图 / 菜单，晚 0.1 秒没关系。组合里别的键
    //（比如 Y+视图键 里的 Y）是出招键，扣住会把连招吞掉（太刀登龙、气刃斩），所以照常立刻给游戏
    const unsigned short c = g_chord.load();
    const unsigned short w = c & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START);
    if (!w) return;
    const unsigned short down = g.wButtons & w;
    const bool full = (g.wButtons & c) == c;   // 整个组合都按着
    const ULONGLONG now = ::GetTickCount64();
    switch (p.st) {
    case Pad::kIdle:
        if (full) p.st = Pad::kChord;
        else if (down) { p.st = Pad::kPending; p.t0 = now; p.pend = down; }
        break;
    case Pad::kPending:
        p.pend |= down;
        if (full) p.st = Pad::kChord;
        else if (!down) { p.pulse = kPulsePolls; p.pulseMask = p.pend; p.st = Pad::kIdle; }   // 点一下就松：补发
        else if (now - p.t0 > kChordWindowMs) p.st = Pad::kPass;
        break;
    case Pad::kChord:
    case Pad::kPass:
        if (!down) p.st = Pad::kIdle;
        break;
    }
    if (p.st == Pad::kPending || p.st == Pad::kChord) g.wButtons &= ~w;
    if (p.pulse > 0) { g.wButtons |= p.pulseMask; --p.pulse; }
}

// ---- 诊断：重开后一段时间里，把游戏每次读到的按键变化写进日志 ----
std::atomic<ULONGLONG> g_traceUntil{0};
std::atomic<unsigned> g_traceGen{0};
struct Seen {
    unsigned gen = 0;
    DWORD r = 0;
    unsigned raw = 0, out = 0;
    unsigned calls = 0;
    ULONGLONG lastReport = 0;
};
Seen g_seen[XUSER_MAX_COUNT];

unsigned Mask(const XINPUT_GAMEPAD& g)
{
    return g.wButtons | (g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 1u << 16 : 0) |
           (g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 1u << 17 : 0);
}

std::string Names(unsigned m)
{
    static const struct { unsigned bit; const char* name; } kNames[] = {
        {XINPUT_GAMEPAD_A, "A"}, {XINPUT_GAMEPAD_B, "B"}, {XINPUT_GAMEPAD_X, "X"}, {XINPUT_GAMEPAD_Y, "Y"},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, "LB"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"}, {1u << 16, "LT"}, {1u << 17, "RT"},
        {XINPUT_GAMEPAD_BACK, "View"}, {XINPUT_GAMEPAD_START, "Menu"},
        {XINPUT_GAMEPAD_LEFT_THUMB, "LS"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "RS"},
        {XINPUT_GAMEPAD_DPAD_UP, "上"}, {XINPUT_GAMEPAD_DPAD_DOWN, "下"},
        {XINPUT_GAMEPAD_DPAD_LEFT, "左"}, {XINPUT_GAMEPAD_DPAD_RIGHT, "右"},
    };
    std::string s;
    for (const auto& n : kNames)
        if (m & n.bit) { if (!s.empty()) s += '+'; s += n.name; }
    if (m & 0x0400) s += s.empty() ? "Guide" : "+Guide";
    return s.empty() ? "-" : s;
}

// 游戏线程里只排队，主循环（FlushTrace）再写文件，免得在游戏读手柄的线程里做磁盘 IO
struct Ev {
    DWORD pad, r;
    unsigned raw, out, calls;
    bool report;   // true = 「5 秒内被读了几次」
};
std::mutex g_evMx;
std::vector<Ev> g_ev;

void Push(const Ev& e)
{
    if (!g_evMx.try_lock()) return;   // 主循环正在取：丢这一条，不让游戏线程等
    if (g_ev.size() < 256) g_ev.push_back(e);
    g_evMx.unlock();
}

void TraceRead(DWORD i, DWORD r, unsigned raw, const XINPUT_STATE* s)
{
    if (i >= XUSER_MAX_COUNT) return;
    const ULONGLONG now = ::GetTickCount64();
    if (now >= g_traceUntil.load()) return;
    Seen& t = g_seen[i];
    const unsigned gen = g_traceGen.load();
    const unsigned out = (r == ERROR_SUCCESS && s) ? Mask(s->Gamepad) : 0;
    const bool fresh = t.gen != gen;
    ++t.calls;
    if (fresh || r != t.r || raw != t.raw || out != t.out) {
        if (r == ERROR_SUCCESS || fresh || r != t.r) Push({i, r, raw, out, 0, false});
        if (fresh) { t.calls = 1; t.lastReport = now; }
        t.gen = gen;
        t.r = r;
        t.raw = raw;
        t.out = out;
    }
    if (r == ERROR_SUCCESS && now - t.lastReport >= 5000) {   // 游戏还在不在读手柄
        Push({i, r, 0, 0, t.calls, true});
        t.calls = 0;
        t.lastReport = now;
    }
}

template <int K> DWORD WINAPI HkGetState(DWORD i, XINPUT_STATE* s)
{
    const DWORD r = g_orig[K](i, s);
    const unsigned raw = (r == ERROR_SUCCESS && s) ? Mask(s->Gamepad) : 0;
    if (r == ERROR_SUCCESS) Filter(i, s);
    TraceRead(i, r, raw, s);
    return r;
}

typedef DWORD(WINAPI* HookFn)(DWORD, XINPUT_STATE*);
const HookFn kHooks[kSlots] = {HkGetState<0>, HkGetState<1>, HkGetState<2>, HkGetState<3>,
                               HkGetState<4>, HkGetState<5>, HkGetState<6>, HkGetState<7>,
                               HkGetState<8>, HkGetState<9>, HkGetState<10>, HkGetState<11>};

} // namespace

bool Install(std::string& info)
{
    if (g_installed) return true;
    // MHW 导入的是 XINPUT9_1_0.dll。每个加载了的 XInput 库都试，XInputGetState 和序号 100
    //（XInputGetStateEx，带 Guide 键）都试；同一个地址只占一个槽。
    static const wchar_t* const kDlls[] = {L"xinput9_1_0.dll", L"xinput1_4.dll", L"xinput1_3.dll",
                                           L"xinput1_2.dll", L"xinput1_1.dll"};
    int n = 0;
    bool anyModule = false;
    for (const wchar_t* d : kDlls) {
        HMODULE m = ::GetModuleHandleW(d);
        if (!m) continue;
        anyModule = true;
        const FARPROC fns[2] = {::GetProcAddress(m, "XInputGetState"), ::GetProcAddress(m, reinterpret_cast<LPCSTR>(100))};
        for (int k = 0; k < 2; ++k) {
            GetStateFn fn = reinterpret_cast<GetStateFn>(fns[k]);
            if (!fn || g_slots >= kSlots) continue;
            bool dup = false;
            for (int s = 0; s < g_slots; ++s) dup |= (g_orig[s] == fn);
            if (dup) continue;
            const int slot = g_slots;
            g_orig[slot] = fn;   // 先填好原函数再换指针：换完的下一瞬间游戏就可能调进来
            char what[64];
            std::snprintf(what, sizeof(what), "%ls!%s", d, k == 0 ? "XInputGetState" : "#100");
            // 存原函数的表本身不能换，否则钩子调「原函数」就是调自己。游戏里不会碰到
            //（这张表在我们的 DLL 里，扫的是游戏 exe），过滤器被编进被扫的模块时（测试）会。
            std::string where;
            const int hit = hook::ReplaceModulePointers(reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr)),
                                                        reinterpret_cast<void*>(fn), reinterpret_cast<void*>(kHooks[slot]),
                                                        g_orig, g_orig + kSlots, &where);
            if (hit) { Log("padfilter: 换掉 %s处的 %s", where.c_str(), what); ++g_slots; n += hit; }
            else g_orig[slot] = nullptr;
        }
    }
    if (!anyModule) { info = "游戏还没加载 XInput"; return false; }
    if (n == 0) { info = "游戏模块里没找到任何 XInputGetState 的指针"; return false; }
    g_installed = true;
    info = "替换了 " + std::to_string(n) + " 处";
    return true;
}

bool Installed() { return g_installed; }
void TestFilter(unsigned long pad, void* s) { Filter(pad, static_cast<XINPUT_STATE*>(s)); }
void SetBlock(bool block) { g_block = block; }
void SetChord(unsigned short mask) { g_chord = mask; }
void Trace(unsigned long ms)
{
    ++g_traceGen;
    g_traceUntil = ::GetTickCount64() + ms;
}

void FlushTrace()
{
    std::vector<Ev> ev;
    {
        std::lock_guard<std::mutex> lk(g_evMx);
        if (g_ev.empty()) return;
        ev.swap(g_ev);
    }
    for (const Ev& e : ev) {
        if (e.report) Log("pad%lu 5 秒内被游戏读了 %u 次", e.pad, e.calls);
        else if (e.r != ERROR_SUCCESS) Log("pad%lu 游戏读：没连接（%lu）", e.pad, e.r);
        else if (e.raw == e.out) Log("pad%lu 游戏读到 %s", e.pad, Names(e.out).c_str());
        else Log("pad%lu 手柄 %s → 游戏读到 %s", e.pad, Names(e.raw).c_str(), Names(e.out).c_str());
    }
}

} // namespace padfilter
