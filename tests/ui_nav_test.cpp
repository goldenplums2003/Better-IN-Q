// 面板的手柄导航：模拟十字键 / A / B，看页面和焦点是不是按设计走。离屏渲染，不开窗口。
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <vector>
#include <xinput.h>

#include "../src/config.h"
#include "../src/gamedata.h"
#include "../src/input.h"
#include "../src/ui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

static ID3D11Device* dev = nullptr;
static ID3D11DeviceContext* ctx = nullptr;
static ID3D11RenderTargetView* rtv = nullptr;
static ui::Model m;
static std::vector<int> g_sideTrace;   // 每帧焦点在侧栏第几格

static void Frame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ui::Draw(m, io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_sideTrace.push_back(ui::DebugSidebarFocusIndex());
}

static float PanelH() { return ui::DebugFrameHeight(); }

// 录键：手柄 / 键盘按住一组，再全部松开
static void Hold(unsigned pad, std::vector<int> keys = {})
{
    m.padHeld = pad;
    m.keysHeld = keys;
    for (int i = 0; i < 3; ++i) Frame();
}

static void Press(ImGuiKey k)
{
    ImGui::GetIO().AddKeyEvent(k, true);
    Frame();
    ImGui::GetIO().AddKeyEvent(k, false);
    Frame();
    Frame();
}

static int ActiveIndex(const Config& c)
{
    for (int i = 0; i < (int)c.sets.size(); ++i)
        if (c.sets[i].name == c.activeSet) return i;
    return -1;
}

static const char* PageName(int p)
{
    static const char* n[] = {"主页", "换任务", "套装", "套装编辑", "猫饭", "猫饭编辑", "设置"};
    return p >= 0 && p < 7 ? n[p] : "?";
}

int main()
{
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1920; td.Height = 1080; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rt = nullptr;
    dev->CreateTexture2D(&td, nullptr, &rt);
    dev->CreateRenderTargetView(rt, nullptr, &rtv);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui_ImplDX11_Init(dev, ctx);
    ui::Init(1.0f);

    Config cfg;
    std::string err;
    LoadConfig(L"config\\BetterINQ.ini", cfg, err);
    m.cfg = &cfg;
    m.ready = true;

    Frame();
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    CHECK(ui::DebugPage() == 0 && !ui::DebugSidebarFocused(), "打开面板：在主页，焦点在内容区（%s）", PageName(ui::DebugPage()));

    g_sideTrace.clear();
    Press(ImGuiKey_GamepadDpadLeft);
    CHECK(ui::DebugSidebarFocused() && ui::DebugPage() == 0, "按左：进侧栏，还停在「任务」这一格，没乱切页（%s）", PageName(ui::DebugPage()));
    {
        bool clean = true;
        for (int i : g_sideTrace) clean &= (i == -1 || i == 0);
        CHECK(clean, "按左的过程中焦点没在别的格上停过（之前会先闪到「猫饭」）");
    }

    Press(ImGuiKey_GamepadDpadDown);
    CHECK(ui::DebugPage() == 2 && ui::DebugSidebarFocused(), "按下：切到「套装」，焦点留在侧栏（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadDpadDown);
    CHECK(ui::DebugPage() == 4, "再按下：切到「猫饭」（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadDpadUp);
    CHECK(ui::DebugPage() == 2, "按上：回到「套装」（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadDpadRight);
    CHECK(!ui::DebugSidebarFocused() && ui::DebugPage() == 2, "按右：回到内容区（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceDown);
    CHECK(ui::DebugPage() == 3, "按 A：打开第一套的编辑页（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceRight);
    CHECK(ui::DebugPage() == 2, "按 B：回到套装列表（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceRight);
    CHECK(ui::DebugPage() == 0 && !m.reqClose, "再按 B：回主页（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceRight);
    CHECK(m.reqClose, "主页按 B：请求关面板");
    m.reqClose = false;

    // 侧栏里按 A：切页并把焦点送进内容区
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadDpadLeft);
    Press(ImGuiKey_GamepadDpadDown);
    Press(ImGuiKey_GamepadDpadDown);
    Press(ImGuiKey_GamepadDpadDown);
    CHECK(ui::DebugPage() == 6, "侧栏里一路往下到「设置」（%s）", PageName(ui::DebugPage()));
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(ui::DebugPage() == 6 && !ui::DebugSidebarFocused(), "侧栏里按 A：焦点进内容区（%s）", PageName(ui::DebugPage()));

    // 第 4 条 bug 的回归：主页换任务。「米拉」套装自己带着任务（66845），重开时套装的任务说了算，
    // 所以主页换任务要改套装里的那个，主页显示的也要跟着变
    {
        const int globalBefore = cfg.questId;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadDpadDown);    // 重开 → 下面一排圆按钮（落在中间那个）
        Press(ImGuiKey_GamepadDpadLeft);    // → 换任务
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 1, "主页：下、左、A 进「换任务」（%s）", PageName(ui::DebugPage()));
        Press(ImGuiKey_GamepadDpadUp);      // 66845 上面一个是 66846
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 0, "选中后回到主页（%s）", PageName(ui::DebugPage()));
        CHECK(cfg.FindSet("米拉") && cfg.FindSet("米拉")->quest == 66846, "改的是「米拉」套装里的任务（现在 %d）",
              cfg.FindSet("米拉") ? cfg.FindSet("米拉")->quest : -1);
        CHECK(cfg.questId == globalBefore, "全局任务没被误改（%d）", cfg.questId);
        CHECK(m.reqSave, "提了存盘请求");
        m.reqSave = false;
    }

    // 往左：内容区里左边还有控件就照常移动；没有了才进侧栏，而且落在当前页那一格
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadDpadDown);    // 重开 → 中间的「换套装」
    Press(ImGuiKey_GamepadDpadRight);   // → 「换猫饭」
    Press(ImGuiKey_GamepadDpadLeft);
    CHECK(!ui::DebugSidebarFocused(), "主页「换猫饭」往左：留在内容区（到「换套装」）");
    Press(ImGuiKey_GamepadDpadLeft);
    Press(ImGuiKey_GamepadDpadLeft);
    CHECK(ui::DebugSidebarFocused() && ui::DebugSidebarFocusIndex() == 0 && ui::DebugPage() == 0,
          "再往左两次：进侧栏「任务」（第 %d 格）", ui::DebugSidebarFocusIndex());
    ui::DebugGo(6, 0);
    for (int i = 0; i < 4; ++i) Frame();
    for (int i = 0; i < 6; ++i) Press(ImGuiKey_GamepadDpadDown);
    g_sideTrace.clear();
    Press(ImGuiKey_GamepadDpadLeft);
    {
        bool clean = true;
        for (int i : g_sideTrace) clean &= (i == -1 || i == 3);
        CHECK(clean && ui::DebugSidebarFocusIndex() == 3 && ui::DebugPage() == 6,
              "设置页中间往左：直接到侧栏「设置」（第 %d 格，%s）", ui::DebugSidebarFocusIndex(), PageName(ui::DebugPage()));
    }

    // 换页时面板高度是渐变的
    {
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(6, 0);
        for (int i = 0; i < 40; ++i) Frame();
        const float tall = PanelH();
        ui::DebugGo(0, 0);
        Frame();
        const float h1 = PanelH();
        for (int i = 0; i < 40; ++i) Frame();
        const float shortH = PanelH();
        CHECK(tall > shortH + 50 && h1 < tall && h1 > shortH + 20,
              "设置 → 主页：高度 %.0f → %.0f（第一帧 %.0f，中间有过渡）", tall, shortH, h1);
    }

    // 录快捷键
    {
        Config& c = cfg;
        c.hkPanel = "View+Menu | Insert";
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugStartCapture(m, 1, true);
        Hold(XINPUT_GAMEPAD_A);               // 按 A 开始录的那一下还按着：不算
        CHECK(m.capturing, "开始录手柄：主循环知道在录（%d）", (int)m.capturing);
        Hold(0);
        Hold(XINPUT_GAMEPAD_LEFT_SHOULDER);
        Hold(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
        Hold(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | kPadLT);
        Hold(0);
        CHECK(c.hkPanel == "LB+RB+LT | Insert" && !m.capturing && m.reqSave,
              "按 LB、RB、LT 再松开：手柄那组换成 LB+RB+LT，键盘那组不动（%s）", c.hkPanel.c_str());
        m.reqSave = false;

        ui::DebugStartCapture(m, 1, true);
        Hold(0);
        Hold(XINPUT_GAMEPAD_Y);
        Hold(0);
        CHECK(m.capturing && c.hkPanel == "LB+RB+LT | Insert" && !ui::DebugCaptureWhy().empty(),
              "只按一个 Y：不收，说明原因、接着录（%s）", ui::DebugCaptureWhy().c_str());
        // 录手柄时按 B：是组合里的键，不能被当成「返回」
        ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
        Hold(XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_Y);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
        Hold(0);
        CHECK(c.hkPanel == "B+Y | Insert" && ui::DebugPage() == 6, "B+Y 也能录，按 B 没把页面退回去（%s，%s）",
              c.hkPanel.c_str(), PageName(ui::DebugPage()));

        ui::DebugStartCapture(m, 1, false);
        Hold(0);
        Hold(0, {'R'});
        Hold(0);
        CHECK(m.capturing && c.hkPanel == "B+Y | Insert", "键盘单按 R：不收（%s）", ui::DebugCaptureWhy().c_str());
        Hold(0, {VK_CONTROL});
        Hold(0, {VK_CONTROL, 'R'});
        Hold(0);
        CHECK(c.hkPanel == "B+Y | Ctrl+R" && !m.capturing, "Ctrl+R：键盘那组换掉（%s）", c.hkPanel.c_str());

        // 清除：录键盘时单按 Delete，录手柄时单按 X；录手柄时按键盘 Delete 也算清除
        ui::DebugStartCapture(m, 1, false);
        Hold(0);
        Hold(0, {VK_DELETE});
        Hold(0);
        CHECK(c.hkPanel == "B+Y" && !m.capturing, "录键盘时单按 Delete：键盘那组清掉，不是把 Delete 设成快捷键（%s）",
              c.hkPanel.c_str());
        ui::DebugStartCapture(m, 1, true);
        Hold(0);
        Hold(XINPUT_GAMEPAD_X);
        Hold(0);
        CHECK(c.hkPanel.empty() && !m.capturing, "录手柄时单按 X：手柄那组清掉（「%s」）", c.hkPanel.c_str());
        c.hkPanel = "View+Menu | Insert";
        ui::DebugStartCapture(m, 1, true);
        Hold(0);
        Press(ImGuiKey_Delete);
        CHECK(c.hkPanel == "Insert" && !m.capturing, "录手柄时按键盘 Delete：手柄那组清掉（%s）", c.hkPanel.c_str());
        ui::DebugStartCapture(m, 1, false);
        Hold(0);
        Hold(0, {VK_CONTROL});
        Hold(0, {VK_CONTROL, VK_DELETE});
        Hold(0);
        CHECK(c.hkPanel == "Ctrl+Delete", "Ctrl+Delete 照样能录（%s）", c.hkPanel.c_str());
        c.hkPanel = "View+Menu | Insert";

        ui::DebugStartCapture(m, 0, false);
        Hold(0);
        const std::string before = c.hkRestart;
        Press(ImGuiKey_Escape);
        CHECK(!m.capturing && c.hkRestart == before && ui::DebugPage() == 6, "Esc：取消录制，快捷键不变、也没退页（%s）",
              PageName(ui::DebugPage()));
        Press(ImGuiKey_Escape);
        CHECK(ui::DebugPage() == 0, "不在录的时候 Esc 照常返回（%s）", PageName(ui::DebugPage()));
        m.reqSave = false;
    }

    // 套装 / 猫饭：列表里只有套装 / 猫饭本身，点进去按「确认」才选这一套
    {
        const std::string setBefore = cfg.activeSet;
        const int foodBefore = cfg.food;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        cfg.activeSet.clear();
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadDpadUp);           // 焦点在第一套上，上面没有别的行了
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 3 && cfg.activeSet.empty(), "套装列表第一行就是套装：点进编辑页，还没选它（%s）",
              PageName(ui::DebugPage()));
        for (int i = 0; i < 5; ++i) Press(ImGuiKey_GamepadDpadDown);   // 装备 道具 联动 猫饭 任务 → 确认
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 2 && cfg.activeSet == setBefore, "编辑页按「确认」：选上这套并回列表（%s，%s）",
              cfg.activeSet.c_str(), PageName(ui::DebugPage()));

        cfg.food = 0;
        ui::DebugGo(4, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadDpadUp);
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 5 && cfg.food == 0, "猫饭列表第一行就是猫饭：点进编辑页，还没选它（%s）",
              PageName(ui::DebugPage()));
        for (int i = 0; i < 8; ++i) Press(ImGuiKey_GamepadDpadDown);   // 五排档位、三个技能 → 确认
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 4 && cfg.food == foodBefore, "编辑页按「确认」：每次进任务都吃这份（%d，%s）", cfg.food,
              PageName(ui::DebugPage()));

        // 套装编辑页的下拉第一项也是「—」：选它 = 这一项不换
        LoadoutSet* mila = const_cast<LoadoutSet*>(cfg.FindSet("米拉"));
        const std::string equipBefore = mila ? mila->equip : std::string();
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceDown);          // 打开装备套装下拉，焦点在当前项（测试里没有游戏套装，就是「—」）
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(mila && mila->equip.empty() && ui::DebugPage() == 3, "装备套装下拉选「—」：清空（原来「%s」）", equipBefore.c_str());
        if (mila) mila->equip = equipBefore;
        m.reqSave = false;
    }

    // 下拉选项里按 B：只关下拉，不退页
    {
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceDown);          // 焦点默认在「装备套装」，A 打开下拉
        CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), "套装编辑页按 A：打开装备套装的下拉");
        Press(ImGuiKey_GamepadFaceRight);
        CHECK(!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && ui::DebugPage() == 3,
              "下拉里按 B：关掉下拉，还在套装编辑页（%s）", PageName(ui::DebugPage()));
        Press(ImGuiKey_GamepadFaceRight);
        CHECK(ui::DebugPage() == 2, "再按 B：才回套装列表（%s）", PageName(ui::DebugPage()));
    }

    // 搜任务：拼音、首字母、混着打、多音字、ID 开头
    {
        const QuestInfo* q = gamedata::FindQuest(66845);   // 传说中的黑龙
        const QuestInfo* w = gamedata::FindQuest(66846);   // 最后的白骑士
        CHECK(q && w, "找到两个任务");
        if (q && w) {
            const char* yes[] = {"heilong", "hl", "HeiLong", "hlong", "heil", "chuanshuo", "cszdhl", "zhongdehei", "zhuanshuo",
                                 "chuan shuo", "668", "66845", "黑龙"};
            for (const char* n : yes) CHECK(gamedata::QuestMatches(*q, n), "「传说中的黑龙」能用「%s」搜到", n);
            const char* no[] = {"bailong", "hlz", "66846", "白", "xyz"};
            for (const char* n : no) CHECK(!gamedata::QuestMatches(*q, n), "「传说中的黑龙」用「%s」搜不到", n);
            CHECK(gamedata::QuestMatches(*w, "bqs") && gamedata::QuestMatches(*w, "zuihou"), "「最后的白骑士」：bqs、zuihou");
        }
    }

    // 任务选「无」：全局列表「当前任务」下面一行；选了之后主页的大按钮就是原地套用
    {
        const int questBefore = cfg.questId;
        const std::string setBefore = cfg.activeSet;
        cfg.activeSet.clear();                    // 套装里的任务会盖过全局的，先不用套装
        cfg.questId = 0;                          // 焦点会落在「当前任务」上
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(1, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadDpadDown);          // 当前任务 → 无
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(cfg.questId == kQuestNone && ui::DebugPage() == 0 && m.reqSave,
              "换任务里选「无」：全局任务 = %d，回到主页（%s）", cfg.questId, PageName(ui::DebugPage()));
        CHECK(cfg.EffectiveQuest() == kQuestNone, "实际任务 =「无」");
        m.reqSave = false;
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(m.reqRestart && m.reqClose, "主页按 A：照样发「重开」请求（选了「无」时主循环那边只原地套用）");
        m.reqRestart = m.reqClose = false;

        // 套装里也能选「无」，它会盖过全局的
        cfg.questId = 66845;
        cfg.activeSet = setBefore;
        LoadoutSet* mila = const_cast<LoadoutSet*>(cfg.FindSet(setBefore));
        if (mila) {
            const int setQuest = mila->quest;
            mila->quest = kQuestNone;
            CHECK(cfg.EffectiveQuest() == kQuestNone, "套装里的任务是「无」：实际任务也是「无」");
            mila->quest = setQuest;
        }
        cfg.questId = questBefore;
        cfg.activeSet = setBefore;
    }

    // 猫饭改名：名字框打字时要输入法，打完存进配置；清空就回到「猫饭 编号」
    {
        const int fid = cfg.foods.begin()->first;
        const std::string nameBefore = cfg.foods[fid].name;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        CHECK(!m.wantIme, "平时面板里不要输入法");
        ui::DebugGo(5, fid);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugFocusName();
        Frame();
        Frame();
        CHECK(m.wantIme, "名字框在打字：要输入法（能打中文）");
        ImGui::GetIO().AddInputCharactersUTF8("黑龙;饭");
        Frame();
        Frame();
        CHECK(cfg.foods[fid].name == "黑龙饭" && m.reqSave, "打「黑龙;饭」：存成「%s」（分号是配置文件的注释符，不收）",
              cfg.foods[fid].name.c_str());
        CHECK(cfg.FoodName(fid) == "黑龙饭", "列表、主页、套装里显示的名字跟着变（%s）", cfg.FoodName(fid).c_str());
        Press(ImGuiKey_Enter);
        CHECK(!m.wantIme && ui::DebugPage() == 5, "回车：打完了，不再要输入法，还在猫饭编辑页（%s）", PageName(ui::DebugPage()));
        ui::DebugFocusName();
        Frame();
        Frame();
        for (int i = 0; i < 4; ++i) Press(ImGuiKey_Backspace);
        CHECK(cfg.foods[fid].name.empty() && cfg.FoodName(fid) == "猫饭 " + std::to_string(fid),
              "删光：回到默认名「%s」", cfg.FoodName(fid).c_str());
        Press(ImGuiKey_Enter);
        cfg.foods[fid].name = nameBefore;
        m.reqSave = false;
    }

    // 套装改名：打完才生效；正在用的那套改了名，「重开时套用」跟着改；空的、重名的不改
    {
        const int idx = ActiveIndex(cfg);
        CHECK(idx >= 0, "测试配置里有正在用的套装");
        if (idx >= 0) {
            const std::string oldName = cfg.sets[idx].name;
            LoadoutSet other;
            other.name = "别的套装";
            cfg.sets.push_back(other);
            ui::OnOpen(m);
            for (int i = 0; i < 4; ++i) Frame();
            ui::DebugGo(3, idx);
            for (int i = 0; i < 4; ++i) Frame();
            ui::DebugFocusName();
            Frame();
            Frame();
            CHECK(m.wantIme, "套装名字框在打字：要输入法");
            const std::string renamed = "新套装名";
            for (int i = 0; i < 12; ++i) Press(ImGuiKey_Backspace);   // 获得焦点时原名是全选的，删一下就空了，多删几下也没事
            ImGui::GetIO().AddInputCharactersUTF8(renamed.c_str());
            Frame();
            Frame();
            CHECK(cfg.sets[idx].name == oldName, "打字中：还没改名（%s）", cfg.sets[idx].name.c_str());
            Press(ImGuiKey_Enter);
            CHECK(cfg.sets[idx].name == renamed && cfg.activeSet == renamed && m.reqSave && !m.wantIme,
                  "回车：改成「%s」，重开时套用的也跟着改（%s）", cfg.sets[idx].name.c_str(), cfg.activeSet.c_str());
            m.reqSave = false;

            // 删光再回车：不能为空，不改，框里退回原名
            ui::DebugFocusName();
            Frame();
            Frame();
            for (int i = 0; i < 12; ++i) Press(ImGuiKey_Backspace);
            CHECK(ui::DebugNameWarn() == "名字不能为空", "删光：提示「%s」", ui::DebugNameWarn().c_str());
            Press(ImGuiKey_Enter);
            CHECK(cfg.sets[idx].name == renamed && !m.reqSave, "空着回车：不改，还叫「%s」", cfg.sets[idx].name.c_str());

            // 和别的套装重名：不改
            ui::DebugFocusName();
            Frame();
            Frame();
            for (int i = 0; i < 12; ++i) Press(ImGuiKey_Backspace);
            ImGui::GetIO().AddInputCharactersUTF8("别的套装");
            Frame();
            Frame();
            CHECK(ui::DebugNameWarn().find("别的套装") != std::string::npos, "打成别的套装的名字：提示「%s」",
                  ui::DebugNameWarn().c_str());
            Press(ImGuiKey_Enter);
            CHECK(cfg.sets[idx].name == renamed && cfg.sets.back().name == "别的套装" && cfg.activeSet == renamed,
                  "重名回车：两套都没动（%s / %s）", cfg.sets[idx].name.c_str(), cfg.sets.back().name.c_str());
            for (int i = 0; i < 3; ++i) Frame();
            CHECK(!ui::DebugNameWarn().empty(), "没改成：提示留着，说明为什么");
            ui::DebugGo(2, 0);
            Frame();
            CHECK(ui::DebugNameWarn().empty(), "离开这页：提示收起");

            cfg.sets.pop_back();
            cfg.sets[idx].name = oldName;
            cfg.activeSet = oldName;
            m.reqSave = false;
        }
    }

    // 「只重置回据点」快捷键：设置页第三行，能录
    {
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        const std::string before = cfg.hkResetOnly;
        ui::DebugStartCapture(m, 2, true);
        Hold(0);
        Hold(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_BACK);
        Hold(0);
        CHECK(cfg.hkResetOnly == "LB+RB+View" && !m.capturing, "录「只重置回据点」：%s", cfg.hkResetOnly.c_str());
        cfg.hkResetOnly = before;
        m.reqSave = false;
    }

    // 主页的「重开」按 A
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(m.reqRestart && m.reqClose, "主页直接按 A：重开并关面板");

    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
