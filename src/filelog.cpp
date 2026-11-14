#include "filelog.h"

#include "hook.h"
#include "log.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <mutex>

namespace filelog {
namespace {

typedef HANDLE(WINAPI* CreateWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef DWORD(WINAPI* AttrWFn)(LPCWSTR);
typedef DWORD(WINAPI* AttrAFn)(LPCSTR);
typedef HANDLE(WINAPI* FindWFn)(LPCWSTR, LPWIN32_FIND_DATAW);
typedef HANDLE(WINAPI* FindExWFn)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);

// 原函数（GetProcAddress 拿的）。换指针时要跳过这张表本身，理由同 padfilter
struct Orig {
    CreateWFn createW;
    CreateAFn createA;
    AttrWFn attrW;
    AttrAFn attrA;
    FindWFn findW;
    FindExWFn findExW;
} g_orig = {};

struct Rec {
    SYSTEMTIME t;
    const char* api;
    std::wstring path;
    bool ok;
};
std::mutex g_mx;
std::vector<Rec> g_recs;
unsigned g_other = 0;           // 别的 nativePC 查询，攒着按秒报
ULONGLONG g_otherSince = 0;
bool g_installed = false;

bool Contains(const std::wstring& s, const wchar_t* needle)
{
    const std::size_t n = std::wcslen(needle);
    for (std::size_t i = 0; i + n <= s.size(); ++i) {
        std::size_t k = 0;
        while (k < n && std::towlower(s[i + k]) == needle[k]) ++k;
        if (k == n) return true;
    }
    return false;
}

// 记一笔。跑在游戏刚调完的文件函数后面：游戏接着可能要看 GetLastError，记录过程里别把它改了
struct KeepLastError {
    DWORD e = ::GetLastError();
    ~KeepLastError() { ::SetLastError(e); }
};

void Note(const char* api, const std::wstring& path, bool ok)
{
    KeepLastError keep;
    const bool sound = Contains(path, L"sound");
    if (!sound && !Contains(path, L"nativepc")) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!sound) {
        if (!g_other) g_otherSince = ::GetTickCount64();
        ++g_other;
        return;
    }
    if (g_recs.size() >= 4096) return;   // 主循环卡住时别无限攒
    Rec r;
    ::GetLocalTime(&r.t);
    r.api = api;
    r.path = path.size() > 260 ? path.substr(0, 260) : path;
    r.ok = ok;
    g_recs.push_back(std::move(r));
}

std::wstring Widen(LPCSTR s)
{
    if (!s) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) ::MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

HANDLE WINAPI HkCreateW(LPCWSTR p, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
{
    const HANDLE h = g_orig.createW(p, a, sh, sa, cd, fl, t);
    if (p) Note("打开", p, h != INVALID_HANDLE_VALUE);
    return h;
}
HANDLE WINAPI HkCreateA(LPCSTR p, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
{
    const HANDLE h = g_orig.createA(p, a, sh, sa, cd, fl, t);
    KeepLastError keep;   // Widen 里的 MultiByteToWideChar 也会动它
    if (p) Note("打开", Widen(p), h != INVALID_HANDLE_VALUE);
    return h;
}
DWORD WINAPI HkAttrW(LPCWSTR p)
{
    const DWORD r = g_orig.attrW(p);
    if (p) Note("查属性", p, r != INVALID_FILE_ATTRIBUTES);
    return r;
}
DWORD WINAPI HkAttrA(LPCSTR p)
{
    const DWORD r = g_orig.attrA(p);
    KeepLastError keep;
    if (p) Note("查属性", Widen(p), r != INVALID_FILE_ATTRIBUTES);
    return r;
}
HANDLE WINAPI HkFindW(LPCWSTR p, LPWIN32_FIND_DATAW d)
{
    const HANDLE h = g_orig.findW(p, d);
    if (p) Note("查找", p, h != INVALID_HANDLE_VALUE);
    return h;
}
HANDLE WINAPI HkFindExW(LPCWSTR p, FINDEX_INFO_LEVELS lv, LPVOID d, FINDEX_SEARCH_OPS op, LPVOID f, DWORD fl)
{
    const HANDLE h = g_orig.findExW(p, lv, d, op, f, fl);
    if (p) Note("查找", p, h != INVALID_HANDLE_VALUE);
    return h;
}

} // namespace

bool Install(std::string& info)
{
    if (g_installed) return true;
    HMODULE k = ::GetModuleHandleW(L"kernel32.dll");
    if (!k) { info = "kernel32 没加载？"; return false; }
    if (!g_orig.createW) {   // 先填好原函数再换指针：换完的下一瞬间游戏就可能调进来
        g_orig.createW = reinterpret_cast<CreateWFn>(::GetProcAddress(k, "CreateFileW"));
        g_orig.createA = reinterpret_cast<CreateAFn>(::GetProcAddress(k, "CreateFileA"));
        g_orig.attrW = reinterpret_cast<AttrWFn>(::GetProcAddress(k, "GetFileAttributesW"));
        g_orig.attrA = reinterpret_cast<AttrAFn>(::GetProcAddress(k, "GetFileAttributesA"));
        g_orig.findW = reinterpret_cast<FindWFn>(::GetProcAddress(k, "FindFirstFileW"));
        g_orig.findExW = reinterpret_cast<FindExWFn>(::GetProcAddress(k, "FindFirstFileExW"));
    }
    const std::uintptr_t exe = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    const struct { const char* name; void* fn; void* hk; } fns[] = {
        {"CreateFileW", reinterpret_cast<void*>(g_orig.createW), reinterpret_cast<void*>(HkCreateW)},
        {"CreateFileA", reinterpret_cast<void*>(g_orig.createA), reinterpret_cast<void*>(HkCreateA)},
        {"GetFileAttributesW", reinterpret_cast<void*>(g_orig.attrW), reinterpret_cast<void*>(HkAttrW)},
        {"GetFileAttributesA", reinterpret_cast<void*>(g_orig.attrA), reinterpret_cast<void*>(HkAttrA)},
        {"FindFirstFileW", reinterpret_cast<void*>(g_orig.findW), reinterpret_cast<void*>(HkFindW)},
        {"FindFirstFileExW", reinterpret_cast<void*>(g_orig.findExW), reinterpret_cast<void*>(HkFindExW)},
    };
    static int hits[6] = {};   // 累计：没装全会重试，前几次换掉的不会再被找到
    int total = 0;
    info.clear();
    for (int i = 0; i < 6; ++i) {
        if (!fns[i].fn) continue;
        std::string where;
        const int n = hook::ReplaceModulePointers(exe, fns[i].fn, fns[i].hk, &g_orig, &g_orig + 1, &where);
        if (n) Log("filelog: 换掉 %s处的 %s", where.c_str(), fns[i].name);
        hits[i] += n;
        total += hits[i];
        info += std::string(i ? "，" : "") + fns[i].name + " " + std::to_string(hits[i]) + " 处";
    }
    // 打开文件那两个找到了就算装好（查属性、查找可能游戏根本不用）
    g_installed = hits[0] || hits[1];
    (void)total;
    return g_installed;
}

bool Installed() { return g_installed; }

std::vector<std::string> Drain()
{
    std::vector<Rec> recs;
    unsigned other = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        recs.swap(g_recs);
        if (g_other && ::GetTickCount64() - g_otherSince >= 1000) {
            other = g_other;
            g_other = 0;
        }
    }
    std::vector<std::string> out;
    for (const Rec& r : recs) {
        char head[64];
        std::snprintf(head, sizeof(head), "文件 %02u:%02u:%02u.%03u %s%s ", r.t.wHour, r.t.wMinute, r.t.wSecond,
                      r.t.wMilliseconds, r.api, r.ok ? "" : "（没有）");
        out.push_back(head + ToUtf8(r.path));
    }
    if (other) out.push_back("文件 这一秒别的 nativePC 查询 " + std::to_string(other) + " 次");
    return out;
}

} // namespace filelog
