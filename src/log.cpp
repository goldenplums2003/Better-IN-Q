#include "log.h"
#include "game.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace {
std::wstring g_logPath;
std::mutex g_logMx;
std::mutex g_chatMx;
std::deque<std::string> g_chat;
std::string g_last;
unsigned long long g_lastAt = 0;

typedef void (*SystemMessageFn)(void* mgr, const char* utf8, float duration,
                                std::int32_t messageId, bool emphasized);
} // namespace

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

void LogInit(const std::wstring& path)
{
    std::lock_guard<std::mutex> lk(g_logMx);
    g_logPath = path;
    ::DeleteFileW(path.c_str());
}

void Log(const char* fmt, ...)
{
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char line[1100];
    const int n = std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u %s\r\n",
                                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_logPath.empty()) return;
    HANDLE h = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    ::WriteFile(h, line, (DWORD)n, &wr, nullptr);
    ::CloseHandle(h);
}

void Chat(const std::string& utf8)
{
    Log("[chat] %s", utf8.c_str());
    std::lock_guard<std::mutex> lk(g_chatMx);
    if (g_chat.size() < 16) g_chat.push_back(utf8);
    static const std::string kPrefix = "Better-IN-Q：";   // 面板里不用再写一遍是谁说的
    g_last = utf8.compare(0, kPrefix.size(), kPrefix) == 0 ? utf8.substr(kPrefix.size()) : utf8;
    g_lastAt = ::GetTickCount64();
}

std::string LastChat(unsigned long long maxAgeMs)
{
    std::lock_guard<std::mutex> lk(g_chatMx);
    if (g_last.empty() || ::GetTickCount64() - g_lastAt > maxAgeMs) return std::string();
    return g_last;
}

void FlushChat()
{
    if (!game::InScene()) return;
    void* mgr = reinterpret_cast<void*>(game::P(game::kSystemMessageMgr));
    if (!mgr) return;
    auto fn = reinterpret_cast<SystemMessageFn>(game::kSystemMessage);
    for (;;) {
        std::string m;
        {
            std::lock_guard<std::mutex> lk(g_chatMx);
            if (g_chat.empty()) return;
            m = g_chat.front();
            g_chat.pop_front();
        }
        char buf[0x180] = {};   // 游戏那边最多拷 0x17F 字节
        std::snprintf(buf, sizeof(buf), "%s", m.c_str());
        fn(mgr, buf, 0.0f, -1, false);
    }
}

// ImGui 的断言失败（见 imgui_user.h）：同一处可能每帧都失败，每处只记一次，最多记 16 处
void ImGuiAssertFailed(const char* expr, const char* file, int line)
{
    static std::mutex mx;
    static std::vector<std::pair<std::string, int>> seen;
    {
        std::lock_guard<std::mutex> lk(mx);
        if (seen.size() >= 16) return;
        for (const auto& s : seen)
            if (s.second == line && s.first == file) return;
        seen.emplace_back(file, line);
    }
    const char* base = file;
    for (const char* p = file; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    Log("imgui: 断言失败（跳过了，接着跑）%s:%d  %s", base, line, expr);
}
