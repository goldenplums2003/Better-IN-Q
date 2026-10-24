// ============================================================================
//  Better-IN-Q —— MHW:IB 的快速重开任务，做成游戏插件（Stracker's Loader 加载）
//
//  核心逻辑移植自 INQ.CT：原作者 年糕狗三世（B 站 BV1yz2PBNEWn），作者允许
//  修 bug / 加功能后重新分发；CT 的中文化与修改由 QCanon 完成。
// ============================================================================
#include <windows.h>
#include <mutex>

#include "config.h"
#include "cursorfilter.h"
#include "inq.h"
#include "kbmfilter.h"
#include "input.h"
#include "log.h"
#include "overlay.h"
#include "padfilter.h"
#include "ui.h"

namespace {

HMODULE g_module = nullptr;
volatile LONG g_stop = 0;
const char* const kVersion = "0.6.9";

// 主循环线程和游戏渲染线程（画面板）共用，都要先拿这把锁
std::mutex g_mx;
Config g_cfg;
ui::Model g_model;
bool g_refreshNames = false;

std::wstring ModuleDir()
{
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(g_module, buf, MAX_PATH);
    std::wstring s(buf);
    const std::size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : s.substr(0, p + 1);
}

void LogHotkey(const char* what, const std::string& spec, const Hotkey& hk)
{
    Log("hotkey %s = \"%s\"（%zu 组）%s%s", what, spec.c_str(), hk.alts.size(),
        hk.bad.empty() ? "" : " 认不出：", hk.bad.c_str());
}

// 面板组合键里的手柄按钮（给 padfilter 扣住用）
unsigned short PadButtons(const Hotkey& hk)
{
    for (const Combo& c : hk.alts)
        if (c.IsPad() && c.keys.empty()) return c.pad;
    return 0;
}

// ---- 渲染线程里的回调 ----
void OnOverlayInit(float scale)
{
    std::lock_guard<std::mutex> lk(g_mx);
    ui::Init(scale);
}
void OnOverlayOpened()
{
    std::lock_guard<std::mutex> lk(g_mx);
    ui::OnOpen(g_model);
    g_refreshNames = true;
}
void OnOverlayDraw(float w, float h)
{
    std::lock_guard<std::mutex> lk(g_mx);
    ui::Draw(g_model, w, h);
    overlay::SetImeWanted(g_model.wantIme);   // 在打名字：把输入法挂回来，能打中文
}

struct Hotkeys {
    Hotkey restart, resetOnly, panel;
    std::string specs[3];
    bool parsed = false;
    // 存盘时也会调：快捷键没变就不重新解析（按键的按下状态要保留，不然按着时存盘会误触发）
    void Parse(const Config& c)
    {
        if (parsed && specs[0] == c.hkRestart && specs[1] == c.hkResetOnly && specs[2] == c.hkPanel) return;
        parsed = true;
        specs[0] = c.hkRestart;
        specs[1] = c.hkResetOnly;
        specs[2] = c.hkPanel;
        restart = ParseHotkey(c.hkRestart);
        resetOnly = ParseHotkey(c.hkResetOnly);
        panel = ParseHotkey(c.hkPanel);
        LogHotkey("Restart", c.hkRestart, restart);
        LogHotkey("ResetOnly", c.hkResetOnly, resetOnly);
        LogHotkey("Panel", c.hkPanel, panel);
        padfilter::SetChord(PadButtons(panel));
    }
};

bool g_blocked = false;   // 现在是不是在挡游戏的输入（面板开着）

void SetPanel(bool open)
{
    overlay::SetOpen(open);
    g_blocked = overlay::IsOpen();
    padfilter::SetBlock(g_blocked);
    cursorfilter::SetBlock(g_blocked);
    kbmfilter::SetBlock(g_blocked);
}

// 手柄、光标过滤：游戏的 XInput 可能晚一点才加载，装不上就隔两秒再试，最多试 2 分钟
struct Filter {
    const char* name;
    bool (*install)(std::string&);
    bool (*installed)();
    int tries = 0;
    ULONGLONG last = 0;
    void Try(ULONGLONG now)
    {
        if (installed() || tries >= 60 || now - last <= 2000) return;
        last = now;
        ++tries;
        std::string info;
        const bool ok = install(info);
        if (ok || tries == 1 || tries == 60) Log("%s: %s", name, info.c_str());
        if (ok && overlay::IsOpen()) SetPanel(true);   // 面板已经开着：补上拦截
    }
};

DWORD WINAPI Worker(LPVOID)
{
    while (!::GetModuleHandleW(L"MonsterHunterWorld.exe") && !g_stop) ::Sleep(200);
    if (g_stop) return 0;
    inq::ReserveArena();

    const std::wstring dir = ModuleDir() + L"BetterINQ\\";
    const std::wstring cfgPath = dir + L"BetterINQ.ini";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    LogInit(dir + L"BetterINQ.log");
    Log("Better-IN-Q %s starting", kVersion);

    Hotkeys hk;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        std::string err;
        if (!LoadConfig(cfgPath, g_cfg, err)) Log("config: %s", err.c_str());
        Log("config: QuestID=%d Reset=%d AcceptOnly=%d Wyvern=%d ActiveSet='%s' Food=%d 全套%zu 猫饭%zu",
            g_cfg.questId, g_cfg.reset, g_cfg.acceptOnly, g_cfg.wyvern, g_cfg.activeSet.c_str(), g_cfg.food,
            g_cfg.sets.size(), g_cfg.foods.size());
        g_model.cfg = &g_cfg;
        g_model.version = kVersion;
        hk.Parse(g_cfg);
    }

    Input in;
    if (!in.Init()) Log("XInput 没加载到，手柄快捷键不可用");

    std::string report;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        ok = inq::Install(g_cfg, report);
        g_model.ready = ok;
    }
    if (!report.empty()) Log("install report:\n%s", report.c_str());

    overlay::Callbacks cb;
    cb.init = OnOverlayInit;
    cb.opened = OnOverlayOpened;
    cb.draw = OnOverlayDraw;
    std::string err;
    const bool panelOk = overlay::Install(cb, err);
    if (!panelOk) Log("overlay: %s", err.c_str());

    if (ok) Chat(std::string("Better-IN-Q ") + kVersion + " 已就绪");
    else Chat("Better-IN-Q：重开功能没装上，详情见 BetterINQ.log");

    Filter filters[] = {{"padfilter", padfilter::Install, padfilter::Installed},
                        {"cursorfilter", cursorfilter::Install, cursorfilter::Installed},
                        {"kbmfilter", kbmfilter::Install, kbmfilter::Installed}};
    ULONGLONG lastTick = 0, dirtyAt = 0;
    while (!g_stop) {
        const ULONGLONG now = ::GetTickCount64();
        for (Filter& f : filters) f.Try(now);

        in.Poll();
        bool capturing;   // 面板正在录快捷键：按的键是要录的，不当快捷键用
        {
            std::lock_guard<std::mutex> lk(g_mx);
            capturing = g_model.capturing && overlay::IsOpen();
            g_model.padHeld = in.PadHeld();
            if (capturing) g_model.keysHeld = KeysHeld();
            else g_model.keysHeld.clear();
        }
        if (in.Fired(hk.panel) && !capturing) {
            if (overlay::Failed() || !panelOk) Chat("Better-IN-Q：面板画不出来，详情见 BetterINQ.log（重开快捷键照常能用）");
            else SetPanel(!overlay::IsOpen());
        }
        // 面板刚打开就发现画不出来：别一直挡着手柄和鼠标
        if (g_blocked && overlay::Failed()) {
            SetPanel(false);
            Chat("Better-IN-Q：面板画不出来，详情见 BetterINQ.log（重开快捷键照常能用）");
        }
        const bool open = overlay::IsOpen();
        const bool restartKey = in.Fired(hk.restart);       // 面板开着时不响应，免得在面板里误触
        const bool resetKey = in.Fired(hk.resetOnly);

        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (!open && restartKey) { Log("hotkey: restart"); padfilter::Trace(30000); inq::RequestRestart(g_cfg); }
            if (!open && resetKey) { Log("hotkey: reset only"); padfilter::Trace(30000); inq::RequestRestart(g_cfg, true); }

            ui::Model& m = g_model;
            if (m.reqClose) { m.reqClose = false; SetPanel(false); }
            if (m.reqRestart) { m.reqRestart = false; Log("panel: restart"); padfilter::Trace(30000); inq::RequestRestart(g_cfg); }
            if (m.reqSave) {             // 设置马上生效；写盘等停手 0.4 秒，免得打一个字存一次
                m.reqSave = false;
                inq::ApplyOptions(g_cfg);
                dirtyAt = now;
            }
            if (dirtyAt && now - dirtyAt > 400) {
                dirtyAt = 0;
                if (!SaveConfig(cfgPath, g_cfg)) Log("config: 存盘失败");
                hk.Parse(g_cfg);
            }
            if (g_refreshNames) {
                g_refreshNames = false;
                m.equipNames.clear();
                m.itemNames.clear();
                for (const auto& e : inq::ListEquipLoadouts()) m.equipNames.push_back({e.number, e.name});
                for (const auto& it : inq::ListItemLoadouts()) m.itemNames.push_back({it.number, it.name});
            }
            if (now - lastTick >= 100) {
                lastTick = now;
                inq::Tick(g_cfg);
                m.status = LastChat(6000);
            }
        }
        padfilter::FlushTrace();
        FlushChat();
        ::Sleep(16);
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) BOOL BetterINQ_IsInstalled() { return TRUE; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        ::DisableThreadLibraryCalls(module);
        HANDLE t = ::CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) ::CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        ::InterlockedExchange(&g_stop, 1);
    }
    return TRUE;
}
