#include "input.h"

#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_getState = nullptr;

std::string Upper(std::string s)
{
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// 一个按键名 → 写进 combo。认不出返回 false。
// keyboard=true 时单个字母按键盘字母理解（Ctrl+A 的 A），否则 A/B/X/Y 是手柄键。
bool AddToken(const std::string& raw, Combo& c, bool keyboard)
{
    const std::string t = Upper(Trim(raw));
    struct P { const char* n; unsigned short b; };
    static const P pads[] = {
        {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
        {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {"L1", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"R1", XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
        {"L3", XINPUT_GAMEPAD_LEFT_THUMB}, {"R3", XINPUT_GAMEPAD_RIGHT_THUMB},
        {"UP", XINPUT_GAMEPAD_DPAD_UP}, {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
        {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
        {"VIEW", XINPUT_GAMEPAD_BACK}, {"BACK", XINPUT_GAMEPAD_BACK}, {"SELECT", XINPUT_GAMEPAD_BACK},
        {"SHARE", XINPUT_GAMEPAD_BACK},
        {"MENU", XINPUT_GAMEPAD_START}, {"START", XINPUT_GAMEPAD_START}, {"OPTIONS", XINPUT_GAMEPAD_START},
    };
    if (!keyboard)
        for (const auto& p : pads) if (t == p.n) { c.pad |= p.b; return true; }
    if (t == "LT" || t == "L2") { c.lt = true; return true; }
    if (t == "RT" || t == "R2") { c.rt = true; return true; }

    struct K { const char* n; int vk; };
    static const K keys[] = {
        {"CTRL", VK_CONTROL}, {"SHIFT", VK_SHIFT}, {"ALT", VK_MENU},
        {"INSERT", VK_INSERT}, {"INS", VK_INSERT}, {"DELETE", VK_DELETE}, {"DEL", VK_DELETE},
        {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
        {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"ENTER", VK_RETURN}, {"BACKSPACE", VK_BACK},
        {"PAUSE", VK_PAUSE}, {"SCROLLLOCK", VK_SCROLL},
    };
    for (const auto& k : keys) if (t == k.n) { c.keys.push_back(k.vk); return true; }
    if (t.size() >= 2 && t[0] == 'F' && std::isdigit((unsigned char)t[1])) {
        const int n = std::atoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) { c.keys.push_back(VK_F1 + n - 1); return true; }
    }
    if (t.size() == 1 && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= '0' && t[0] <= '9'))) {
        c.keys.push_back(t[0]);
        return true;
    }
    return false;
}

} // namespace

Hotkey ParseHotkey(const std::string& spec)
{
    Hotkey hk;
    std::size_t p = 0;
    while (p <= spec.size()) {
        std::size_t bar = spec.find('|', p);
        if (bar == std::string::npos) bar = spec.size();
        const std::string alt = Trim(spec.substr(p, bar - p));
        p = bar + 1;
        if (alt.empty()) continue;
        // 先看这一组里有没有修饰键/功能键，有就整组按键盘理解（Ctrl+A 里的 A 是字母键）
        Combo c;
        bool hasKb = false;
        for (std::size_t q = 0; q <= alt.size();) {
            std::size_t plus = alt.find('+', q);
            if (plus == std::string::npos) plus = alt.size();
            const std::string t = Upper(Trim(alt.substr(q, plus - q)));
            if (t == "CTRL" || t == "SHIFT" || t == "ALT" || (t.size() >= 2 && t[0] == 'F' && std::isdigit((unsigned char)t[1])) ||
                t == "INSERT" || t == "INS" || t == "HOME" || t == "END" || t == "DELETE" || t == "DEL" ||
                t == "PAGEUP" || t == "PAGEDOWN")
                hasKb = true;
            q = plus + 1;
        }
        bool ok = true;
        for (std::size_t q = 0; q <= alt.size();) {
            std::size_t plus = alt.find('+', q);
            if (plus == std::string::npos) plus = alt.size();
            const std::string t = alt.substr(q, plus - q);
            q = plus + 1;
            if (Trim(t).empty()) continue;
            if (!AddToken(t, c, hasKb)) {
                ok = false;
                hk.bad += Trim(t) + " ";
            }
        }
        if (ok && (c.IsPad() || !c.keys.empty())) hk.alts.push_back(c);
    }
    return hk;
}

bool Input::Init()
{
    const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* d : dlls) {
        HMODULE m = ::LoadLibraryW(d);
        if (!m) continue;
        g_getState = reinterpret_cast<XInputGetStateFn>(::GetProcAddress(m, "XInputGetState"));
        if (g_getState) return true;
    }
    return false;
}

void Input::Poll()
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
    focused_ = (pid == ::GetCurrentProcessId());
    padOk_ = false;
    buttons_ = 0;
    lt_ = rt_ = 0;
    if (!focused_ || !g_getState) return;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE s{};
        if (g_getState(i, &s) != ERROR_SUCCESS) continue;
        padOk_ = true;
        buttons_ |= s.Gamepad.wButtons;
        if (s.Gamepad.bLeftTrigger > lt_) lt_ = s.Gamepad.bLeftTrigger;
        if (s.Gamepad.bRightTrigger > rt_) rt_ = s.Gamepad.bRightTrigger;
    }
}

bool Input::Held(const Combo& c) const
{
    if (!focused_) return false;
    if (c.IsPad()) {
        if (!padOk_) return false;
        if ((buttons_ & c.pad) != c.pad) return false;
        if (c.lt && lt_ <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) return false;
        if (c.rt && rt_ <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) return false;
    }
    for (int vk : c.keys)
        if (!(::GetAsyncKeyState(vk) & 0x8000)) return false;
    // 组合里没有的修饰键不能按着：不然按 Ctrl+Shift+N 时 Ctrl+N 也算按下了，两个快捷键一起触发
    if (!c.keys.empty())
        for (int mod : {VK_CONTROL, VK_SHIFT, VK_MENU})
            if (std::find(c.keys.begin(), c.keys.end(), mod) == c.keys.end() && (::GetAsyncKeyState(mod) & 0x8000))
                return false;
    return true;
}

bool Input::Fired(Hotkey& hk)
{
    bool now = false;
    for (const auto& c : hk.alts) if (Held(c)) { now = true; break; }
    const bool fired = now && !hk.was;
    hk.was = now;
    return fired;
}

unsigned Input::PadHeld() const
{
    unsigned m = buttons_;
    if (lt_ > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) m |= kPadLT;
    if (rt_ > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) m |= kPadRT;
    return m;
}

namespace {

// 录快捷键时扫的键盘键，顺序就是写出来的顺序（修饰键在前）
struct KeyName { int vk; const char* name; };
const KeyName kKeyNames[] = {
    {VK_CONTROL, "Ctrl"}, {VK_SHIFT, "Shift"}, {VK_MENU, "Alt"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"},
    {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"}, {VK_PAUSE, "Pause"}, {VK_SCROLL, "ScrollLock"},
    {VK_BACK, "Backspace"},
};
bool IsModifier(int vk) { return vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU; }
bool IsCharKey(int vk) { return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'); }

std::string KeyToName(int vk)
{
    for (const auto& k : kKeyNames) if (k.vk == vk) return k.name;
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (IsCharKey(vk)) return std::string(1, (char)vk);
    return std::string();
}

} // namespace

std::vector<int> KeysHeld()
{
    std::vector<int> v;
    auto down = [](int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; };
    for (const auto& k : kKeyNames) if (down(k.vk)) v.push_back(k.vk);
    for (int vk = VK_F1; vk <= VK_F24; ++vk) if (down(vk)) v.push_back(vk);
    for (int vk = '0'; vk <= '9'; ++vk) if (down(vk)) v.push_back(vk);
    for (int vk = 'A'; vk <= 'Z'; ++vk) if (down(vk)) v.push_back(vk);
    return v;
}

std::string PadComboText(unsigned mask)
{
    struct P { unsigned bit; const char* name; };
    static const P pads[] = {
        {XINPUT_GAMEPAD_LEFT_SHOULDER, "LB"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"}, {kPadLT, "LT"}, {kPadRT, "RT"},
        {XINPUT_GAMEPAD_A, "A"}, {XINPUT_GAMEPAD_B, "B"}, {XINPUT_GAMEPAD_X, "X"}, {XINPUT_GAMEPAD_Y, "Y"},
        {XINPUT_GAMEPAD_BACK, "View"}, {XINPUT_GAMEPAD_START, "Menu"},
        {XINPUT_GAMEPAD_LEFT_THUMB, "L3"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "R3"},
        {XINPUT_GAMEPAD_DPAD_UP, "Up"}, {XINPUT_GAMEPAD_DPAD_DOWN, "Down"},
        {XINPUT_GAMEPAD_DPAD_LEFT, "Left"}, {XINPUT_GAMEPAD_DPAD_RIGHT, "Right"},
    };
    std::string s;
    for (const auto& p : pads)
        if (mask & p.bit) s += (s.empty() ? "" : "+") + std::string(p.name);
    return s;
}

std::string KeyComboText(const std::vector<int>& vks)
{
    // 修饰键按 Ctrl、Shift、Alt 的顺序写在前面，其余按按下的顺序
    std::string s;
    for (int vk : {VK_CONTROL, VK_SHIFT, VK_MENU})
        if (std::find(vks.begin(), vks.end(), vk) != vks.end()) s += KeyToName(vk) + "+";
    std::vector<int> seen;
    for (int vk : vks) {
        if (IsModifier(vk) || KeyToName(vk).empty() || std::find(seen.begin(), seen.end(), vk) != seen.end()) continue;
        seen.push_back(vk);
        s += KeyToName(vk) + "+";
    }
    if (!s.empty()) s.pop_back();
    return s;
}

std::string PadComboSpec(unsigned mask, std::string& why)
{
    int n = 0;
    for (unsigned m = mask; m; m &= m - 1) ++n;
    if (n < 2) { why = "手柄至少要两个键一起按（单键会和游戏操作冲突）"; return std::string(); }
    return PadComboText(mask);
}

std::string KeyComboSpec(const std::vector<int>& vks, std::string& why)
{
    bool mod = false, main = false, chr = false;
    for (int vk : vks) {
        if (KeyToName(vk).empty()) continue;
        if (IsModifier(vk)) mod = true;
        else { main = true; chr |= IsCharKey(vk); }
    }
    if (!main) { why = "Ctrl / Shift / Alt 之外还要再按一个键"; return std::string(); }
    if (chr && !mod) { why = "字母、数字键要配 Ctrl / Shift / Alt（单按会和游戏操作冲突）"; return std::string(); }
    return KeyComboText(vks);
}
