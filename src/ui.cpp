#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui.h"

#include "gamedata.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "input.h"

#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui {
namespace {

// ---------------------------------------------------------------------------
//  配色：怪猎菜单的深褐黑半透明底 + 金色。层次靠字号和亮度，不靠颜色种类。
// ---------------------------------------------------------------------------
const ImU32 kGold     = IM_COL32(226, 194, 122, 255);
const ImU32 kGoldHot  = IM_COL32(238, 211, 148, 255);
const ImU32 kGoldDown = IM_COL32(201, 164, 92, 255);
const ImU32 kInk      = IM_COL32(26, 20, 11, 255);     // 金底上的字
const ImU32 kTitle    = IM_COL32(246, 236, 210, 255);
const ImU32 kText     = IM_COL32(237, 230, 214, 255);
const ImU32 kMuted    = IM_COL32(150, 140, 118, 255);
const ImU32 kFaint    = IM_COL32(111, 102, 86, 255);
const ImU32 kWarn     = IM_COL32(232, 140, 110, 255);
const ImU32 kSoft     = IM_COL32(237, 230, 214, 26);   // 次要按钮底
const ImU32 kSoftHot  = IM_COL32(237, 230, 214, 44);
ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

float S = 1.0f;
ImFont* fReg = nullptr;
ImFont* fBold = nullptr;
float szBody, szSmall, szTitle, szPage, szBtn;

enum Page { kHome, kQuests, kSets, kSetEdit, kFood, kFoodEdit, kSettings };
Page g_page = kHome;
float g_contentH[8] = {};   // 每页上一帧量出来的内容高度（0 = 还没量过）
// 进页后把焦点放到默认项。连续两帧都设：面板刚弹出那一帧 ImGui 自己的「窗口默认焦点」
// 会在帧尾把焦点挪走，第二帧再设一次才稳。
int g_focus = 2;
bool g_focusUsed = false;
int g_editSet = -1;         // 正在编辑的全套（cfg.sets 下标）
int g_editFood = 0;         // 正在编辑的猫饭编号
int g_nameFor = -1;         // 名字框现在装的是哪一份猫饭 / 哪一套的名字（换了就重新填；进编辑页时清掉）
char g_nameBuf[64] = {};
bool g_focusName = false;
std::string g_nameWarn;     // 套装名字不合规（空的、重名）的提示

// 排序：鼠标按住一行上下拖；手柄选中一行按 LB / RB 上下挪；键盘 Ctrl + 上下
//（手柄 X 不能用：ImGui 拿它切菜单层，一按焦点就跑了）
bool g_dragMoved = false;   // 这一下鼠标按住拖过位置：松开时别当成点开
std::vector<ImRect> g_rowRects;   // 测试用：这一帧列表各行的位置
int g_questTarget = -1;     // 给谁选任务：-1 = 全局，>=0 = cfg.sets[i]
char g_search[96] = {};
int g_scrollToSel = 0;      // 还要把选中的任务滚到视野里几帧（新建的列表头一帧还不知道自己多长，滚不过去）
bool g_groupOpen[16] = {};
bool g_questInit = true;      // 刚进「换任务」：只展开当前任务所在的那一组
bool g_sideFocusedLast = false;

Page g_questFrom = kHome;   // 从哪一页进的「换任务」（返回时回那里）
const char* const kNone = "—";   // 「不额外选择」：套装编辑页下拉的第一项，以及没选时显示的值
bool g_popupLastFrame = false;   // 上一帧有没有开着的弹窗（下拉选项、删除确认）
ImGuiID g_sideCurId = 0;    // 侧栏里当前页那一格（十字键从内容区往左就落到它）
int g_sideFocusIdx = -1;    // 这一帧焦点在侧栏第几格（测试用）
float g_animHc = 0;         // 内容区高度的动画值（0 = 下一帧直接到位）
float g_frameH = 0;         // 这一帧画出来的外框高度（测试用）

// 按 B / 点返回箭头回到哪一页（主页面返回自己 = 没有上一级）
Page Back(Page p)
{
    switch (p) {
    case kQuests: return g_questFrom;
    case kSetEdit: return kSets;
    case kFoodEdit: return kFood;
    default: return p;
    }
}

// 侧栏上高亮哪一页
Page MainOf(Page p)
{
    switch (p) {
    case kQuests: return g_questFrom == kSetEdit ? kSets : kHome;
    case kSetEdit: return kSets;
    case kFoodEdit: return kFood;
    default: return p;
    }
}


void Go(Page p)
{
    g_page = p;
    g_focus = 2;
    if (p == kQuests) { g_search[0] = 0; g_scrollToSel = 3; g_questInit = true; }
    if (p == kSetEdit || p == kFoodEdit) g_nameFor = -1;   // 名字框重新填
    g_nameWarn.clear();
}

// 编辑页最上面的「名字」一栏。打字时要输入法（能打中文）；; 和 # 不收（配置文件的注释符）
struct NameEdit {
    bool changed = false;     // 这一帧框里的字变了
    bool active = false;      // 还在打字
    bool committed = false;   // 刚打完（回车、点了别处），而且改过
    std::string text;         // 框里的字，去掉首尾空格
};
NameEdit NameField(Model& m, const char* hint)
{
    NameEdit r;
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(fReg, szBody, ImVec2(p.x + 14 * S, p.y + 10 * S), kMuted, "名字");
    ImGui::SetCursorScreenPos(ImVec2(p.x + w * 0.3f, p.y + 4 * S));
    ImGui::SetNextItemWidth(w * 0.7f - 6 * S);
    ImGui::PushFont(fReg, szBody);
    if (g_focusName) { ImGui::SetKeyboardFocusHere(); g_focusName = false; }
    auto noComment = [](ImGuiInputTextCallbackData* d) { return (d->EventChar == ';' || d->EventChar == '#') ? 1 : 0; };
    r.changed = ImGui::InputTextWithHint("##name", hint, g_nameBuf, sizeof(g_nameBuf), ImGuiInputTextFlags_CallbackCharFilter,
                                         noComment);
    r.active = ImGui::IsItemActive();
    r.committed = ImGui::IsItemDeactivatedAfterEdit();
    if (r.active) m.wantIme = true;
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 46 * S));
    ImGui::Dummy(ImVec2(0, 0));
    const std::string n = g_nameBuf;
    const std::size_t a = n.find_first_not_of(' '), b = n.find_last_not_of(' ');
    r.text = a == std::string::npos ? std::string() : n.substr(a, b - a + 1);
    return r;
}

bool NavVisible() { return GImGui->NavCursorVisible; }

// 刚画完的这个控件拿默认焦点（进页后头两帧）。
// 不用 SetKeyboardFocusHere：它按 Tab 键的逻辑找「下一个可输入的控件」，会跳过按钮，
// 落到后面的滑条上还会把滑条切成输入框。
void FocusDefault()
{
    if (g_focus <= 0) return;
    --g_focus;
    g_focusUsed = true;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    // ImGui 上下移动时会保持一个横向位置，没有就靠最左。进页时定成默认项的中线：
    // 从通栏的「重开」往下落到正下方中间那个，而不是最左边
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const float cx = GImGui->LastItemData.NavRect.GetCenter().x - w->DC.CursorStartPos.x;
    w->RootWindowForNav->NavPreferredScoringPosRel[0] = ImVec2(cx, FLT_MAX);
}

void Text(ImFont* f, float size, ImU32 col, const char* s, bool wrap = false)
{
    ImGui::PushFont(f, size);
    ImGui::PushStyleColor(ImGuiCol_Text, V(col));
    if (wrap) ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(s);
    if (wrap) ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

ImVec2 TextSize(ImFont* f, float size, const char* s) { return f->CalcTextSizeA(size, FLT_MAX, 0.0f, s); }

void CenterText(ImDrawList* dl, ImFont* f, float size, ImVec2 center, ImU32 col, const char* s)
{
    const ImVec2 ts = TextSize(f, size, s);
    dl->AddText(f, size, ImVec2(std::floor(center.x - ts.x * 0.5f), std::floor(center.y - ts.y * 0.5f)), col, s);
}

// 不画 ImGui 默认的方形焦点框，自定义控件自己画
void BeginCustom() { ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0)); }
void EndCustom() { ImGui::PopStyleColor(); }
bool Focused() { return ImGui::IsItemFocused() && NavVisible(); }

// ---------------------------------------------------------------------------
//  线条图标（在 c 为中心、半边长 r 的方块里画）
// ---------------------------------------------------------------------------
enum Icon { kIcRefresh, kIcSearch, kIcShield, kIcBowl, kIcFlag, kIcGear, kIcBack, kIcPlus };

void DrawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float r, ImU32 col)
{
    const float t = std::fmax(1.6f, r * 0.16f);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    switch (ic) {
    case kIcRefresh: {
        dl->PathArcTo(c, r * 0.72f, -1.25f, 4.15f, 28);
        dl->PathStroke(col, 0, t);
        const ImVec2 tip = ImVec2(c.x + std::cos(-1.25f) * r * 0.72f, c.y + std::sin(-1.25f) * r * 0.72f);
        dl->AddTriangleFilled(ImVec2(tip.x - r * 0.05f, tip.y - r * 0.30f), ImVec2(tip.x + r * 0.33f, tip.y + r * 0.02f),
                              ImVec2(tip.x - r * 0.12f, tip.y + r * 0.26f), col);
        break;
    }
    case kIcSearch:
        dl->AddCircle(P(-0.15f, -0.15f), r * 0.52f, col, 24, t);
        dl->AddLine(P(0.24f, 0.24f), P(0.72f, 0.72f), col, t * 1.2f);
        break;
    case kIcShield: {
        const ImVec2 pts[] = {P(-0.62f, -0.70f), P(0.0f, -0.86f), P(0.62f, -0.70f), P(0.62f, 0.02f),
                              P(0.0f, 0.82f), P(-0.62f, 0.02f)};
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
        break;
    }
    case kIcBowl:
        dl->AddLine(P(-0.80f, -0.05f), P(0.80f, -0.05f), col, t);
        dl->PathArcTo(P(0.0f, -0.05f), r * 0.80f, 0.0f, 3.14159f, 20);
        dl->PathStroke(col, 0, t);
        dl->AddLine(P(-0.30f, 0.84f), P(0.30f, 0.84f), col, t);
        dl->AddLine(P(-0.25f, -0.35f), P(-0.25f, -0.75f), col, t * 0.8f);
        dl->AddLine(P(0.15f, -0.35f), P(0.15f, -0.80f), col, t * 0.8f);
        break;
    case kIcFlag: {
        dl->AddLine(P(-0.55f, -0.80f), P(-0.55f, 0.85f), col, t);
        const ImVec2 pts[] = {P(-0.55f, -0.78f), P(0.65f, -0.50f), P(-0.55f, -0.12f)};
        dl->AddPolyline(pts, 3, col, ImDrawFlags_Closed, t);
        break;
    }
    case kIcGear:
        dl->AddCircle(c, r * 0.48f, col, 24, t);
        dl->AddCircle(c, r * 0.16f, col, 12, t);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 3.14159f / 4.0f;
            dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.50f, c.y + std::sin(a) * r * 0.50f),
                        ImVec2(c.x + std::cos(a) * r * 0.80f, c.y + std::sin(a) * r * 0.80f), col, t * 1.3f);
        }
        break;
    case kIcBack:
        dl->AddLine(P(0.25f, -0.55f), P(-0.25f, 0.0f), col, t);
        dl->AddLine(P(-0.25f, 0.0f), P(0.25f, 0.55f), col, t);
        break;
    case kIcPlus:
        dl->AddLine(P(-0.6f, 0.0f), P(0.6f, 0.0f), col, t);
        dl->AddLine(P(0.0f, -0.6f), P(0.0f, 0.6f), col, t);
        break;
    }
}

// ---------------------------------------------------------------------------
//  控件
// ---------------------------------------------------------------------------
// 胶囊按钮。一页最多一个主按钮（金色实心），其余是淡底金字；删除这类是不带底的淡红字。
enum BtnStyle { kPrimary, kSecondary, kDanger };

bool PillButton(const char* id, const char* label, float w, float h, BtnStyle style, Icon icon = (Icon)-1)
{
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = h * 0.5f;
    if (style == kPrimary) dl->AddRectFilled(a, b, act ? kGoldDown : (hov ? kGoldHot : kGold), rr);
    else if (style == kSecondary) dl->AddRectFilled(a, b, (hov || act) ? kSoftHot : kSoft, rr);
    else if (hov || act) dl->AddRectFilled(a, b, IM_COL32(232, 140, 110, 22), rr);
    if (Focused()) dl->AddRect(a - ImVec2(4, 4) * S, b + ImVec2(4, 4) * S, kGold, rr + 4 * S, 0, 2.0f * S);
    const float fs = style == kPrimary ? szBtn : szBody;
    ImFont* font = style == kDanger ? fReg : fBold;
    const ImVec2 ts = TextSize(font, fs, label);
    const float iconW = (int)icon >= 0 ? fs * 1.15f : 0.0f;
    const float x0 = (a.x + b.x - ts.x - iconW) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const ImU32 col = style == kPrimary ? kInk : (style == kDanger ? kWarn : kGold);
    if ((int)icon >= 0) DrawIcon(dl, icon, ImVec2(x0 + fs * 0.42f, cy), fs * 0.48f, col);
    dl->AddText(font, fs, ImVec2(std::floor(x0 + iconW), std::floor(cy - ts.y * 0.5f)), col, label);
    return pressed;
}

// 一排芯片单选（猫饭的档位）：手柄左右移动、A 选中，比拖滑条顺手
bool SegRow(const char* id, const char* label, int* v, int count, const char* const* labels, bool takeFocus)
{
    ImGui::PushID(id);
    const float w = ImGui::GetContentRegionAvail().x;
    const float rowH = 48 * S, h = 36 * S, gap = 6 * S;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(fReg, szBody, ImVec2(p.x + 14 * S, p.y + (rowH - szBody) * 0.5f), kMuted, label);
    const float x0 = p.x + w * 0.30f;
    const float segW = (w * 0.70f - 6 * S - gap * (count - 1)) / count;
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x0 + i * (segW + gap), p.y + (rowH - h) * 0.5f));
        ImGui::PushID(i);
        BeginCustom();
        if (ImGui::InvisibleButton("##seg", ImVec2(segW, h), ImGuiButtonFlags_EnableNav)) { *v = i; changed = true; }
        EndCustom();
        if (takeFocus && i == *v) FocusDefault();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool on = i == *v, hov = ImGui::IsItemHovered();
        dl->AddRectFilled(a, b, on ? kGold : (hov ? kSoftHot : kSoft), h * 0.5f);
        if (Focused()) dl->AddRect(a - ImVec2(3, 3) * S, b + ImVec2(3, 3) * S, kGold, h * 0.5f + 3 * S, 0, 2.0f * S);
        CenterText(dl, on ? fBold : fReg, szSmall, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), on ? kInk : kText, labels[i]);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
    return changed;
}

// 圆形图标按钮 + 下方短标签（主页那一排）
bool RoundButton(const char* id, Icon icon, const char* label, float colW)
{
    const float d = 54 * S;
    const float h = d + 10 * S + szSmall + 4 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(colW, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 c(a.x + colW * 0.5f, a.y + d * 0.5f);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, d * 0.5f, (hov || act) ? kSoftHot : kSoft, 40);
    if (Focused()) dl->AddCircle(c, d * 0.5f + 4 * S, kGold, 40, 2.0f * S);
    DrawIcon(dl, icon, c, d * 0.26f, kGold);
    CenterText(dl, fReg, szSmall, ImVec2(c.x, a.y + d + 10 * S + szSmall * 0.5f), Focused() ? kText : kMuted, label);
    return pressed;
}

// 整行可点的列表项：左边主文字，右边次要文字
bool Row(const char* id, const char* left, const char* right, bool selected, float h = 0)
{
    if (h <= 0) h = 46 * S;
    ImGui::PushID(id);
    const bool pressed = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(0, h));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rightW = 0;
    if (right && *right) {
        const ImVec2 ts = TextSize(fReg, szSmall, right);
        rightW = ts.x + 16 * S;
        dl->AddText(fReg, szSmall, ImVec2(b.x - 14 * S - ts.x, std::floor(a.y + (h - ts.y) * 0.5f)), kMuted, right);
    }
    dl->PushClipRect(a, ImVec2(b.x - rightW - 8 * S, b.y), true);
    float x = a.x + 14 * S;
    if (selected) {
        dl->AddCircleFilled(ImVec2(x + 3 * S, (a.y + b.y) * 0.5f), 3.5f * S, kGold);
        x += 14 * S;
    }
    const ImVec2 ts = TextSize(fReg, szBody, left);
    dl->AddText(fReg, szBody, ImVec2(x, std::floor(a.y + (h - ts.y) * 0.5f)), selected ? kGold : kText, left);
    dl->PopClipRect();
    ImGui::PopID();
    return pressed;
}

// 开关行：左边文字（下面可带一行小字说明），右边开关
bool ToggleRow(const char* id, const char* label, const char* note, bool* v)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = (note && *note) ? 58 * S : 46 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    if (pressed) *v = !*v;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || Focused()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 12), 12 * S);
    if (Focused()) dl->AddRect(a, b, kGold, 12 * S, 0, 1.5f * S);
    const float ty = (note && *note) ? a.y + 8 * S : a.y + (h - szBody) * 0.5f;
    dl->AddText(fReg, szBody, ImVec2(a.x + 14 * S, ty), kText, label);
    if (note && *note) dl->AddText(fReg, szSmall, ImVec2(a.x + 14 * S, ty + szBody + 4 * S), kMuted, note);
    const float sw = 46 * S, sh = 26 * S;
    const ImVec2 sa(b.x - 14 * S - sw, (a.y + b.y - sh) * 0.5f);
    dl->AddRectFilled(sa, sa + ImVec2(sw, sh), *v ? kGold : IM_COL32(237, 230, 214, 46), sh * 0.5f);
    const float kx = *v ? sa.x + sw - sh * 0.5f : sa.x + sh * 0.5f;
    dl->AddCircleFilled(ImVec2(kx, sa.y + sh * 0.5f), sh * 0.5f - 3 * S, *v ? kInk : kText, 24);
    return pressed;
}

// 单选行
bool RadioRow(const char* id, const char* label, const char* note, bool on)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = (note && *note) ? 58 * S : 46 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || Focused()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 12), 12 * S);
    if (Focused()) dl->AddRect(a, b, kGold, 12 * S, 0, 1.5f * S);
    const ImVec2 c(a.x + 24 * S, (a.y + b.y) * 0.5f);
    dl->AddCircle(c, 9 * S, on ? kGold : kMuted, 24, 1.8f * S);
    if (on) dl->AddCircleFilled(c, 5 * S, kGold, 20);
    const float tx = a.x + 44 * S;
    const float ty = (note && *note) ? a.y + 8 * S : a.y + (h - szBody) * 0.5f;
    dl->AddText(fReg, szBody, ImVec2(tx, ty), on ? kTitle : kText, label);
    if (note && *note) dl->AddText(fReg, szSmall, ImVec2(tx, ty + szBody + 4 * S), kMuted, note);
    return pressed;
}

// 「名称 …… 值 ›」的行，按下打开选择弹窗
bool ValueRow(const char* id, const char* label, const char* value)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 46 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || Focused()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 12), 12 * S);
    if (Focused()) dl->AddRect(a, b, kGold, 12 * S, 0, 1.5f * S);
    dl->AddText(fReg, szBody, ImVec2(a.x + 14 * S, a.y + (h - szBody) * 0.5f), kMuted, label);
    const ImVec2 ts = TextSize(fReg, szBody, value);
    dl->AddText(fReg, szBody, ImVec2(b.x - 34 * S - ts.x, a.y + (h - szBody) * 0.5f), kText, value);
    DrawIcon(dl, kIcBack, ImVec2(b.x - 18 * S, (a.y + b.y) * 0.5f), -7 * S, kMuted);   // r<0 = 镜像成 ›
    return pressed;
}

// 页标题。子页带返回箭头：箭头加标题整块可以用鼠标点（手柄用 B，所以不进十字键导航）
void PageHeader(const char* title, const char* sub, bool back)
{
    if (back) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 ts = TextSize(fBold, szPage, title);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        BeginCustom();
        if (ImGui::InvisibleButton("##back", ImVec2(ts.x + 34 * S, ts.y))) Go(Back(g_page));
        EndCustom();
        ImGui::PopItemFlag();
        const bool hov = ImGui::IsItemHovered();
        DrawIcon(dl, kIcBack, ImVec2(p.x + 6 * S, p.y + ts.y * 0.5f), 9 * S, hov ? kGold : kMuted);
        dl->AddText(fBold, szPage, ImVec2(p.x + 24 * S, p.y), hov ? kGold : kTitle, title);
    } else {
        Text(fBold, szPage, kTitle, title);
    }
    if (sub && *sub) Text(fReg, szSmall, kMuted, sub);
    ImGui::Dummy(ImVec2(0, 10 * S));
}

// 选择弹窗：返回选中的下标（没选返回 -1）
int PickPopup(const char* id, const std::vector<std::string>& items, int current)
{
    int picked = -1;
    ImGui::SetNextWindowSizeConstraints(ImVec2(320 * S, 0), ImVec2(420 * S, 460 * S));
    if (ImGui::BeginPopup(id)) {
        for (int i = 0; i < (int)items.size(); ++i) {
            ImGui::PushID(i);
            if (Row("it", items[i].c_str(), nullptr, i == current, 40 * S)) { picked = i; ImGui::CloseCurrentPopup(); }
            if (i == current && ImGui::IsWindowAppearing()) {
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetScrollHereY(0.4f);
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    return picked;
}

std::string QuestLabel(int id, const char* zeroText)
{
    if (id == 0) return zeroText;
    if (id == kQuestNone) return "无";
    const QuestInfo* q = gamedata::FindQuest(id);
    return q ? q->zh : ("任务 " + std::to_string(id));
}

std::string FoodSummary(const FoodPreset& f)
{
    static const char* lv[] = {"无", "小", "中", "大"};
    std::string s = "体力+" + std::to_string(f.hp * 10) + " 耐力+" + std::to_string(f.stamina * 25);
    if (f.attack > 0 && f.attack <= 3) s += std::string(" 攻击") + lv[f.attack];
    if (f.defense > 0 && f.defense <= 3) s += std::string(" 防御") + lv[f.defense];
    if (f.elemRes > 0 && f.elemRes <= 3) s += std::string(" 耐性") + lv[f.elemRes];
    return s;
}

std::string SetSummary(const Config& c, const LoadoutSet& s)
{
    std::string r;
    auto add = [&](const std::string& t) { if (!r.empty()) r += " · "; r += t; };
    if (!s.equip.empty()) add("装备 " + s.equip);
    if (!s.item.empty()) add("道具 " + s.item);
    if (s.food) add(c.FoodName(s.food));
    if (s.quest == kQuestNone) add("原地换装");
    return r.empty() ? "空套装" : r;
}

std::string HotkeyText(const std::string& spec)
{
    std::string r;
    for (char ch : spec) {
        if (ch == '+') r += " + ";
        else if (ch == '|') r += "  或  ";
        else if (ch != ' ') r += ch;
    }
    return r;
}

// ---------------------------------------------------------------------------
//  各页
// ---------------------------------------------------------------------------
int ActiveSetIndex(const Config& c)
{
    for (int i = 0; i < (int)c.sets.size(); ++i) if (c.sets[i].name == c.activeSet) return i;
    return -1;
}

void PageHome(Model& m)
{
    Config& c = *m.cfg;
    const float w = ImGui::GetContentRegionAvail().x;
    const int setIdx = ActiveSetIndex(c);
    const LoadoutSet* set = setIdx >= 0 ? &c.sets[setIdx] : nullptr;
    const int quest = c.EffectiveQuest();
    const bool inPlace = quest == kQuestNone;   // 任务选「无」：重开键 = 原地套用

    ImGui::Dummy(ImVec2(0, 4 * S));
    Text(fReg, szSmall, kMuted, "当前任务");
    ImGui::Dummy(ImVec2(0, 2 * S));
    Text(fBold, szTitle, kTitle, inPlace ? "无（原地换装）" : QuestLabel(quest, "刚才打的那个任务").c_str(), true);
    // 副标题：重开时会套用什么（套装、道具套装、猫饭）
    std::string sub;
    auto add = [&](const std::string& t) { sub += (sub.empty() ? "" : " · ") + t; };
    if (set) add("套装「" + set->name + "」");
    if (set && !set->item.empty()) add("道具「" + set->item + "」");
    const int food = (set && set->food) ? set->food : c.food;
    if (food) add(c.FoodName(food));
    if (!sub.empty()) Text(fReg, szSmall, kMuted, sub.c_str(), true);
    ImGui::Dummy(ImVec2(0, 24 * S));

    if (!m.ready) {
        Text(fReg, szSmall, kWarn, "重开功能没装上，详情见 BetterINQ.log", true);
        ImGui::Dummy(ImVec2(0, 6 * S));
    }
    if (PillButton("##restart", inPlace ? "原地换装" : "重开", w, 58 * S, kPrimary, kIcRefresh)) {
        m.reqRestart = true;
        m.reqClose = true;
    }
    FocusDefault();
    if (!c.hkRestart.empty()) {
        const std::string hk = HotkeyText(c.hkRestart);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        CenterText(ImGui::GetWindowDrawList(), fReg, szSmall, ImVec2(p.x + w * 0.5f, p.y + szSmall * 0.8f), kFaint, hk.c_str());
        ImGui::Dummy(ImVec2(0, szSmall * 1.6f));
    }
    ImGui::Dummy(ImVec2(0, 18 * S));

    const float col = w / 3.0f;
    if (RoundButton("##q", kIcSearch, "换任务", col)) {
        // 套装里指定了任务的话，重开进的是套装的任务 —— 那就直接改套装里的
        g_questTarget = (set && set->quest) ? setIdx : -1;
        g_questFrom = kHome;
        Go(kQuests);
    }
    ImGui::SameLine(0, 0);
    if (RoundButton("##s", kIcShield, "换套装", col)) Go(kSets);
    ImGui::SameLine(0, 0);
    if (RoundButton("##f", kIcBowl, "换猫饭", col)) Go(kFood);
}

// 可展开的分组行：左边箭头，右边这一组的任务数
bool GroupRow(const char* id, const char* label, int count, bool open)
{
    const float h = 44 * S;
    ImGui::PushID(id);
    const bool pressed = ImGui::Selectable("##grp", false, ImGuiSelectableFlags_None, ImVec2(0, h));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(a.x + 18 * S, (a.y + b.y) * 0.5f);
    const float r = 5 * S, t = 1.8f * S;
    if (open) {   // ⌄
        dl->AddLine(ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.5f), kGold, t);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x + r, c.y - r * 0.5f), kGold, t);
    } else {      // ›
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y - r), ImVec2(c.x + r * 0.5f, c.y), kMuted, t);
        dl->AddLine(ImVec2(c.x + r * 0.5f, c.y), ImVec2(c.x - r * 0.5f, c.y + r), kMuted, t);
    }
    const ImVec2 ts = TextSize(fBold, szBody, label);
    dl->AddText(fBold, szBody, ImVec2(a.x + 36 * S, std::floor(a.y + (h - ts.y) * 0.5f)), open ? kTitle : kText, label);
    char n[16];
    std::snprintf(n, sizeof(n), "%d", count);
    const ImVec2 ns = TextSize(fReg, szSmall, n);
    dl->AddText(fReg, szSmall, ImVec2(b.x - 14 * S - ns.x, std::floor(a.y + (h - ns.y) * 0.5f)), kFaint, n);
    ImGui::PopID();
    return pressed;
}

void PageQuests(Model& m)
{
    Config& c = *m.cfg;
    const bool forSet = g_questTarget >= 0 && g_questTarget < (int)c.sets.size();
    int& target = forSet ? c.sets[g_questTarget].quest : c.questId;
    PageHeader("换任务", forSet ? ("改的是套装「" + c.sets[g_questTarget].name + "」里的任务").c_str() : nullptr, true);

    ImGui::SetNextItemWidth(-1);
    ImGui::PushFont(fReg, szBody);
    ImGui::InputTextWithHint("##search", "拼音、首字母或 ID，比如 heilong、hl", g_search, sizeof(g_search));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4 * S));

    const float listH = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##qlist", ImVec2(0, listH), ImGuiChildFlags_NavFlattened);
    auto pick = [&](int id) { target = id; m.reqSave = true; Go(Back(kQuests)); };

    int gn = 0, qn = 0;
    const QuestGroup* groups = gamedata::Groups(gn);
    const QuestInfo* qs = gamedata::Quests(qn);
    if (gn > 16) gn = 16;
    if (g_questInit) {
        g_questInit = false;
        for (bool& o : g_groupOpen) o = false;
        if (const QuestInfo* tq = gamedata::FindQuest(target))
            if (tq->group >= 0 && tq->group < gn) g_groupOpen[tq->group] = true;
    }

    const std::string needle = g_search;
    const bool searching = !needle.empty();
    // 全局任务可以是 0 =「当前任务」（重开刚打完 / 正在打的那个）。全局和套装都能选「无」：
    // 按重开键不重开，原地套用装备、道具、猫饭。搜索时这两行和搜的字无关，不列
    const bool zeroRow = !forSet && !searching;
    const bool noneRow = !searching;
    if (zeroRow) {
        if (Row("q0", "当前任务", nullptr, target == 0)) pick(0);
        if (target == 0) FocusDefault();
        if (target == 0 && g_scrollToSel > 0) { ImGui::SetScrollHereY(0.3f); --g_scrollToSel; }
    }
    if (noneRow) {
        if (Row("qn", "无", "原地更换套装", target == kQuestNone)) pick(kQuestNone);
        if (target == kQuestNone) FocusDefault();
        if (target == kQuestNone && g_scrollToSel > 0) { ImGui::SetScrollHereY(0.3f); --g_scrollToSel; }
    }
    // 没有选中的就把焦点放在第一组上
    const bool hasSel = gamedata::FindQuest(target) || (zeroRow && target == 0) || (noneRow && target == kQuestNone);

    // 先按组归好（搜索时只留匹配的），再画：每帧只做一遍字符串比较
    std::vector<int> byGroup[16];
    for (int i = 0; i < qn; ++i)
        if (qs[i].group >= 0 && qs[i].group < gn && gamedata::QuestMatches(qs[i], needle)) byGroup[qs[i].group].push_back(i);

    for (int gi = 0; gi < gn; ++gi) {
        if (byGroup[gi].empty()) continue;
        const bool open = searching || g_groupOpen[gi];
        char gid[8];
        std::snprintf(gid, sizeof(gid), "g%d", gi);
        ImGui::Dummy(ImVec2(0, 2 * S));
        if (GroupRow(gid, groups[gi].zh, (int)byGroup[gi].size(), open) && !searching) g_groupOpen[gi] = !g_groupOpen[gi];
        if (!hasSel && gi == 0) FocusDefault();
        if (!open) continue;
        ImGui::Indent(10 * S);
        for (int i : byGroup[gi]) {
            const QuestInfo& q = qs[i];
            char idbuf[16];
            std::snprintf(idbuf, sizeof(idbuf), "%d", q.id);
            const bool sel = (q.id == target);
            if (Row(idbuf, q.zh, idbuf, sel, 42 * S)) pick(q.id);
            if (sel) FocusDefault();
            if (sel && g_scrollToSel > 0) { ImGui::SetScrollHereY(0.3f); --g_scrollToSel; }
        }
        ImGui::Unindent(10 * S);
    }
    if (searching && std::all_of(byGroup, byGroup + gn, [](const std::vector<int>& v) { return v.empty(); }))
        Text(fReg, szSmall, kMuted, "没找到");
    ImGui::EndChild();
}

// 列表里能挪位置的一行（刚画完 Row 之后调）。i = 现在的位置，n = 总共几行。
// 要挪就把目标位置写进 moveTo（画完整个列表再挪）。返回「点开」：拖过位置后松开的那一下不算
bool ReorderRow(bool pressed, int i, int n, int& moveTo)
{
    if (i == 0) g_rowRects.clear();
    g_rowRects.push_back(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
    // 鼠标：按住拖出这一行，就和上 / 下一行换位置（这一行的 ID 跟着内容走，换完还按着它）
    if (ImGui::IsItemActive() && !ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const int to = i + (ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y < 0 ? -1 : 1);
        if (to >= 0 && to < n) {
            moveTo = to;
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            g_dragMoved = true;
        }
    }
    if (pressed && g_dragMoved) { g_dragMoved = false; pressed = false; }
    // 手柄 LB / RB、键盘 Ctrl + 上下：选中的这一行挪一格（ID 跟着内容走，焦点留在它身上）
    if (ImGui::IsItemFocused()) {
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        int to = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || (ctrl && ImGui::IsKeyPressed(ImGuiKey_UpArrow))) to = i - 1;
        else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || (ctrl && ImGui::IsKeyPressed(ImGuiKey_DownArrow))) to = i + 1;
        if (to != -1) {
            if (ctrl) ImGui::NavMoveRequestCancel();   // Ctrl + 上下是挪这一行，不是挪焦点
            if (to >= 0 && to < n) moveTo = to;
        }
    }
    return pressed;
}

// 列表底下的操作提示
void ReorderHint()
{
    ImGui::Dummy(ImVec2(0, 4 * S));
    Text(fReg, szSmall, kFaint, "调整顺序：按住拖动，或选中后按 LB / RB", true);
}

// 编辑页底部的「删除」：先弹框确认，确认了返回 true
bool DeleteButton(const std::string& what)
{
    if (PillButton("##del", "删除", ImGui::GetContentRegionAvail().x, 46 * S, kDanger)) ImGui::OpenPopup("confirm_del");
    bool yes = false;
    if (ImGui::BeginPopup("confirm_del")) {
        Text(fReg, szBody, kText, ("删除「" + what + "」？").c_str());
        ImGui::Dummy(ImVec2(0, 6 * S));
        if (PillButton("##yes", "删除", 200 * S, 40 * S, kPrimary)) { yes = true; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    return yes;
}

// 套装列表：点某一套进编辑页，在那里按「确认」就用这套
void PageSets(Model& m)
{
    Config& c = *m.cfg;
    const int activeIdx = ActiveSetIndex(c);
    PageHeader("套装", activeIdx >= 0 ? ("重开时套用「" + c.activeSet + "」").c_str() : nullptr, false);
    int from = -1, to = -1, open = -1;
    const int n = (int)c.sets.size();
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(c.sets[i].name.c_str());   // ID 跟着套装走：挪了位置焦点、拖动都还在它身上
        const bool pressed = Row("set", c.sets[i].name.c_str(), SetSummary(c, c.sets[i]).c_str(), i == activeIdx, 54 * S);
        int moveTo = -1;
        if (ReorderRow(pressed, i, n, moveTo)) open = i;
        if (moveTo >= 0) { from = i; to = moveTo; }
        if (i == (activeIdx >= 0 ? activeIdx : 0)) FocusDefault();
        ImGui::PopID();
    }
    if (from >= 0) {
        std::swap(c.sets[from], c.sets[to]);
        m.reqSave = true;
    }
    if (open >= 0) {
        g_editSet = open;
        Go(kSetEdit);
    }
    if (n > 1) ReorderHint();
    ImGui::Dummy(ImVec2(0, 8 * S));
    const bool newSet = PillButton("##newset", "新建套装", ImGui::GetContentRegionAvail().x, 46 * S, kSecondary, kIcPlus);
    if (c.sets.empty()) FocusDefault();
    if (newSet) {
        int n = (int)c.sets.size() + 1;
        std::string name;
        do { name = "套装" + std::to_string(n++); } while (c.FindSet(name));
        LoadoutSet s;
        s.name = name;
        c.sets.push_back(s);
        g_editSet = (int)c.sets.size() - 1;
        m.reqSave = true;
        Go(kSetEdit);
    }
}

void PageSetEdit(Model& m)
{
    Config& c = *m.cfg;
    if (g_editSet < 0 || g_editSet >= (int)c.sets.size()) { Go(kSets); return; }
    LoadoutSet& s = c.sets[g_editSet];
    const bool active = s.name == c.activeSet;
    PageHeader(s.name.c_str(), active ? "重开时会套用这套" : nullptr, true);

    // 名字：套装是按名字认的（重开时套用哪一套、配置文件里的分节），不能空、不能和别的套装重名。
    // 打完（回车、点别处）才改名；不合规的不改，框下面说明原因，框里退回原来的名字
    if (g_nameFor != g_editSet) {
        g_nameFor = g_editSet;
        std::snprintf(g_nameBuf, sizeof(g_nameBuf), "%s", s.name.c_str());
    }
    {
        const NameEdit e = NameField(m, "套装名字");
        auto problem = [&](const std::string& n) -> std::string {
            if (n.empty()) return "名字不能为空";
            if (n != s.name && c.FindSet(n)) return "已经有叫「" + n + "」的套装了";
            return std::string();
        };
        if (e.changed) g_nameWarn = problem(e.text);
        if (e.committed && problem(e.text).empty() && e.text != s.name) {
            if (c.activeSet == s.name) c.activeSet = e.text;   // 重开时套用的就是这套：跟着改
            s.name = e.text;
            m.reqSave = true;
        }
        // 不打字了：框里是实际的名字（不合规没改成的退回去）。提示留着说明为什么没改，再打字或离开这页才收
        if (!e.active && s.name != g_nameBuf) std::snprintf(g_nameBuf, sizeof(g_nameBuf), "%s", s.name.c_str());
        if (!g_nameWarn.empty()) {
            Text(fReg, szSmall, kWarn, g_nameWarn.c_str());
            ImGui::Dummy(ImVec2(0, 4 * S));
        }
    }

    // 装备
    std::vector<std::string> equips = {kNone};
    int curE = 0;
    for (const auto& e : m.equipNames) {
        equips.push_back(e.name);
        if (e.name == s.equip) curE = (int)equips.size() - 1;
    }
    if (ValueRow("##eq", "装备套装", s.equip.empty() ? kNone : s.equip.c_str())) ImGui::OpenPopup("pick_eq");
    FocusDefault();
    int p = PickPopup("pick_eq", equips, curE);
    if (p >= 0) { s.equip = p == 0 ? std::string() : equips[p]; m.reqSave = true; }

    // 道具（名字可能重复，选中时存名字；游戏里只认第一个同名的）
    std::vector<std::string> items = {kNone};
    int curI = 0;
    for (const auto& it : m.itemNames) {
        items.push_back(std::to_string(it.number) + "  " + it.name);
        if (it.name == s.item || std::to_string(it.number) == s.item) curI = (int)items.size() - 1;
    }
    if (ValueRow("##it", "道具套装", s.item.empty() ? kNone : s.item.c_str())) ImGui::OpenPopup("pick_it");
    p = PickPopup("pick_it", items, curI);
    if (p >= 0) { s.item = p == 0 ? std::string() : m.itemNames[p - 1].name; m.reqSave = true; }

    if (ToggleRow("##lr", "联动快捷栏", nullptr, &s.linkRadial)) m.reqSave = true;

    // 猫饭
    std::vector<std::string> foods = {kNone};
    std::vector<int> foodIds = {0};
    int curF = 0;
    for (int id : c.FoodIds()) {
        foods.push_back(c.FoodName(id));
        foodIds.push_back(id);
        if (id == s.food) curF = (int)foods.size() - 1;
    }
    if (ValueRow("##fd", "猫饭", s.food ? c.FoodName(s.food).c_str() : kNone)) ImGui::OpenPopup("pick_fd");
    p = PickPopup("pick_fd", foods, curF);
    if (p >= 0) { s.food = foodIds[p]; m.reqSave = true; }

    if (ValueRow("##qs", "任务", QuestLabel(s.quest, kNone).c_str())) {
        g_questTarget = g_editSet;
        g_questFrom = kSetEdit;
        Go(kQuests);
    }

    // 确认 = 重开时套用这套（改的内容是随改随存的，不用确认）
    ImGui::Dummy(ImVec2(0, 14 * S));
    if (PillButton("##ok", "确认", ImGui::GetContentRegionAvail().x, 54 * S, kPrimary)) {
        c.activeSet = s.name;
        m.reqSave = true;
        Go(kSets);
    }
    // 原地换上：设成这套，并且马上原地换装（不管任务选的什么），关面板
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (PillButton("##apply", "原地换上这套", ImGui::GetContentRegionAvail().x, 46 * S, kSecondary)) {
        c.activeSet = s.name;
        m.reqSave = true;
        m.reqApplySet = s.name;
        m.reqClose = true;
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (DeleteButton(s.name)) {
        if (active) c.activeSet.clear();
        c.sets.erase(c.sets.begin() + g_editSet);
        g_editSet = -1;
        m.reqSave = true;
        Go(kSets);
    }
}

// 猫饭列表：点某一份进编辑页，在那里按「确认」就每次进任务都吃它
void PageFood(Model& m)
{
    Config& c = *m.cfg;
    const bool has = c.foods.count(c.food) > 0;
    PageHeader("猫饭", has ? ("每次进任务都吃「" + c.FoodName(c.food) + "」").c_str() : nullptr, false);
    std::vector<int> ids = c.FoodIds();
    int from = -1, to = -1, open = 0;
    const int n = (int)ids.size();
    for (int i = 0; i < n; ++i) {
        const int id = ids[i];
        ImGui::PushID(id);
        const bool pressed = Row("food", c.FoodName(id).c_str(), FoodSummary(c.foods.at(id)).c_str(), id == c.food, 54 * S);
        int moveTo = -1;
        if (ReorderRow(pressed, i, n, moveTo)) open = id;
        if (moveTo >= 0) { from = i; to = moveTo; }
        if (has ? id == c.food : i == 0) FocusDefault();
        ImGui::PopID();
    }
    if (from >= 0) {
        std::swap(ids[from], ids[to]);
        c.foodOrder = ids;
        m.reqSave = true;
    }
    if (open) {
        g_editFood = open;
        Go(kFoodEdit);
    }
    if (n > 1) ReorderHint();
    ImGui::Dummy(ImVec2(0, 8 * S));
    const bool newFood = PillButton("##newfood", "新建猫饭", ImGui::GetContentRegionAvail().x, 46 * S, kSecondary, kIcPlus);
    if (c.foods.empty()) FocusDefault();
    if (newFood) {
        int n = 1;
        while (c.foods.count(n)) ++n;
        c.foods[n] = FoodPreset();
        g_editFood = n;
        m.reqSave = true;
        Go(kFoodEdit);
    }
    ImGui::Dummy(ImVec2(0, 10 * S));
    if (ToggleRow("##fe", "正常接任务时沿用此猫饭", nullptr, &c.foodEveryQuest))
        m.reqSave = true;
}

void PageFoodEdit(Model& m)
{
    Config& c = *m.cfg;
    auto it = c.foods.find(g_editFood);
    if (it == c.foods.end()) { Go(kFood); return; }
    FoodPreset& f = it->second;
    const bool active = c.food == g_editFood;
    const std::string title = c.FoodName(g_editFood);
    PageHeader(title.c_str(), active ? "每次进任务都吃这个" : nullptr, true);

    // 名字：空着就叫「猫饭 编号」。这一格打字时输入法照常能用（别处面板会把输入法摘掉）
    if (g_nameFor != g_editFood) {
        g_nameFor = g_editFood;
        std::snprintf(g_nameBuf, sizeof(g_nameBuf), "%s", f.name.c_str());
    }
    {
        // 猫饭按编号认，名字随打随改；空着就叫「猫饭 编号」
        const NameEdit e = NameField(m, ("猫饭 " + std::to_string(g_editFood)).c_str());
        if (e.changed) {
            f.name = e.text;
            m.reqSave = true;
        }
    }

    static const char* hp[] = {"+0", "+10", "+20", "+30", "+40", "+50"};
    static const char* st[] = {"+0", "+25", "+50"};
    static const char* lv[] = {"无", "小", "中", "大"};
    if (SegRow("hp", "体力", &f.hp, 6, hp, true)) m.reqSave = true;
    if (SegRow("st", "耐力", &f.stamina, 3, st, false)) m.reqSave = true;
    if (SegRow("at", "攻击", &f.attack, 4, lv, false)) m.reqSave = true;
    if (SegRow("df", "防御", &f.defense, 4, lv, false)) m.reqSave = true;
    if (SegRow("er", "属性耐性", &f.elemRes, 4, lv, false)) m.reqSave = true;

    int sn = 0;
    const FoodSkillName* skills = gamedata::FoodSkills(sn);
    std::vector<std::string> names;
    for (int i = 0; i < sn; ++i) names.push_back(skills[i].zh);
    for (int k = 0; k < 3; ++k) {
        char id[16], pid[16], label[16];
        std::snprintf(id, sizeof(id), "##sk%d", k);
        std::snprintf(pid, sizeof(pid), "pick_sk%d", k);
        std::snprintf(label, sizeof(label), "技能 %d", k + 1);
        int cur = 0;
        for (int i = 0; i < sn; ++i) if (skills[i].id == f.skill[k]) cur = i;
        if (ValueRow(id, label, gamedata::FoodSkillNameOf(f.skill[k]))) ImGui::OpenPopup(pid);
        const int p = PickPopup(pid, names, cur);
        if (p >= 0) { f.skill[k] = skills[p].id; m.reqSave = true; }
    }

    // 确认 = 每次进任务都吃这个
    ImGui::Dummy(ImVec2(0, 14 * S));
    if (PillButton("##ok", "确认", ImGui::GetContentRegionAvail().x, 54 * S, kPrimary)) {
        c.food = g_editFood;
        m.reqSave = true;
        Go(kFood);
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (DeleteButton(title)) {
        if (active) c.food = 0;
        for (auto& s : c.sets) if (s.food == g_editFood) s.food = 0;
        c.foods.erase(it);
        m.reqSave = true;
        Go(kFood);
    }
}

// ---------------------------------------------------------------------------
//  快捷键：选中一格按 A（或点一下）开始录，按下想用的键、全部松开就记住
// ---------------------------------------------------------------------------
struct Capture {
    std::string* spec = nullptr;   // 录完写进哪个快捷键（nullptr = 没在录）
    bool pad = false;              // 录手柄那一组还是键盘那一组
    bool armed = false;            // 开始录之前按着的键（比如刚按下去的 A）都松开了
    unsigned padAcc = 0;           // 这一轮按过的键，全松开时认这一组
    std::vector<int> keyAcc;
    unsigned prevPad = 0;
    double lastInput = 0;          // 太久没动静就取消
    ImRect rect;                   // 正在录的那一格（点别处 = 取消）
};
Capture g_cap;
std::string g_capWhy;              // 刚才那组为什么不能用（录的过程中显示）

void EndCapture()
{
    g_cap = Capture();
    g_capWhy.clear();
}

// 快捷键写法拆成手柄那组和键盘那组（各取第一组）
void SplitSpec(const std::string& spec, std::string& pad, std::string& key)
{
    pad.clear();
    key.clear();
    for (std::size_t p = 0; p <= spec.size();) {
        std::size_t bar = spec.find('|', p);
        if (bar == std::string::npos) bar = spec.size();
        std::string alt = spec.substr(p, bar - p);
        p = bar + 1;
        alt.erase(std::remove(alt.begin(), alt.end(), ' '), alt.end());
        const Hotkey h = ParseHotkey(alt);
        if (h.alts.size() != 1) continue;
        std::string& to = h.alts[0].keys.empty() ? pad : key;
        if (to.empty()) to = alt;
    }
}

std::string JoinSpec(const std::string& pad, const std::string& key)
{
    if (pad.empty()) return key;
    if (key.empty()) return pad;
    return pad + " | " + key;
}

// 把录到的（或清空的）一组写回快捷键
void SetCapturedPart(Model& m, const std::string& combo)
{
    std::string pad, key;
    SplitSpec(*g_cap.spec, pad, key);
    (g_cap.pad ? pad : key) = combo;
    *g_cap.spec = JoinSpec(pad, key);
    m.reqSave = true;
    EndCapture();
}

// 每帧调一次：录键的状态机。单按手柄 X 或键盘 Delete / Backspace 再松开 = 清除这一格
//（手柄单键、单按 Delete 本来就不能当快捷键，不会和录键冲突）
void UpdateCapture(Model& m)
{
    if (!g_cap.spec) return;
    const double now = ImGui::GetTime();
    const bool padB = (m.padHeld & XINPUT_GAMEPAD_B) && !(g_cap.prevPad & XINPUT_GAMEPAD_B);
    g_cap.prevPad = m.padHeld;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || now - g_cap.lastInput > 8.0 ||
        (ImGui::IsMouseClicked(0) && !g_cap.rect.Contains(ImGui::GetIO().MousePos)) ||
        (!g_cap.pad && padB)) {   // 录键盘时按手柄 B = 取消（录手柄时 B 本身可以是组合里的键）
        EndCapture();
        return;
    }
    if (g_cap.pad && g_cap.armed &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        SetCapturedPart(m, std::string());   // 录手柄时键盘不参与，Delete 直接就是清除
        return;
    }
    const bool held = g_cap.pad ? m.padHeld != 0 : !m.keysHeld.empty();
    if (!g_cap.armed) {
        if (!held) { g_cap.armed = true; g_cap.lastInput = now; }
        return;
    }
    if (held) {
        g_cap.lastInput = now;
        if (g_cap.pad) g_cap.padAcc |= m.padHeld;
        else
            for (int vk : m.keysHeld)
                if (std::find(g_cap.keyAcc.begin(), g_cap.keyAcc.end(), vk) == g_cap.keyAcc.end()) g_cap.keyAcc.push_back(vk);
        return;
    }
    if (!g_cap.padAcc && g_cap.keyAcc.empty()) return;   // 还没按
    const bool clear = g_cap.pad ? g_cap.padAcc == XINPUT_GAMEPAD_X
                                 : std::all_of(g_cap.keyAcc.begin(), g_cap.keyAcc.end(),
                                               [](int vk) { return vk == VK_DELETE || vk == VK_BACK; });
    if (clear) { SetCapturedPart(m, std::string()); return; }
    std::string why;
    const std::string combo = g_cap.pad ? PadComboSpec(g_cap.padAcc, why) : KeyComboSpec(g_cap.keyAcc, why);
    g_cap.padAcc = 0;
    g_cap.keyAcc.clear();
    if (combo.empty()) { g_capWhy = why; return; }      // 不能用：说明原因，接着录
    SetCapturedPart(m, combo);
}

constexpr float kHkLabelW = 0.30f;   // 快捷键行左边标签占的宽度比例

// 一格：某个快捷键的手柄组或键盘组。按 A / 点一下开始录（录的时候再点一下取消）
void HotkeyChip(const char* id, std::string& spec, bool pad, float w, float h, Model& m)
{
    std::string padPart, keyPart;
    SplitSpec(spec, padPart, keyPart);
    const std::string cur = pad ? padPart : keyPart;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool focused = Focused(), hov = ImGui::IsItemHovered();
    bool mine = g_cap.spec == &spec && g_cap.pad == pad;
    if (pressed) {
        if (mine) EndCapture();
        else if (!g_cap.spec) {
            EndCapture();
            g_cap.spec = &spec;
            g_cap.pad = pad;
            g_cap.lastInput = ImGui::GetTime();
        }
        mine = g_cap.spec == &spec && g_cap.pad == pad;
    }
    if (mine) g_cap.rect = ImRect(a, b);
    if (focused && !g_cap.spec && !cur.empty() &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        spec = pad ? keyPart : padPart;
        m.reqSave = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = 12 * S;
    std::string text;
    ImU32 col = kText;
    if (mine) {
        const float t = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.0f);
        dl->AddRectFilled(a, b, IM_COL32(226, 194, 122, (int)(22 + 28 * t)), rr);
        dl->AddRect(a, b, kGold, rr, 0, 2.0f * S);
        if (g_cap.armed) {
            std::vector<int> keys = g_cap.keyAcc;
            for (int vk : m.keysHeld) if (std::find(keys.begin(), keys.end(), vk) == keys.end()) keys.push_back(vk);
            text = pad ? PadComboText(g_cap.padAcc | m.padHeld) : KeyComboText(keys);
        }
        if (text.empty()) text = g_cap.armed ? "请按键…" : "先松开…";
        col = kGold;
    } else {
        dl->AddRectFilled(a, b, hov ? kSoftHot : kSoft, rr);
        if (focused) dl->AddRect(a - ImVec2(3, 3) * S, b + ImVec2(3, 3) * S, kGold, rr + 3 * S, 0, 2.0f * S);
        text = cur.empty() ? "不设" : cur;
        col = cur.empty() ? kFaint : kText;
    }
    float fs = szBody;
    if (TextSize(fReg, fs, text.c_str()).x > w - 16 * S) fs = szSmall;   // 长组合缩小字号
    dl->PushClipRect(a, b, true);
    CenterText(dl, fReg, fs, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), col, text.c_str());
    dl->PopClipRect();
}

void HotkeyRow(const char* id, const char* label, std::string& spec, Model& m)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float rowH = 50 * S, h = 38 * S, gap = 8 * S;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(fReg, szBody, ImVec2(p.x + 14 * S, p.y + (rowH - szBody) * 0.5f), kMuted, label);
    const float x0 = p.x + w * kHkLabelW;
    const float cw = (w * (1.0f - kHkLabelW) - 6 * S - gap) * 0.5f;
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(x0, p.y + (rowH - h) * 0.5f));
    HotkeyChip("##pad", spec, true, cw, h, m);
    ImGui::SetCursorScreenPos(ImVec2(x0 + cw + gap, p.y + (rowH - h) * 0.5f));
    HotkeyChip("##key", spec, false, cw, h, m);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(0, 0));
}

// 两列的列名（手柄 / 键盘），和 HotkeyRow 的两格对齐
void HotkeyHeader()
{
    const float w = ImGui::GetContentRegionAvail().x, gap = 8 * S;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x0 = p.x + w * kHkLabelW;
    const float cw = (w * (1.0f - kHkLabelW) - 6 * S - gap) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    CenterText(dl, fReg, szSmall, ImVec2(x0 + cw * 0.5f, p.y + szSmall * 0.6f), kFaint, "手柄");
    CenterText(dl, fReg, szSmall, ImVec2(x0 + cw + gap + cw * 0.5f, p.y + szSmall * 0.6f), kFaint, "键盘");
    ImGui::Dummy(ImVec2(0, szSmall * 1.3f));
}

void PageSettings(Model& m)
{
    Config& c = *m.cfg;
    PageHeader("设置", nullptr, false);

    Text(fReg, szSmall, kFaint, "重开方式");
    ImGui::Dummy(ImVec2(0, 2 * S));
    const int mode = c.acceptOnly ? 2 : (c.reset ? 1 : 0);
    if (RadioRow("##m0", "直接重开", nullptr, mode == 0)) { c.reset = false; c.acceptOnly = false; m.reqSave = true; }
    if (mode == 0) FocusDefault();
    if (RadioRow("##m1", "重置后回据点再接", nullptr, mode == 1)) { c.reset = true; c.acceptOnly = false; m.reqSave = true; }
    if (mode == 1) FocusDefault();
    if (RadioRow("##m2", "只接任务，不出发", nullptr, mode == 2)) { c.reset = false; c.acceptOnly = true; m.reqSave = true; }
    if (mode == 2) FocusDefault();
    ImGui::Dummy(ImVec2(0, 10 * S));

    if (ToggleRow("##wy", "翼龙起始", "普通地图坐翼龙开局，关闭即为在营地开局", &c.wyvern)) m.reqSave = true;
    if (ToggleRow("##bo", "缩短转场黑屏", "下次开游戏生效", &c.quickBlackout)) m.reqSave = true;
    ImGui::Dummy(ImVec2(0, 10 * S));

    Text(fReg, szSmall, kFaint, "面板");
    ImGui::Dummy(ImVec2(0, 2 * S));
    {
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(fReg, szBody, ImVec2(p.x + 14 * S, p.y + 10 * S), kMuted, "不透明度");
        ImGui::SetCursorScreenPos(ImVec2(p.x + w * 0.42f, p.y + 4 * S));
        ImGui::SetNextItemWidth(w * 0.58f - 6 * S);
        ImGui::PushFont(fReg, szBody);
        if (ImGui::SliderInt("##op", &c.panelOpacity, 30, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
            m.reqSave = true;
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 46 * S));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::Dummy(ImVec2(0, 10 * S));

    Text(fReg, szSmall, kFaint, "快捷键");
    ImGui::Dummy(ImVec2(0, 2 * S));
    HotkeyHeader();
    HotkeyRow("##hk1", "重开", c.hkRestart, m);
    HotkeyRow("##hk2", "只重置回据点", c.hkResetOnly, m);
    HotkeyRow("##hk3", "打开面板", c.hkPanel, m);
    ImGui::Dummy(ImVec2(0, 4 * S));
    const char* hint = !g_cap.spec ? "选中一格按 A 或点一下，再按想用的键"
                       : g_cap.pad ? "按住想用的手柄键（至少两个），全部松开就记住 · Esc 取消"
                                   : "按下想用的键，全部松开就记住 · Esc 取消";
    Text(fReg, szSmall, g_capWhy.empty() ? kFaint : kWarn, g_capWhy.empty() ? hint : g_capWhy.c_str(), true);
    Text(fReg, szSmall, kFaint, "不设：选中后，手柄那格单按 X，键盘那格单按 Delete", true);
    ImGui::Dummy(ImVec2(0, 14 * S));

    Text(fReg, szSmall, kFaint, ("Better-IN-Q " + m.version).c_str());
    Text(fReg, szSmall, kFaint, "原作者 年糕狗三世", true);
}

// ---------------------------------------------------------------------------
//  左侧栏。返回这一帧焦点在不在侧栏上。
// ---------------------------------------------------------------------------
bool Sidebar(float x, float y, float w, float h)
{
    struct It { Page p; Icon ic; const char* label; };
    static const It items[] = {{kHome, kIcFlag, "任务"}, {kSets, kIcShield, "套装"}, {kFood, kIcBowl, "猫饭"},
                               {kSettings, kIcGear, "设置"}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(o.x + x + w, o.y + y + 14 * S), ImVec2(o.x + x + w, o.y + y + h - 14 * S),
                IM_COL32(201, 164, 92, 40), 1.0f);
    // 十字键：内容里往左就进侧栏（落在当前页那一格），在侧栏里上下移直接切页，往右或按 A 回到内容
    const Page cur = MainOf(g_page);
    ImGuiID ids[4] = {};
    int focused = -1, pressed = -1, curIdx = 0;
    for (int k = 0; k < 4; ++k) {
        ImGui::PushID(items[k].label);
        ids[k] = ImGui::GetID("##side");
        ImGui::PopID();
        if (items[k].p == cur) curIdx = k;
    }
    g_sideCurId = ids[curIdx];
    // 焦点不在侧栏时，侧栏不参与十字键导航：不然 ImGui 会按几何位置挑离得最近的一格
    //（从「重开」往左会先落到「猫饭」）。从内容区进侧栏由 Draw 接手，直接落到当前页那一格。
    const bool inSide = std::find(ids, ids + 4, GImGui->NavId) != ids + 4;
    if (!inSide) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    float iy = y + 18 * S;
    for (int k = 0; k < 4; ++k) {
        const It& it = items[k];
        const float bw = 60 * S, bh = 62 * S;
        ImGui::SetCursorPos(ImVec2(x + (w - bw) * 0.5f, iy));
        ImGui::PushID(it.label);
        BeginCustom();
        if (ImGui::InvisibleButton("##side", ImVec2(bw, bh), ImGuiButtonFlags_EnableNav)) pressed = k;
        EndCustom();
        if (ImGui::IsItemFocused()) focused = k;
        ImGui::PopID();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool on = cur == it.p;
        if (on) dl->AddRectFilled(a, b, IM_COL32(226, 194, 122, 40), 14 * S);
        else if (ImGui::IsItemHovered()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 14), 14 * S);
        if (focused == k && NavVisible()) dl->AddRect(a - ImVec2(3, 3) * S, b + ImVec2(3, 3) * S, kGold, 16 * S, 0, 2.0f * S);
        const ImU32 col = on ? kGold : kMuted;
        DrawIcon(dl, it.ic, ImVec2((a.x + b.x) * 0.5f, a.y + 22 * S), 10 * S, col);
        CenterText(dl, fReg, szSmall, ImVec2((a.x + b.x) * 0.5f, a.y + 46 * S), col, it.label);
        iy += bh + 6 * S;
    }
    if (!inSide) ImGui::PopItemFlag();
    g_sideFocusIdx = focused;
    const bool rightKey = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
    if (pressed >= 0) {
        Go(items[pressed].p);              // 鼠标点 / 按 A：切过去，焦点进内容的默认项
        g_sideFocusedLast = false;
    } else if (focused >= 0 && rightKey) {
        g_focus = 2;                       // 往右：回到这一页的默认项（不让 ImGui 按几何位置乱挑）
        g_sideFocusedLast = false;
    } else if (focused >= 0) {
        if (!g_sideFocusedLast && focused != curIdx) {
            // 刚从内容移进来：落点是离原焦点最近的那格，不一定是当前页 —— 挪回当前页那格
            ImGui::SetFocusID(ids[curIdx], ImGui::GetCurrentWindow());
        } else if (focused != curIdx) {
            g_page = items[focused].p;     // 在侧栏里上下移：直接切页，焦点留在侧栏
            g_focus = 0;
        }
        g_sideFocusedLast = true;
    } else {
        if (g_sideFocusedLast) g_focus = 2;   // 从侧栏往右回到内容：落到这一页的默认项
        g_sideFocusedLast = false;
    }
    ImGui::SetCursorPos(ImVec2(x, y + h - 28 * S));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    CenterText(dl, fReg, szSmall * 0.9f, ImVec2(p.x + w * 0.5f, p.y), kFaint, "B 返回");
    return focused >= 0;
}

} // namespace

// ---------------------------------------------------------------------------
void Init(float scale)
{
    S = scale < 0.7f ? 0.7f : (scale > 2.5f ? 2.5f : scale);
    szBody = 19 * S;
    szSmall = 15 * S;
    szTitle = 34 * S;
    szPage = 26 * S;
    szBtn = 22 * S;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;

    wchar_t win[MAX_PATH] = {};
    ::GetWindowsDirectoryW(win, MAX_PATH);
    char fonts[MAX_PATH * 3] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, win, -1, fonts, sizeof(fonts), nullptr, nullptr);
    const std::string dir = std::string(fonts) + "\\Fonts\\";
    ImFontConfig fc;
    fc.OversampleH = 1;
    fReg = io.Fonts->AddFontFromFileTTF((dir + "msyh.ttc").c_str(), szBody, &fc);
    fBold = io.Fonts->AddFontFromFileTTF((dir + "msyhbd.ttc").c_str(), szBody, &fc);
    if (!fReg) fReg = io.Fonts->AddFontDefault();
    if (!fBold) fBold = fReg;
    io.FontDefault = fReg;

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 22 * S;
    st.WindowBorderSize = 1.0f;
    st.WindowPadding = ImVec2(0, 0);
    st.ChildRounding = 0;
    st.ChildBorderSize = 0;
    st.PopupRounding = 16 * S;
    st.PopupBorderSize = 1.0f;
    st.FrameRounding = 12 * S;
    st.FramePadding = ImVec2(14 * S, 10 * S);
    st.ItemSpacing = ImVec2(8 * S, 6 * S);
    st.GrabRounding = 10 * S;
    st.GrabMinSize = 18 * S;
    st.ScrollbarSize = 6 * S;
    st.ScrollbarRounding = 6 * S;
    st.SelectableTextAlign = ImVec2(0, 0.5f);

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = V(kText);
    c[ImGuiCol_TextDisabled] = V(kMuted);
    c[ImGuiCol_WindowBg] = ImVec4(0.063f, 0.051f, 0.035f, 0.80f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.094f, 0.078f, 0.055f, 0.97f);
    c[ImGuiCol_Border] = V(IM_COL32(201, 164, 92, 90));
    c[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0.35f);
    c[ImGuiCol_FrameBgHovered] = V(IM_COL32(226, 194, 122, 30));
    c[ImGuiCol_FrameBgActive] = V(IM_COL32(226, 194, 122, 46));
    c[ImGuiCol_Header] = V(IM_COL32(226, 194, 122, 30));
    c[ImGuiCol_HeaderHovered] = V(IM_COL32(237, 230, 214, 18));
    c[ImGuiCol_HeaderActive] = V(IM_COL32(226, 194, 122, 56));
    c[ImGuiCol_SliderGrab] = V(kGold);
    c[ImGuiCol_SliderGrabActive] = V(kGoldHot);
    c[ImGuiCol_CheckMark] = V(kGold);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = V(IM_COL32(237, 230, 214, 40));
    c[ImGuiCol_ScrollbarGrabHovered] = V(IM_COL32(237, 230, 214, 70));
    c[ImGuiCol_ScrollbarGrabActive] = V(IM_COL32(226, 194, 122, 120));
    c[ImGuiCol_NavCursor] = V(kGold);
    c[ImGuiCol_TextSelectedBg] = V(IM_COL32(226, 194, 122, 80));
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.3f);
}

void DebugGo(int page, int editIndex)
{
    static const Page map[] = {kHome, kQuests, kSets, kSetEdit, kFood, kFoodEdit, kSettings};
    if (page < 0 || page > 6) return;
    g_editSet = editIndex;
    g_editFood = editIndex;
    g_questTarget = -1;
    g_questFrom = kHome;
    Go(map[page]);
}

int DebugPage()
{
    static const Page map[] = {kHome, kQuests, kSets, kSetEdit, kFood, kFoodEdit, kSettings};
    for (int i = 0; i < 7; ++i) if (map[i] == g_page) return i;
    return -1;
}

bool DebugSidebarFocused() { return g_sideFocusedLast; }
int DebugSidebarFocusIndex() { return g_sideFocusIdx; }
float DebugFrameHeight() { return g_frameH; }

void DebugStartCapture(Model& m, int row, bool pad)
{
    std::string* const specs[] = {&m.cfg->hkRestart, &m.cfg->hkPanel, &m.cfg->hkResetOnly};
    if (row < 0 || row > 2) return;
    g_page = kSettings;
    EndCapture();
    g_cap.spec = specs[row];
    g_cap.pad = pad;
    g_cap.lastInput = ImGui::GetTime();
}

std::string DebugCaptureWhy() { return g_capWhy; }

void DebugSetSearch(const char* text) { std::snprintf(g_search, sizeof(g_search), "%s", text); }
void DebugFocusName() { g_focusName = true; }
bool DebugRowCenter(int i, float& x, float& y)
{
    if (i < 0 || i >= (int)g_rowRects.size()) return false;
    x = g_rowRects[i].GetCenter().x;
    y = g_rowRects[i].GetCenter().y;
    return true;
}
std::string DebugNameWarn() { return g_nameWarn; }

void OnOpen(Model& m)
{
    g_page = kHome;
    g_focus = 2;
    g_search[0] = 0;
    g_animHc = 0;
    g_popupLastFrame = false;
    EndCapture();
    m.capturing = false;
    ImGui::SetNavCursorVisible(true);
}

void Draw(Model& m, float screenW, float screenH)
{
    if (!m.cfg) return;
    m.wantIme = false;   // 名字框这一帧在打字的话会再置上
    const float W = 560 * S;
    const float sideW = 86 * S;
    const float pad = 26 * S;
    const ImVec2 inPad(12 * S, 8 * S);   // 内容区内边距：控件的焦点描边画在控件外面，贴边会被裁
    const float statusH = m.status.empty() ? 0 : szSmall + 14 * S;
    // 内容区高度按这一页上一帧量出来的实际内容收紧（主页就很矮），长的页面封顶后滚动；
    // 换任务是长列表，直接用满
    const float maxHc = 560 * S, minHc = 300 * S;
    const bool measured = g_page != kQuests && g_contentH[g_page] > 0;
    const float hc = measured ? std::fmin(maxHc, std::fmax(minHc, g_contentH[g_page])) : maxHc;
    // 换页时面板外框的高度平滑过渡（约 0.2 秒），以屏幕中线为轴上下一起收放。
    // 只有画出来的外框在动：ImGui 窗口一换页就放在最终的位置和大小，内容按最终排版，
    // 外框外面的部分这一帧裁掉。窗口自己每帧挪动或变高的话，ImGui 的十字键导航和滚动定位会乱。
    // 第一次进这页还没量过：先别动，下一帧量完再变。刚打开面板就碰上没量过的页时 g_animHc 还是 0，
    // 这一帧整个不画（外框高度 0，内容全裁掉），下一帧量完直接到位，免得先撑满再缩回来
    float target = hc;
    if (g_page != kQuests && !measured) target = g_animHc;
    if (g_animHc <= 0) g_animHc = target;
    else g_animHc += (target - g_animHc) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 16.0f));
    if (std::fabs(target - g_animHc) < 0.5f) g_animHc = target;
    const float H = (pad - inPad.y) + hc + statusH + pad;               // 这一页的最终大小
    const float Hf = g_animHc > 0 ? (pad - inPad.y) + g_animHc + statusH + pad : 0.0f;   // 看得见的外框（动画中）
    // 左边有侧栏、上面是标题，正中摆放时视觉重心会偏右下。整体往左挪四分之一个侧栏宽、
    // 再往上提一点，主页的「重开」落在屏幕正中略偏上
    const float cy = std::floor(screenH * 0.5f - 26 * S);
    const ImVec2 pos(std::floor((screenW - W) * 0.5f - sideW * 0.25f), std::floor(cy - H * 0.5f));
    const ImVec2 fMin(pos.x, std::floor(cy - Hf * 0.5f)), fMax(pos.x + W, std::floor(cy - Hf * 0.5f) + Hf);
    g_frameH = Hf;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);   // 底和边框自己画（按动画中的外框大小）
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const bool shown = ImGui::Begin("IN-Q##panel", nullptr, wf);
    ImGui::PopStyleVar();
    if (!shown) { ImGui::End(); return; }
    if (ImGui::IsWindowAppearing()) ImGui::SetWindowFocus();
    ImGuiWindow* const panel = ImGui::GetCurrentWindow();
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        ImVec4 bg = st.Colors[ImGuiCol_WindowBg];
        bg.w = m.cfg->panelOpacity / 100.0f;
        ImDrawList* bdl = Hf > 0 ? ImGui::GetBackgroundDrawList() : nullptr;
        if (bdl) bdl->AddRectFilled(fMin, fMax, ImGui::ColorConvertFloat4ToU32(bg), st.WindowRounding);
        if (bdl) bdl->AddRect(fMin, fMax, ImGui::GetColorU32(ImGuiCol_Border), st.WindowRounding, 0, 1.0f);
    }

    // 录快捷键时手柄和键盘都不用来导航（按什么都是在录），下一帧起生效
    const bool capturing = g_cap.spec != nullptr;
    UpdateCapture(m);
    const ImGuiConfigFlags navFlags = ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    if (g_cap.spec) ImGui::GetIO().ConfigFlags &= ~navFlags;
    else ImGui::GetIO().ConfigFlags |= navFlags;

    // B / Esc 返回（主页上就是关面板）。弹窗、输入框、录快捷键时不算。
    // 弹窗里按 B 是关弹窗：ImGui 在这一帧开头就已经把弹窗关掉了，所以还要看上一帧开没开着，
    // 不然同一下 B 又会被当成「返回上一页」
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || g_popupLastFrame;
    const bool typing = ImGui::GetIO().WantTextInput;
    const bool backKey = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    // 拖过位置的标记留到松开那一帧（松开时 Row 会报「点了」，要靠它认出来不算）
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) g_dragMoved = false;
    if (!popup && !typing && !capturing && backKey) {
        if (Back(g_page) != g_page) Go(Back(g_page));
        else if (g_page != kHome) Go(kHome);
        else m.reqClose = true;
    }

    const bool sideNav = Sidebar(0, 0, sideW, H);

    // 焦点在侧栏时，内容区不参与十字键导航：上下只在侧栏里走，不会被右边离得近的一行抢走；
    // 回内容区由侧栏自己处理（按右 / 按 A 落到这一页的默认项）
    if (sideNav) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::SetCursorPos(ImVec2(sideW + pad - inPad.x, pad - inPad.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, inPad);
    ImGui::BeginChild("##content", ImVec2(W - sideW - pad * 2 + inPad.x * 2, hc),
                      ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding,
                      g_page == kQuests ? ImGuiWindowFlags_NoScrollbar : 0);
    ImGui::PopStyleVar();
    g_focusUsed = false;
    const Page drawn = g_page;
    switch (g_page) {
    case kHome: PageHome(m); break;
    case kQuests: PageQuests(m); break;
    case kSets: PageSets(m); break;
    case kSetEdit: PageSetEdit(m); break;
    case kFood: PageFood(m); break;
    case kFoodEdit: PageFoodEdit(m); break;
    case kSettings: PageSettings(m); break;
    }
    // 默认项这一帧没画出来（比如被搜索过滤掉了）就作废，免得之后抢焦点。
    // 但这一帧中途换了页的话，请求是给新页的，留到下一帧
    if (!g_focusUsed && g_page == drawn) g_focus = 0;
    g_contentH[drawn] = ImGui::GetCursorPosY() + inPad.y;   // 这一页内容实际多高，下一帧按它收紧面板
    ImGui::EndChild();
    if (sideNav) ImGui::PopItemFlag();

    // 十字键往左、内容区里左边已经没有能去的控件：直接去侧栏当前页那一格。
    // 这一帧所有控件都已经参与过打分，结果是空的就取消这次移动，换成我们指定的落点。
    ImGuiContext& g = *GImGui;
    if (!sideNav && !popup && g_sideCurId && g.NavMoveSubmitted && g.NavMoveScoringItems && g.NavMoveDir == ImGuiDir_Left &&
        !g.NavMoveResultLocal.ID && !g.NavMoveResultLocalVisible.ID && !g.NavMoveResultOther.ID) {
        ImGui::NavMoveRequestCancel();
        ImGui::SetFocusID(g_sideCurId, ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
    }
    m.capturing = g_cap.spec != nullptr;
    g_popupLastFrame = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

    if (!m.status.empty()) {
        ImGui::SetCursorPos(ImVec2(sideW + pad, (pad - inPad.y) + hc + 6 * S));
        Text(fReg, szSmall, kMuted, m.status.c_str());
    }
    ImGui::End();

    // 外框在收放时，面板（连同里面的子窗口）这一帧画出来的东西裁到外框里。弹窗是独立的窗口，不受影响。
    if (Hf < H - 0.5f)
        for (ImGuiWindow* w : GImGui->Windows)
            if (w->RootWindow == panel && w->WasActive && w->DrawList)
                for (ImDrawCmd& c : w->DrawList->CmdBuffer) {
                    c.ClipRect.y = std::fmax(c.ClipRect.y, fMin.y);
                    c.ClipRect.w = std::fmin(c.ClipRect.w, fMax.y);
                }
}

} // namespace ui
