// 键鼠过滤的测试。
//   1. 装钩子：DirectInput 设备虚表的 GetDeviceState / GetDeviceData 换成了钩子、调用照常放行；
//      测试程序自己导入表里的 GetAsyncKeyState / GetKeyState 被换掉（和游戏模块一样按值找）。
//   2. 过滤逻辑：不经过真设备，直接喂数据（键盘 256 字节状态、鼠标 DIMOUSESTATE2、事件数组、按键查询）。
// 不按真键、不抢键盘鼠标（设备不 Acquire）。
#include "kbmfilter.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

typedef HRESULT(WINAPI* CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

static IDirectInputDevice8W* NewDevice(CreateFn create, const GUID& g, IDirectInput8W** diOut)
{
    IDirectInput8W* di = nullptr;
    if (FAILED(create(::GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                      reinterpret_cast<void**>(&di), nullptr)) || !di)
        return nullptr;
    IDirectInputDevice8W* dev = nullptr;
    if (FAILED(di->CreateDevice(g, &dev, nullptr))) dev = nullptr;
    *diOut = di;
    return dev;
}

// 导入表里的那一格（直接读，编译器不会拿装钩子之前读到的值来顶）
extern "C" void* __imp_GetAsyncKeyState;
extern "C" void* __imp_GetKeyState;
static void* Iat(void** p) { return *reinterpret_cast<void* volatile*>(p); }

static void* Slot(void* obj, int i) { return (*reinterpret_cast<void***>(obj))[i]; }

static bool AllZero(const unsigned char* p, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

int main()
{
    std::printf("== 装钩子\n");
    HMODULE d8 = ::LoadLibraryW(L"dinput8.dll");
    CHECK(d8 != nullptr, "dinput8 已加载");
    const CreateFn create = reinterpret_cast<CreateFn>(::GetProcAddress(d8, "DirectInput8Create"));
    IDirectInput8W* di = nullptr;
    IDirectInputDevice8W* kb = NewDevice(create, GUID_SysKeyboard, &di);
    CHECK(kb != nullptr, "建了一个键盘设备");
    void* const origState = kb ? Slot(kb, 9) : nullptr;
    void* const origData = kb ? Slot(kb, 10) : nullptr;

    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    void* const realAsync = reinterpret_cast<void*>(::GetProcAddress(u, "GetAsyncKeyState"));
    void* const realKey = reinterpret_cast<void*>(::GetProcAddress(u, "GetKeyState"));
    (void)::GetAsyncKeyState(VK_SHIFT);   // 让测试程序的导入表里有这两个函数
    (void)::GetKeyState(VK_CAPITAL);

    std::string info;
    CHECK(kbmfilter::Install(info), "Install：%s", info.c_str());
    CHECK(kbmfilter::Install(info), "再调一次 Install 也是 true");
    CHECK(kb && Slot(kb, 9) != origState && Slot(kb, 10) != origData, "已有设备的虚表两格都换成了钩子");

    IDirectInput8W* di2 = nullptr;
    IDirectInputDevice8W* mouse = NewDevice(create, GUID_SysMouse, &di2);
    CHECK(mouse && Slot(mouse, 9) != origState, "新建的鼠标设备也走钩子");

    unsigned char st[256];
    std::memset(st, 0x55, sizeof(st));
    const HRESULT hr = kb ? kb->GetDeviceState(sizeof(st), st) : E_FAIL;
    CHECK(hr == DIERR_NOTACQUIRED, "没 Acquire 的设备调 GetDeviceState：原函数照常返回 DIERR_NOTACQUIRED（0x%08lX）", (unsigned long)hr);
    DWORD n = 4;
    DIDEVICEOBJECTDATA buf[4] = {};
    const HRESULT hr2 = kb ? kb->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), buf, &n, 0) : E_FAIL;
    CHECK(FAILED(hr2), "GetDeviceData 也照常放行（0x%08lX）", (unsigned long)hr2);

    CHECK(Iat(&__imp_GetAsyncKeyState) != realAsync, "导入表里的 GetAsyncKeyState 换成了钩子");
    CHECK(Iat(&__imp_GetKeyState) != realKey, "导入表里的 GetKeyState 换成了钩子");
    const SHORT caps = ::GetKeyState(VK_CAPITAL);
    CHECK((caps & 1) == (reinterpret_cast<SHORT(WINAPI*)(int)>(realKey)(VK_CAPITAL) & 1), "关着：GetKeyState 读到真实的 Caps Lock 状态");

    if (mouse) mouse->Release();
    if (di2) di2->Release();
    if (kb) kb->Release();
    if (di) di->Release();

    std::printf("== 键盘状态\n");
    const int kKb = 1, kMouse = 2;
    unsigned char k[256] = {};
    k[DIK_M] = 0x80;
    kbmfilter::TestState(kKb, k, 256);
    CHECK(k[DIK_M] == 0x80, "关着：按着 M 照常给游戏");
    kbmfilter::SetBlock(true);
    kbmfilter::TestState(kKb, k, 256);
    CHECK(AllZero(k, 256), "开着：按 M 游戏读到什么都没按");
    k[DIK_M] = 0x80;
    kbmfilter::SetBlock(false);
    kbmfilter::TestState(kKb, k, 256);
    CHECK(AllZero(k, 256), "关面板时 M 还按着：继续挡，等松开");
    std::memset(k, 0, sizeof(k));
    kbmfilter::TestState(kKb, k, 256);
    k[DIK_W] = 0x80;
    kbmfilter::TestState(kKb, k, 256);
    CHECK(k[DIK_W] == 0x80, "全部松开过之后：再按 W 照常给游戏");

    std::printf("== 鼠标状态\n");
    DIMOUSESTATE2 ms{};
    ms.lX = 5; ms.lY = -3; ms.rgbButtons[0] = 0x80;
    kbmfilter::SetBlock(true);
    kbmfilter::TestState(kMouse, &ms, sizeof(ms));
    CHECK(ms.lX == 0 && ms.lY == 0 && ms.rgbButtons[0] == 0, "开着：左键和移动都挡掉");
    ms.lX = 2; ms.rgbButtons[0] = 0x80;
    kbmfilter::SetBlock(false);
    kbmfilter::TestState(kMouse, &ms, sizeof(ms));
    CHECK(ms.lX == 0 && ms.rgbButtons[0] == 0, "关面板时左键还按着（在面板上点的那一下）：继续挡");
    ms = {};
    ms.lX = 7;
    kbmfilter::TestState(kMouse, &ms, sizeof(ms));
    CHECK(ms.lX == 7, "左键松开了：鼠标移动照常给游戏");

    std::printf("== 事件（GetDeviceData）\n");
    DIDEVICEOBJECTDATA ev[4] = {};
    ev[0].dwOfs = DIK_M; ev[0].dwData = 0x80;   // M 按下
    ev[1].dwOfs = DIK_W; ev[1].dwData = 0x00;   // W 松开（开面板前按着的）
    ev[2].dwOfs = DIK_A; ev[2].dwData = 0x80;   // A 按下
    unsigned long left = kbmfilter::TestData(kKb, ev, sizeof(DIDEVICEOBJECTDATA), 3);
    CHECK(left == 3, "关着：键盘事件原样");
    kbmfilter::SetBlock(true);
    left = kbmfilter::TestData(kKb, ev, sizeof(DIDEVICEOBJECTDATA), 3);
    CHECK(left == 1 && ev[0].dwOfs == DIK_W && ev[0].dwData == 0, "开着：只留下 W 松开");
    DIDEVICEOBJECTDATA me[3] = {};
    me[0].dwOfs = DIMOFS_X; me[0].dwData = 12;
    me[1].dwOfs = DIMOFS_BUTTON0; me[1].dwData = 0x80;
    me[2].dwOfs = DIMOFS_BUTTON1; me[2].dwData = 0x00;
    left = kbmfilter::TestData(kMouse, me, sizeof(DIDEVICEOBJECTDATA), 3);
    CHECK(left == 1 && me[0].dwOfs == DIMOFS_BUTTON1, "开着：鼠标只留下右键松开");
    struct Big { DIDEVICEOBJECTDATA d; char pad[8]; } big[2] = {};
    big[0].d.dwOfs = DIK_M; big[0].d.dwData = 0x80;
    big[1].d.dwOfs = DIK_Q; big[1].d.dwData = 0x00;
    left = kbmfilter::TestData(kKb, big, sizeof(Big), 2);
    CHECK(left == 1 && big[0].d.dwOfs == DIK_Q, "游戏用更大的事件结构时按它的步长挪");
    kbmfilter::SetBlock(false);

    std::printf("== 按键查询\n");
    kbmfilter::TestKey('M', 0, false);   // 前面开关过面板，M 在「等松开」：先松开一次
    CHECK(kbmfilter::TestKey('M', (SHORT)0x8000, false) == (SHORT)0x8000, "关着：GetAsyncKeyState 照常");
    kbmfilter::SetBlock(true);
    CHECK(kbmfilter::TestKey('M', (SHORT)0x8001, false) == 0, "开着：GetAsyncKeyState('M') 读到没按");
    CHECK(kbmfilter::TestKey(VK_CAPITAL, (SHORT)0xFF81, true) == 1, "开着：GetKeyState 只留 Caps Lock 的开关位");
    kbmfilter::SetBlock(false);
    CHECK(kbmfilter::TestKey('M', (SHORT)0x8000, false) == 0, "关面板时 M 还按着：继续当没按");
    CHECK(kbmfilter::TestKey('M', 0, false) == 0, "M 松开");
    CHECK(kbmfilter::TestKey('M', (SHORT)0x8000, false) == (SHORT)0x8000, "再按 M：照常");
    CHECK(kbmfilter::TestKey('Z', (SHORT)0x8000, false) == 0, "开着时没查过的键：关面板后也要先松开一次");

    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
