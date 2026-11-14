#include "cursorfilter.h"

#include "hook.h"
#include "log.h"

#include <windows.h>
#include <cstdint>
#include <mutex>

namespace cursorfilter {
namespace {

typedef BOOL(WINAPI* SetPosFn)(int, int);
typedef BOOL(WINAPI* GetPosFn)(LPPOINT);
typedef BOOL(WINAPI* ClipFn)(const RECT*);

// 原函数（GetProcAddress 拿的）。换指针时要跳过这张表本身，理由同 padfilter。
struct Orig {
    SetPosFn setPos;
    GetPosFn getPos;
    ClipFn clip;
} g_orig = {};

std::mutex g_mx;         // 钩子在游戏线程里跑，SetBlock 在主循环里调
bool g_block = false;
POINT g_pos = {};        // 面板开着时游戏读到的光标位置
bool g_moved = false;    // 面板开着时游戏挪过光标 → 关面板时真挪过去
bool g_clipOn = false;   // 游戏要的锁定范围（g_clipOn=false 表示不锁）
RECT g_clip = {};
bool g_clipDirty = false;   // 面板开着期间锁定范围被动过 → 关面板时要按游戏的意思重设
bool g_installed = false;

// 钩子都在锁里调原函数：SetBlock 改状态和游戏改光标不会交错（两者都不会回调进来）
BOOL WINAPI HkSetCursorPos(int x, int y)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_block) return g_orig.setPos(x, y);
    g_pos = {x, y};
    g_moved = true;
    return TRUE;
}

BOOL WINAPI HkGetCursorPos(LPPOINT p)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_block || !p) return g_orig.getPos(p);
    *p = g_pos;
    return TRUE;
}

BOOL WINAPI HkClipCursor(const RECT* r)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_block) return g_orig.clip(r);
    g_clipOn = r != nullptr;
    if (r) g_clip = *r;
    g_clipDirty = true;
    return TRUE;
}

bool IsWholeScreen(const RECT& r)
{
    const int x = ::GetSystemMetrics(SM_XVIRTUALSCREEN), y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    return r.left <= x && r.top <= y && r.right >= x + ::GetSystemMetrics(SM_CXVIRTUALSCREEN) &&
           r.bottom >= y + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// 前台窗口是本进程的，就当它是游戏窗口，取客户区的屏幕坐标
bool GameWindowRect(RECT& r)
{
    HWND h = ::GetForegroundWindow();
    DWORD pid = 0;
    if (!h || !::GetWindowThreadProcessId(h, &pid) || pid != ::GetCurrentProcessId()) return false;
    if (!::GetClientRect(h, &r)) return false;
    ::MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&r), 2);
    return r.right > r.left && r.bottom > r.top;
}

} // namespace

bool Install(std::string& info)
{
    if (g_installed) return true;
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (!u) { info = "user32 还没加载"; return false; }
    void* const api[3] = {reinterpret_cast<void*>(::GetProcAddress(u, "SetCursorPos")),
                          reinterpret_cast<void*>(::GetProcAddress(u, "GetCursorPos")),
                          reinterpret_cast<void*>(::GetProcAddress(u, "ClipCursor"))};
    if (!api[0] || !api[1] || !api[2]) { info = "user32 里找不到光标函数"; return false; }
    // 先填好原函数再换指针：换完的下一瞬间游戏就可能调进来（重试时别把已经接上的链冲掉）
    if (!g_orig.setPos) {
        g_orig.setPos = reinterpret_cast<SetPosFn>(api[0]);
        g_orig.getPos = reinterpret_cast<GetPosFn>(api[1]);
        g_orig.clip = reinterpret_cast<ClipFn>(api[2]);
    }

    const std::uintptr_t exe = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    const struct { const char* name; void* fn; void* hk; void** orig; } fns[] = {
        {"SetCursorPos", api[0], reinterpret_cast<void*>(HkSetCursorPos), reinterpret_cast<void**>(&g_orig.setPos)},
        {"GetCursorPos", api[1], reinterpret_cast<void*>(HkGetCursorPos), reinterpret_cast<void**>(&g_orig.getPos)},
        {"ClipCursor", api[2], reinterpret_cast<void*>(HkClipCursor), reinterpret_cast<void**>(&g_orig.clip)},
    };
    static int hits[3] = {};   // 累计：没装成会重试，前几次换掉的不会再被找到
    info.clear();
    for (int i = 0; i < 3; ++i) {
        std::string where;
        const int n = hits[i] ? 0 : hook::ReplaceChained(exe, fns[i].fn, fns[i].hk, fns[i].orig, &where);
        if (n) Log("cursorfilter: 换掉 %s处的 %s", where.c_str(), fns[i].name);
        hits[i] += n;
        info += std::string(i ? "，" : "") + fns[i].name + " " + std::to_string(hits[i]) + " 处";
    }
    if (!hits[0]) return false;
    // 只换到 SetCursorPos：面板开着时游戏读到的是真实位置，会以为鼠标在动。
    // 光标照样不会被拽回中间，只是视角可能跟着转，记一笔方便查。
    if (!hits[1]) info += "（没换到 GetCursorPos，面板开着时视角可能跟着鼠标转）";
    g_installed = true;
    return true;
}

bool Installed() { return g_installed; }

void SetBlock(bool block)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_installed || block == g_block) return;
    if (block) {
        // 从这一刻起游戏读到的光标位置不再变
        if (!g_orig.getPos(&g_pos)) g_pos = {};
        g_moved = false;
        // 游戏正锁着光标的话记下来，换成锁在游戏窗口里：面板能点到，光标也跑不出窗口
        RECT cur{}, win{};
        g_clipOn = ::GetClipCursor(&cur) && !IsWholeScreen(cur);
        g_clip = cur;
        g_clipDirty = g_clipOn;
        if (g_clipOn) g_orig.clip(GameWindowRect(win) ? &win : nullptr);
    } else {
        if (g_moved) g_orig.setPos(g_pos.x, g_pos.y);   // 放回游戏想放的地方，关面板那一帧视角不会猛跳
        if (g_clipDirty) g_orig.clip(g_clipOn ? &g_clip : nullptr);
    }
    g_block = block;
}

} // namespace cursorfilter
