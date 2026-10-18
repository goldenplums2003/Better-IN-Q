#include "kbmfilter.h"

#include "hook.h"
#include "log.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace kbmfilter {
namespace {

enum Kind { kOther = 0, kKeyboard = 1, kMouse = 2 };

std::atomic<bool> g_block{false};
std::atomic<bool> g_kbMuted{false}, g_mouseMuted{false};   // 面板开过：等这类设备全部松开才恢复
std::atomic<std::uint32_t> g_vkMuted[8];                    // 按键查询：每个虚拟键一位，同上
bool g_installed = false;

// ---------------------------------------------------------------- 过滤逻辑
bool AnyDown(const BYTE* p, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] & 0x80) return true;
    return false;
}

void FilterState(Kind k, DWORD cb, void* data)
{
    std::atomic<bool>* muted = k == kKeyboard ? &g_kbMuted : k == kMouse ? &g_mouseMuted : nullptr;
    if (!muted) return;
    const bool block = g_block.load();
    if (block) *muted = true;
    if (!muted->load()) return;
    const BYTE* b = static_cast<const BYTE*>(data);
    // 鼠标（DIMOUSESTATE / DIMOUSESTATE2）：前 12 字节是 X Y 滚轮，后面是按键
    const bool down = k == kKeyboard ? AnyDown(b, cb) : (cb > 12 && AnyDown(b + 12, cb - 12));
    if (!block && !down) { *muted = false; return; }
    std::memset(data, 0, cb);
}

// 面板开着时：键盘只留松开的事件，鼠标只留按键松开的事件（移动、滚轮、按下都扔掉）
DWORD FilterData(Kind k, DWORD cbObj, void* rg, DWORD n)
{
    if (k == kOther || !g_block.load() || cbObj < sizeof(DIDEVICEOBJECTDATA)) return n;
    BYTE* base = static_cast<BYTE*>(rg);
    DWORD out = 0;
    for (DWORD i = 0; i < n; ++i) {
        const DIDEVICEOBJECTDATA* e = reinterpret_cast<const DIDEVICEOBJECTDATA*>(base + i * cbObj);
        const bool release = !(e->dwData & 0x80);
        const bool keep = k == kKeyboard ? release
                                         : (e->dwOfs >= DIMOFS_BUTTON0 && e->dwOfs <= DIMOFS_BUTTON7 && release);
        if (!keep) continue;
        if (out != i) std::memmove(base + out * cbObj, e, cbObj);
        ++out;
    }
    return out;
}

bool VkMuted(int vk) { return (g_vkMuted[vk >> 5].load() >> (vk & 31)) & 1; }

SHORT FilterKey(int vk, SHORT real, bool getKeyState)
{
    if (vk < 0 || vk > 255) return real;
    // GetKeyState 的最低位是 Caps Lock 这类开关的状态，不算「按着」，照旧给
    const SHORT none = getKeyState ? (SHORT)(real & 1) : 0;
    if (g_block.load()) {
        g_vkMuted[vk >> 5].fetch_or(1u << (vk & 31));
        return none;
    }
    if (!VkMuted(vk)) return real;
    if (real & 0x8000) return none;
    g_vkMuted[vk >> 5].fetch_and(~(1u << (vk & 31)));
    return real;
}

// ---------------------------------------------------------------- DirectInput
// 设备 → 类型。第一次见到某个设备时问一次 GetCapabilities（虚表第 3 格，没换过）
std::mutex g_kindMx;
struct KindEntry { void* dev; Kind kind; };
KindEntry g_kinds[32] = {};
int g_kindNext = 0;

Kind KindOf(void* dev)
{
    std::lock_guard<std::mutex> lk(g_kindMx);
    for (const KindEntry& e : g_kinds)
        if (e.dev == dev) return e.kind;
    typedef HRESULT(WINAPI * CapsFn)(void*, DIDEVCAPS*);
    const CapsFn caps = reinterpret_cast<CapsFn>((*reinterpret_cast<void***>(dev))[3]);
    DIDEVCAPS c{};
    c.dwSize = sizeof(c);
    Kind k = kOther;
    if (SUCCEEDED(caps(dev, &c))) {
        const BYTE t = (BYTE)(c.dwDevType & 0xFF);
        k = t == DI8DEVTYPE_KEYBOARD ? kKeyboard : t == DI8DEVTYPE_MOUSE ? kMouse : kOther;
    }
    g_kinds[g_kindNext] = {dev, k};
    g_kindNext = (g_kindNext + 1) % 32;
    return k;
}

bool Active() { return g_block.load() || g_kbMuted.load() || g_mouseMuted.load(); }

typedef HRESULT(WINAPI* GetStateFn)(void*, DWORD, LPVOID);
typedef HRESULT(WINAPI* GetDataFn)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

// A / W 两套接口、键盘鼠标可能各有各的虚表；按原函数地址分槽，同一个原函数只占一个槽
constexpr int kSlots = 4;
GetStateFn g_origState[kSlots] = {};
GetDataFn g_origData[kSlots] = {};
std::atomic<long> g_hitsState{0}, g_hitsData{0};   // 面板开着时真挡掉过几次（日志里看走的是哪条路）

template <int K> HRESULT WINAPI HkState(void* self, DWORD cb, LPVOID data)
{
    const HRESULT hr = g_origState[K](self, cb, data);
    if (SUCCEEDED(hr) && data && Active()) {
        FilterState(KindOf(self), cb, data);
        if (g_block.load()) ++g_hitsState;
    }
    return hr;
}

template <int K> HRESULT WINAPI HkData(void* self, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD n, DWORD flags)
{
    const HRESULT hr = g_origData[K](self, cbObj, rg, n, flags);
    if (SUCCEEDED(hr) && rg && n && *n && g_block.load()) {
        const DWORD before = *n;
        *n = FilterData(KindOf(self), cbObj, rg, *n);
        if (*n != before) ++g_hitsData;
    }
    return hr;
}

const GetStateFn kStateHooks[kSlots] = {HkState<0>, HkState<1>, HkState<2>, HkState<3>};
const GetDataFn kDataHooks[kSlots] = {HkData<0>, HkData<1>, HkData<2>, HkData<3>};

// 虚表第 idx 格换成钩子；这一格已经是我们的就不动。返回换了几格。
template <class Fn> int PatchSlot(void** vt, int idx, Fn* orig, const Fn* hooks)
{
    Fn cur = reinterpret_cast<Fn>(vt[idx]);
    for (int s = 0; s < kSlots; ++s)
        if (cur == hooks[s]) return 0;
    int slot = -1;
    for (int s = 0; s < kSlots && slot < 0; ++s)
        if (orig[s] == cur) slot = s;
    for (int s = 0; s < kSlots && slot < 0; ++s)
        if (!orig[s]) { orig[s] = cur; slot = s; }   // 先填好原函数再换格子
    if (slot < 0) return 0;
    void* hk = reinterpret_cast<void*>(hooks[slot]);
    return hook::WriteMem(reinterpret_cast<std::uintptr_t>(&vt[idx]), &hk, sizeof(hk)) ? 1 : 0;
}

bool g_diDone = false;

bool HookDirectInput(std::string& info)
{
    HMODULE m = ::GetModuleHandleW(L"dinput8.dll");
    if (!m) { info = "游戏还没加载 dinput8"; return false; }
    typedef HRESULT(WINAPI * CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const CreateFn create = reinterpret_cast<CreateFn>(::GetProcAddress(m, "DirectInput8Create"));
    if (!create) { info = "dinput8 里没有 DirectInput8Create"; return false; }

    int devices = 0, patched = 0;
    for (int wide = 0; wide < 2; ++wide) {
        void* di = nullptr;
        if (FAILED(create(::GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, wide ? IID_IDirectInput8W : IID_IDirectInput8A,
                          &di, nullptr)) || !di)
            continue;
        for (const GUID* g : {&GUID_SysKeyboard, &GUID_SysMouse}) {
            IUnknown* dev = nullptr;
            const HRESULT hr =
                wide ? static_cast<IDirectInput8W*>(di)->CreateDevice(*g, reinterpret_cast<IDirectInputDevice8W**>(&dev), nullptr)
                     : static_cast<IDirectInput8A*>(di)->CreateDevice(*g, reinterpret_cast<IDirectInputDevice8A**>(&dev), nullptr);
            if (FAILED(hr) || !dev) continue;
            ++devices;
            void** vt = *reinterpret_cast<void***>(dev);
            patched += PatchSlot<GetStateFn>(vt, 9, g_origState, kStateHooks);    // GetDeviceState
            patched += PatchSlot<GetDataFn>(vt, 10, g_origData, kDataHooks);      // GetDeviceData
            dev->Release();
        }
        static_cast<IUnknown*>(di)->Release();
    }
    if (!devices) { info = "建不出 DirectInput 设备"; return false; }
    info = "DirectInput 换了 " + std::to_string(patched) + " 格";
    return true;
}

// ---------------------------------------------------------------- 按键查询
typedef SHORT(WINAPI* KeyFn)(int);
struct Orig {
    KeyFn async;
    KeyFn key;
} g_orig = {};

SHORT WINAPI HkGetAsyncKeyState(int vk) { return FilterKey(vk, g_orig.async(vk), false); }
SHORT WINAPI HkGetKeyState(int vk) { return FilterKey(vk, g_orig.key(vk), true); }

} // namespace

bool Install(std::string& info)
{
    if (g_installed) return true;
    std::string di;
    if (!g_diDone) {
        g_diDone = HookDirectInput(di);
        if (g_diDone) Log("kbmfilter: %s", di.c_str());
    } else {
        di = "DirectInput 已换";
    }

    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (!u) { info = di + "；user32 还没加载"; return false; }
    if (!g_orig.async) {   // 先填好原函数再换指针：换完的下一瞬间游戏就可能调进来
        g_orig.async = reinterpret_cast<KeyFn>(::GetProcAddress(u, "GetAsyncKeyState"));
        g_orig.key = reinterpret_cast<KeyFn>(::GetProcAddress(u, "GetKeyState"));
    }
    if (!g_orig.async || !g_orig.key) { info = di + "；user32 里找不到按键查询函数"; return false; }
    const std::uintptr_t exe = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    const struct { const char* name; void* fn; void* hk; } fns[] = {
        {"GetAsyncKeyState", reinterpret_cast<void*>(g_orig.async), reinterpret_cast<void*>(HkGetAsyncKeyState)},
        {"GetKeyState", reinterpret_cast<void*>(g_orig.key), reinterpret_cast<void*>(HkGetKeyState)},
    };
    static int hits[2] = {};   // 累计：没装成会重试，前几次换掉的不会再被找到
    info = di;
    for (int i = 0; i < 2; ++i) {
        std::string where;
        const int n = hook::ReplaceModulePointers(exe, fns[i].fn, fns[i].hk, &g_orig, &g_orig + 1, &where);
        if (n) Log("kbmfilter: 换掉 %s处的 %s", where.c_str(), fns[i].name);
        hits[i] += n;
        info += std::string("，") + fns[i].name + " " + std::to_string(hits[i]) + " 处";
    }
    g_installed = g_diDone && (hits[0] || hits[1]);
    return g_installed;
}

bool Installed() { return g_installed; }

void SetBlock(bool block)
{
    if (block == g_block.load()) return;
    if (block) {
        g_kbMuted = true;
        g_mouseMuted = true;
        for (auto& w : g_vkMuted) w = 0xFFFFFFFFu;
        g_hitsState = 0;
        g_hitsData = 0;
    } else if (g_hitsState || g_hitsData) {
        Log("kbmfilter: 面板开着时挡掉 DirectInput 状态 %ld 次、事件 %ld 批", g_hitsState.load(), g_hitsData.load());
    }
    g_block = block;
}

void TestState(int kind, void* data, unsigned long cb) { FilterState(static_cast<Kind>(kind), cb, data); }
unsigned long TestData(int kind, void* rg, unsigned long cbObj, unsigned long n)
{
    return FilterData(static_cast<Kind>(kind), cbObj, rg, n);
}
short TestKey(int vk, short real, bool getKeyState) { return FilterKey(vk, real, getKeyState); }

} // namespace kbmfilter
