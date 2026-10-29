#include "config.h"

#include <windows.h>
#include <cstdlib>
#include <cstring>

namespace {

std::string Trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// 去掉行内注释：「 ;」或「 #」之后的都算注释（值里本身不会出现分号）
std::string StripComment(const std::string& v)
{
    for (std::size_t i = 0; i < v.size(); ++i)
        if ((v[i] == ';' || v[i] == '#') && (i == 0 || v[i - 1] == ' ' || v[i - 1] == '\t'))
            return v.substr(0, i);
    return v;
}

bool ReadAll(const std::wstring& path, std::string& out)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(h, &sz);
    out.resize((std::size_t)sz.QuadPart);
    DWORD rd = 0;
    const BOOL ok = out.empty() || ::ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    out.resize(rd);
    if (out.compare(0, 3, "\xEF\xBB\xBF") == 0) out.erase(0, 3);
    return true;
}

bool Bool(const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

} // namespace

namespace {

// 配置 → ini 文本。默认配置和面板保存共用这一份模板。
std::string Render(const Config& c)
{
    auto kv = [](const char* k, const std::string& v, const char* note) {
        std::string line = std::string(k) + "=" + v;
        if (note && *note) {
            if (line.size() < 21) line.append(21 - line.size(), ' ');
            line += " ; ";
            line += note;
        }
        return line + "\r\n";
    };
    auto num = [](int v) { return std::to_string(v); };
    std::string o = "\xEF\xBB\xBF";
    o += "; Better-IN-Q 配置。面板里改的设置会自动存回这里；手改的话下次进游戏生效。\r\n\r\n";
    o += "[General]\r\n";
    o += kv("QuestID", num(c.questId), "重开时接的任务 ID，0 = 用上一次的任务，-1 = 无（不重开，原地套用）");
    o += kv("Reset", num(c.reset), "1 = 重置后回据点再接（慢，完全走正规流程）");
    o += kv("AcceptOnly", num(c.acceptOnly), "1 = 只接任务，不出发");
    o += kv("WyvernStart", num(c.wyvern), "1 = 普通地图用翼龙起始");
    o += kv("ActiveSet", c.activeSet, "重开时套用的全套（下面 [Set.名字]），空 = 不套");
    o += kv("Food", num(c.food), "每次进任务都吃的猫饭（下面 [Food.编号]），0 = 不管");
    o += kv("QuickBlackout", num(c.quickBlackout), "缩短转场黑屏");
    o += kv("QuickReset", num(c.quickReset), "快速重置（下次开游戏生效）");
    o += kv("FoodEveryQuest", num(c.foodEveryQuest), "1 = 自己正常接任务也换成插件的猫饭，0 = 只有插件重开才换");
    o += kv("PanelOpacity", num(c.panelOpacity), "面板不透明度 30~100");
    if (!c.foods.empty()) {
        std::string order;
        for (int id : c.FoodIds()) order += (order.empty() ? "" : ",") + num(id);
        o += kv("FoodOrder", order, "猫饭在面板里的顺序");
    }
    if (c.debug) o += kv("Debug", num(c.debug), "");
    o += "\r\n[Hotkeys]\r\n";
    o += "; 手柄：A B X Y LB RB LT RT LS RS Up Down Left Right View Menu\r\n";
    o += "; 键盘：Ctrl Shift Alt F1~F12 A~Z 0~9 Insert Delete Home End PageUp PageDown\r\n";
    o += "; 多个组合用 | 隔开\r\n";
    o += kv("Restart", c.hkRestart, "重开");
    o += kv("ResetOnly", c.hkResetOnly, "只重置回据点（不接任务）");
    o += kv("Panel", c.hkPanel, "打开/关闭面板");
    o += "\r\n; 全套：Equip/Item 写游戏里的套装名字，或游戏列表里看到的编号\r\n";
    if (c.sets.empty())
        o += ";[Set.示例]\r\n;Equip=1\r\n;Item=1\r\n;LinkRadial=0\r\n;Food=1\r\n;Quest=66845\r\n";
    for (const auto& s : c.sets) {
        o += "[Set." + s.name + "]\r\n";
        o += kv("Equip", s.equip, "");
        o += kv("Item", s.item, "");
        o += kv("LinkRadial", num(s.linkRadial), "套道具时联动快捷栏");
        o += kv("Food", num(s.food), "");
        o += kv("Quest", num(s.quest), "0 = 用上面的 QuestID，-1 = 无（原地套用）");
        o += "\r\n";
    }
    o += "; 猫饭：HP 0~5（+0~+50） Stamina 0~2（+0/+25/+50） Attack/Defense/ElemRes 0~3（无/小/中/大）"
         " Skills 三个技能编号，-1 = 无\r\n";
    if (c.foods.empty())
        o += ";[Food.1]\r\n;HP=5\r\n;Stamina=2\r\n;Attack=3\r\n;Defense=0\r\n;ElemRes=0\r\n;Skills=9,32,39\r\n";
    for (const auto& kvp : c.foods) {
        const FoodPreset& f = kvp.second;
        o += "[Food." + num(kvp.first) + "]\r\n";
        if (!f.name.empty()) o += kv("Name", f.name, "");
        o += kv("HP", num(f.hp), "");
        o += kv("Stamina", num(f.stamina), "");
        o += kv("Attack", num(f.attack), "");
        o += kv("Defense", num(f.defense), "");
        o += kv("ElemRes", num(f.elemRes), "");
        o += kv("Skills", num(f.skill[0]) + "," + num(f.skill[1]) + "," + num(f.skill[2]), "");
        o += "\r\n";
    }
    return o;
}

bool WriteAll(const std::wstring& path, const std::string& s, DWORD disposition)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const BOOL ok = ::WriteFile(h, s.data(), (DWORD)s.size(), &wr, nullptr) && wr == s.size();
    ::CloseHandle(h);
    return ok != FALSE;
}

} // namespace

bool SaveDefaultConfig(const std::wstring& path)
{
    return WriteAll(path, Render(Config()), CREATE_NEW);
}

bool SaveConfig(const std::wstring& path, const Config& cfg)
{
    const std::wstring tmp = path + L".tmp";
    if (!WriteAll(tmp, Render(cfg), CREATE_ALWAYS)) return false;
    return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool LoadConfig(const std::wstring& path, Config& cfg, std::string& err)
{
    std::string txt;
    if (!ReadAll(path, txt)) {
        SaveDefaultConfig(path);
        if (!ReadAll(path, txt)) { err = "读不到也建不了配置文件"; return false; }
    }
    cfg = Config();
    std::string sec;
    LoadoutSet* curSet = nullptr;
    FoodPreset* curFood = nullptr;
    std::size_t pos = 0;
    while (pos < txt.size()) {
        std::size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = Trim(txt.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') {
            sec = Trim(line.substr(1, line.size() - 2));
            curSet = nullptr;
            curFood = nullptr;
            if (sec.compare(0, 4, "Set.") == 0) {
                cfg.sets.push_back(LoadoutSet());
                curSet = &cfg.sets.back();
                curSet->name = Trim(sec.substr(4));
            } else if (sec.compare(0, 5, "Food.") == 0) {
                curFood = &cfg.foods[std::atoi(sec.c_str() + 5)];
            }
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = Trim(line.substr(0, eq));
        const std::string v = Trim(StripComment(line.substr(eq + 1)));
        const int n = std::atoi(v.c_str());
        if (sec == "General") {
            if (k == "QuestID") cfg.questId = n;
            else if (k == "Reset") cfg.reset = Bool(v);
            else if (k == "AcceptOnly") cfg.acceptOnly = Bool(v);
            else if (k == "WyvernStart") cfg.wyvern = Bool(v);
            else if (k == "ActiveSet") cfg.activeSet = v;
            else if (k == "Food") cfg.food = n;
            else if (k == "QuickBlackout") cfg.quickBlackout = Bool(v);
            else if (k == "QuickReset") cfg.quickReset = Bool(v);
            else if (k == "FoodEveryQuest") cfg.foodEveryQuest = Bool(v);
            else if (k == "PanelOpacity") cfg.panelOpacity = n < 30 ? 30 : (n > 100 ? 100 : n);
            else if (k == "Debug") cfg.debug = n;
            else if (k == "FoodOrder") {
                for (std::size_t p = 0; p <= v.size();) {
                    std::size_t e = v.find(',', p);
                    if (e == std::string::npos) e = v.size();
                    const std::string t = Trim(v.substr(p, e - p));
                    if (!t.empty()) cfg.foodOrder.push_back(std::atoi(t.c_str()));
                    p = e + 1;
                }
            }
        } else if (sec == "Hotkeys") {
            if (k == "Restart") cfg.hkRestart = v;
            else if (k == "ResetOnly") cfg.hkResetOnly = v;
            else if (k == "Panel") cfg.hkPanel = v;
        } else if (curSet) {
            if (k == "Equip") curSet->equip = v;
            else if (k == "Item") curSet->item = v;
            else if (k == "LinkRadial") curSet->linkRadial = Bool(v);
            else if (k == "Food") curSet->food = n;
            else if (k == "Quest") curSet->quest = n;
        } else if (curFood) {
            if (k == "Name") curFood->name = v;
            else if (k == "HP") curFood->hp = n;
            else if (k == "Stamina") curFood->stamina = n;
            else if (k == "Attack") curFood->attack = n;
            else if (k == "Defense") curFood->defense = n;
            else if (k == "ElemRes") curFood->elemRes = n;
            else if (k == "Skills") {
                int i = 0;
                for (std::size_t p = 0; p <= v.size() && i < 3;) {
                    std::size_t c = v.find(',', p);
                    if (c == std::string::npos) c = v.size();
                    const std::string t = Trim(v.substr(p, c - p));
                    if (!t.empty()) curFood->skill[i++] = std::atoi(t.c_str());
                    p = c + 1;
                }
            }
        }
    }
    return true;
}
