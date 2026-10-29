// 光标过滤的测试：过滤器编进测试程序自己，被换掉的就是测试程序导入表里的
// SetCursorPos / GetCursorPos / ClipCursor，直接调 ::SetCursorPos 就经过钩子，和游戏一样。
//
// 会碰真实光标，但都放回原样：光标最后回到开始的位置；锁定范围只短暂改成
// 「整个屏幕右下各少 1 像素」，测完就放开。
#include "cursorfilter.h"

#include <windows.h>
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

typedef BOOL(WINAPI* SetPosFn)(int, int);
typedef BOOL(WINAPI* GetPosFn)(LPPOINT);
typedef GetPosFn GetPosFnT;
typedef BOOL(WINAPI* ClipFn)(const RECT*);

static bool Same(const POINT& a, const POINT& b) { return a.x == b.x && a.y == b.y; }

// 读真实光标和读「游戏看到的」之间，人可能正好在动鼠标：对不上就再读几次，有一次对上就算
template <class F> static bool SameSoon(F hooked, GetPosFnT real, POINT& a)
{
    for (int i = 0; i < 20; ++i) {
        POINT b{};
        hooked(&a);
        real(&b);
        if (Same(a, b)) return true;
        ::Sleep(10);
    }
    return false;
}
static bool Same(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

int main()
{
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    const auto realSet = reinterpret_cast<SetPosFn>(::GetProcAddress(u, "SetCursorPos"));
    const auto realGet = reinterpret_cast<GetPosFn>(::GetProcAddress(u, "GetCursorPos"));
    const auto realClip = reinterpret_cast<ClipFn>(::GetProcAddress(u, "ClipCursor"));

    std::string info;
    CHECK(cursorfilter::Install(info), "Install：%s", info.c_str());
    CHECK(reinterpret_cast<void*>(&::SetCursorPos) != reinterpret_cast<void*>(realSet) &&
          reinterpret_cast<void*>(&::GetCursorPos) != reinterpret_cast<void*>(realGet) &&
          reinterpret_cast<void*>(&::ClipCursor) != reinterpret_cast<void*>(realClip),
          "导入表里的三个函数都换成了钩子");

    // ---- 面板关着：原样放行 ----
    POINT a{}, b{};
    CHECK(SameSoon(::GetCursorPos, realGet, a), "关着：GetCursorPos 读到真实位置 (%ld,%ld)", a.x, a.y);
    (void)b;

    // ---- 面板开着：游戏挪不动光标，读到的是它想放的位置 ----
    cursorfilter::SetBlock(true);
    // 冻住 = 隔一会儿再读还是同一个位置（这期间真实光标动没动都一样）
    POINT start{}, g{}, g2{};
    ::GetCursorPos(&g);
    ::Sleep(60);
    ::GetCursorPos(&g2);
    CHECK(Same(g, g2), "开面板那一刻的位置被冻住 (%ld,%ld)", g.x, g.y);
    start = g;
    const POINT t{start.x > 100 ? start.x - 50 : start.x + 50, start.y > 100 ? start.y - 50 : start.y + 50};
    CHECK(::SetCursorPos(t.x, t.y), "开着：SetCursorPos 返回成功");
    POINT now{};
    realGet(&now);
    CHECK(!Same(now, t), "开着：真实光标没被挪 (%ld,%ld)", now.x, now.y);
    ::GetCursorPos(&g);
    CHECK(Same(g, t), "开着：游戏读到的就是它刚想放的位置 (%ld,%ld)", g.x, g.y);
    ::SetCursorPos(start.x, start.y);   // 游戏「最后想放的位置」设回起点，关面板时光标就停在原地

    // ---- 锁定范围 ----
    RECT before{};
    ::GetClipCursor(&before);
    const RECT full{::GetSystemMetrics(SM_XVIRTUALSCREEN), ::GetSystemMetrics(SM_YVIRTUALSCREEN),
                    ::GetSystemMetrics(SM_XVIRTUALSCREEN) + ::GetSystemMetrics(SM_CXVIRTUALSCREEN),
                    ::GetSystemMetrics(SM_YVIRTUALSCREEN) + ::GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    const RECT r{full.left, full.top, full.right - 1, full.bottom - 1};
    RECT c{};
    if (!Same(before, full)) {
        std::printf("  skip 锁定范围的测试：现在光标被别的程序锁着 (%ld,%ld)-(%ld,%ld)\n",
                    before.left, before.top, before.right, before.bottom);
        cursorfilter::SetBlock(false);
    } else {
        CHECK(::ClipCursor(&r), "开着：ClipCursor 返回成功");
        ::GetClipCursor(&c);
        CHECK(Same(c, full), "开着：游戏锁不住光标");

        cursorfilter::SetBlock(false);
        realGet(&now);
        CHECK(Same(now, start), "关面板：光标停在游戏最后想放的位置 (%ld,%ld)", now.x, now.y);
        ::GetClipCursor(&c);
        CHECK(Same(c, r), "关面板：补上游戏在面板开着时要的锁定 (%ld,%ld)-(%ld,%ld)", c.left, c.top, c.right, c.bottom);

        // 游戏先锁着，再开面板：放开（测试程序的前台窗口不是自己的，没有游戏窗口可锁）
        cursorfilter::SetBlock(true);
        ::GetClipCursor(&c);
        CHECK(Same(c, full), "已锁着时开面板：放开");
        cursorfilter::SetBlock(false);
        ::GetClipCursor(&c);
        CHECK(Same(c, r), "关面板：恢复原来的锁定");

        // 开着期间游戏自己放开了：关面板后保持放开
        cursorfilter::SetBlock(true);
        ::ClipCursor(nullptr);
        cursorfilter::SetBlock(false);
        ::GetClipCursor(&c);
        CHECK(Same(c, full), "开着期间游戏放开了锁定 → 关面板后不再锁");
        realClip(nullptr);
    }

    // ---- 关着：又原样放行 ----
    CHECK(SameSoon(::GetCursorPos, realGet, a), "关着：GetCursorPos 又读真实位置");
    CHECK(::ClipCursor(&r), "关着：ClipCursor 直接生效");
    ::GetClipCursor(&c);
    CHECK(Same(c, r), "关着：锁定范围 = 游戏要的");
    realClip(nullptr);
    realSet(start.x, start.y);

    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
