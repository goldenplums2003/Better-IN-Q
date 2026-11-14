// 测试用的「别的插件」：先把游戏表里的系统函数指针换成自己的钩子，再按约定导出 InputHookFor。
#include <windows.h>

typedef int (*ApiFn)(int);
static ApiFn g_orig = nullptr;
static int g_calls = 0;

static int PeerHook(int x)
{
    ++g_calls;
    return g_orig(x) + 1000;
}

extern "C" __declspec(dllexport) void PeerInstall(void** slot)
{
    g_orig = reinterpret_cast<ApiFn>(*slot);
    *slot = reinterpret_cast<void*>(PeerHook);
}

extern "C" __declspec(dllexport) int PeerCalls() { return g_calls; }

extern "C" __declspec(dllexport) void* InputHookFor(void* api)
{
    return api == reinterpret_cast<void*>(g_orig) ? reinterpret_cast<void*>(PeerHook) : nullptr;
}
