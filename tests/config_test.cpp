// 配置文件和快捷键写法的解析测试
#include "../src/config.h"
#include "../src/input.h"

#include <windows.h>
#include <xinput.h>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

static std::wstring TempFile(const wchar_t* name)
{
    wchar_t dir[MAX_PATH];
    ::GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

static void WriteFile8(const std::wstring& p, const char* s)
{
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wr = 0;
    ::WriteFile(h, s, (DWORD)strlen(s), &wr, nullptr);
    ::CloseHandle(h);
}

int main()
{
    // ---- 默认配置：文件不存在时自动生成，生成的内容要能读回来 ----
    const std::wstring def = TempFile(L"binq_default_test.ini");
    ::DeleteFileW(def.c_str());
    Config c;
    std::string err;
    CHECK(LoadConfig(def, c, err), "不存在时生成默认配置并读入");
    CHECK(c.hkRestart == "LB+RB+LT+RT", "默认重开键 = %s", c.hkRestart.c_str());
    CHECK(c.hkPanel == "View+Menu | Insert", "默认面板键 = %s", c.hkPanel.c_str());
    CHECK(c.quickBlackout && c.questId == 0 && c.sets.empty(), "默认开关");
    ::DeleteFileW(def.c_str());

    // ---- 你的「米拉」配置（从 IN-Q 的 config.txt 搬过来的样子），带 BOM 和行内注释 ----
    const std::wstring p = TempFile(L"binq_cfg_test.ini");
    WriteFile8(p,
        "\xEF\xBB\xBF; 注释\r\n"
        "[General]\r\n"
        "QuestID=66845   ; 黑龙\r\n"
        "Reset=1\r\n"
        "AcceptOnly=0\r\n"
        "WyvernStart=1\r\n"
        "ActiveSet=米拉\r\n"
        "Food=1\r\n"
        "QuickBlackout=0\r\n"
        "MantleFix=0\r\n"             // 老版本的开关，现在一直开着：读到也不理
        "[Hotkeys]\r\n"
        "Restart=LB+RB+LT+RT | Ctrl+R\r\n"
        "ResetOnly=Ctrl+Shift+R\r\n"  // 老版本的快捷键，已经去掉：读到也不理
        "[Set.米拉]\r\n"
        "Equip=米拉\r\n"
        "Item=米拉2\r\n"
        "LinkRadial=1\r\n"
        "Food=1\r\n"
        "Quest=66845\r\n"
        "[Set.备用]\r\n"
        "Equip=12\r\n"
        "[Food.1]\r\n"
        "HP=5\r\n"
        "Stamina=2\r\n"
        "Attack=3\r\n"
        "Skills=32, 9 ,39\r\n");
    CHECK(LoadConfig(p, c, err), "读入示例配置");
    CHECK(c.questId == 66845 && c.reset && !c.acceptOnly && c.wyvern && !c.quickBlackout, "[General] 各项");
    CHECK(c.activeSet == "米拉" && c.food == 1, "ActiveSet / Food");
    CHECK(c.sets.size() == 2, "两套全套，实际 %zu", c.sets.size());
    const LoadoutSet* s = c.FindSet("米拉");
    CHECK(s && s->equip == "米拉" && s->item == "米拉2" && s->linkRadial && s->food == 1 && s->quest == 66845,
          "[Set.米拉] 各项");
    CHECK(c.FindSet("备用") && c.FindSet("备用")->equip == "12", "[Set.备用] Equip=12");
    CHECK(c.foods.count(1) == 1, "[Food.1] 存在");
    const FoodPreset& f = c.foods[1];
    CHECK(f.hp == 5 && f.stamina == 2 && f.attack == 3 && f.defense == 0 && f.elemRes == 0, "猫饭能力");
    CHECK(f.skill[0] == 32 && f.skill[1] == 9 && f.skill[2] == 39, "猫饭技能 %d,%d,%d", f.skill[0], f.skill[1], f.skill[2]);
    ::DeleteFileW(p.c_str());

    // ---- 随项目发的那份配置（装进游戏的就是它）----
    {
        wchar_t exe[MAX_PATH];
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring root(exe);
        root = root.substr(0, root.find_last_of(L"\\/"));          // out
        root = root.substr(0, root.find_last_of(L"\\/") + 1);      // 项目根
        Config r;
        CHECK(LoadConfig(root + L"config\\BetterINQ.ini", r, err), "读入 config/BetterINQ.ini");
        const LoadoutSet* m = r.FindSet("米拉");
        CHECK(r.questId == 66845 && r.activeSet == "米拉" && r.food == 1 && !r.reset && !r.acceptOnly, "General");
        CHECK(m && m->equip == "米拉" && m->item == "米拉2" && m->linkRadial && m->food == 1 && m->quest == 66845, "米拉套装");
        CHECK(r.foods.count(1) && r.foods[1].hp == 5 && r.foods[1].skill[0] == 32 && r.foods[1].skill[2] == 39, "猫饭 1");
        CHECK(r.hkRestart == "LB+RB+LT+RT" && ParseHotkey(r.hkRestart).alts.size() == 1, "重开键");
    }

    // ---- 存盘再读回来：面板改完会整份重写，不能丢东西 ----
    {
        Config a;
        a.questId = 66845; a.reset = true; a.acceptOnly = false; a.wyvern = true;
        a.activeSet = "米拉"; a.food = 2; a.quickBlackout = false; a.panelOpacity = 65;
        a.hkRestart = "LB+RB+LT+RT | Ctrl+R"; a.hkPanel = "View+Menu | Insert";
        LoadoutSet s1; s1.name = "米拉"; s1.equip = "米拉"; s1.item = "米拉2"; s1.linkRadial = true; s1.food = 2; s1.quest = 66845;
        LoadoutSet s2; s2.name = "空的";
        a.sets = {s1, s2};
        FoodPreset f; f.hp = 5; f.stamina = 2; f.attack = 3; f.defense = 1; f.elemRes = 2; f.skill[0] = 32; f.skill[1] = -1; f.skill[2] = 39;
        a.foods[2] = f;
        a.foods[7] = FoodPreset();
        const std::wstring rp = TempFile(L"binq_roundtrip.ini");
        CHECK(SaveConfig(rp, a), "存盘");
        Config b;
        CHECK(LoadConfig(rp, b, err), "读回来");
        CHECK(b.questId == 66845 && b.reset && !b.acceptOnly && b.wyvern && b.activeSet == "米拉" && b.food == 2 &&
              !b.quickBlackout && b.panelOpacity == 65, "[General] 一致");
        CHECK(b.hkRestart == a.hkRestart && b.hkPanel == a.hkPanel, "[Hotkeys] 一致");
        CHECK(b.sets.size() == 2 && b.sets[0].name == "米拉" && b.sets[0].equip == "米拉" && b.sets[0].item == "米拉2" &&
              b.sets[0].linkRadial && b.sets[0].food == 2 && b.sets[0].quest == 66845 &&
              b.sets[1].name == "空的" && b.sets[1].equip.empty(), "全套一致（含空套装）");
        CHECK(b.foods.size() == 2 && b.foods[2].hp == 5 && b.foods[2].defense == 1 && b.foods[2].elemRes == 2 &&
              b.foods[2].skill[0] == 32 && b.foods[2].skill[1] == -1 && b.foods[2].skill[2] == 39 &&
              b.foods.count(7), "猫饭一致（含 -1 技能）");
        Config c2;
        CHECK(SaveConfig(rp, b) && LoadConfig(rp, c2, err) && c2.sets.size() == 2 && c2.foods.size() == 2, "再存一遍还是一样");
        ::DeleteFileW(rp.c_str());
    }

    // ---- 快捷键写法 ----
    Hotkey h = ParseHotkey("LB+RB+LT+RT");
    CHECK(h.alts.size() == 1 && h.alts[0].pad == (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER) &&
          h.alts[0].lt && h.alts[0].rt && h.alts[0].keys.empty(), "LB+RB+LT+RT");
    h = ParseHotkey("View+Menu | Insert");
    CHECK(h.alts.size() == 2 && h.alts[0].pad == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START) &&
          h.alts[1].keys.size() == 1 && h.alts[1].keys[0] == VK_INSERT, "View+Menu | Insert");
    h = ParseHotkey("Ctrl+A");
    CHECK(h.alts.size() == 1 && h.alts[0].pad == 0 && h.alts[0].keys.size() == 2 &&
          h.alts[0].keys[0] == VK_CONTROL && h.alts[0].keys[1] == 'A', "Ctrl+A 里的 A 是键盘字母");
    h = ParseHotkey("A+B");
    CHECK(h.alts.size() == 1 && h.alts[0].pad == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B), "A+B 是手柄键");
    h = ParseHotkey("Share+Options");
    CHECK(h.alts.size() == 1 && h.alts[0].pad == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START), "PS 叫法 Share+Options");
    h = ParseHotkey("F5");
    CHECK(h.alts.size() == 1 && h.alts[0].keys.size() == 1 && h.alts[0].keys[0] == VK_F5, "F5");
    h = ParseHotkey("LB+Foo | RT");
    CHECK(h.alts.size() == 1 && h.alts[0].rt && h.bad == "Foo ", "认不出的组合整组丢掉并记下：'%s'", h.bad.c_str());
    h = ParseHotkey("");
    CHECK(h.alts.empty(), "空字符串 = 不设快捷键");

    // 0.6.4：任务「无」、猫饭名字、只重置回据点的快捷键，存了能原样读回来
    {
        Config c;
        c.questId = kQuestNone;
        c.hkResetOnly = "LB+RB+View | Ctrl+F5";
        LoadoutSet s;
        s.name = "原地";
        s.quest = kQuestNone;
        s.food = 2;
        c.sets.push_back(s);
        c.foods[2].name = "黑龙饭";
        c.foods[2].hp = 5;
        c.foods[3].hp = 1;                        // 没起名字的
        const std::wstring p = L"out\\config_test_064.ini";
        CHECK(SaveConfig(p, c), "存 0.6.4 的新字段");
        Config r;
        std::string err;
        CHECK(LoadConfig(p, r, err), "读回来");
        CHECK(r.questId == kQuestNone && r.EffectiveQuest() == kQuestNone, "全局任务「无」= %d", r.questId);
        CHECK(r.sets.size() == 1 && r.sets[0].quest == kQuestNone, "套装里的任务「无」");
        CHECK(r.hkResetOnly == "LB+RB+View | Ctrl+F5", "只重置回据点：%s", r.hkResetOnly.c_str());
        CHECK(r.foods[2].name == "黑龙饭" && r.foods[2].hp == 5, "猫饭名字：%s", r.foods[2].name.c_str());
        CHECK(r.FoodName(3) == "猫饭 3" && r.FoodName(2) == "黑龙饭", "没起名字的显示「猫饭 编号」");
        ::DeleteFileW(p.c_str());
    }

    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
