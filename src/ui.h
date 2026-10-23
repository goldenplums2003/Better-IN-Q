#pragma once
// ============================================================================
//  ui.h —— 面板本身（只画界面、处理按键，不碰游戏内存）
//
//  界面原则（参考 Revolut）：每一页只突出一件事。主页最大的是当前任务名，
//  最显眼的是「重开」大按钮，其余操作是一排圆按钮。层次靠字号和留白，不靠装饰。
//  手柄：十字键/左摇杆移动，A 确认，B 返回（主页按 B 关面板），往左进左侧栏切页。
// ============================================================================
#include <string>
#include <vector>

#include "config.h"

namespace ui {

struct LoadoutName {
    int number;          // 游戏里的编号（道具 = 列表显示编号；装备 = 存放格）
    std::string name;
};

// 面板和外界只通过它打交道。Draw 在渲染线程里调用，调用方持锁。
// 面板只「提请求」，真正干活（动游戏内存、写盘）由主循环看到请求后去做。
struct Model {
    Config* cfg = nullptr;
    bool ready = false;                   // 重开功能装上了没有
    std::string version;
    std::string status;                   // 底部提示（最近一条）
    std::vector<LoadoutName> equipNames;  // 游戏里的装备套装名（打开面板时刷新）
    std::vector<LoadoutName> itemNames;   // 游戏里的道具套装名（按显示编号）

    bool reqRestart = false;              // 立刻重开
    bool reqSave = false;                 // 配置改了，存盘并生效
    bool reqClose = false;                // 关面板

    // 设置快捷键：主循环每轮填现在按着的键，面板录完写回配置
    unsigned padHeld = 0;                 // 手柄（XINPUT_GAMEPAD_* 位 + kPadLT/kPadRT，见 input.h）
    std::vector<int> keysHeld;            // 键盘（只在录快捷键时填）
    bool capturing = false;               // 面板正在录快捷键：主循环别把按键当快捷键触发
    bool wantIme = false;                 // 正在打名字：要用输入法（打中文）。别的时候面板里不挂输入法
};

// ImGui 上下文建好后调一次。scale = 屏幕高度 / 1080。
void Init(float scale);
// 每次打开面板时调：回到主页、焦点放在「重开」上
void OnOpen(Model& m);
// 每帧
void Draw(Model& m, float screenW, float screenH);

// 预览工具用：直接跳到某页。0 主页 1 换任务 2 套装 3 套装编辑 4 猫饭 5 猫饭编辑 6 设置
void DebugGo(int page, int editIndex);
// 测试用：当前在哪一页（同上编号），焦点是不是在左侧栏
int DebugPage();
bool DebugSidebarFocused();
int DebugSidebarFocusIndex();   // 这一帧焦点在侧栏第几格（-1 = 不在侧栏）
float DebugFrameHeight();       // 这一帧画出来的外框有多高（换页时有过渡）
// 测试用：直接开始录设置页第 row 个快捷键（0 重开 1 面板 2 只重置回据点）的手柄 / 键盘那一组
void DebugStartCapture(Model& m, int row, bool pad);
std::string DebugCaptureWhy();
// 预览 / 宣传图用：往「换任务」的搜索框里填字
void DebugSetSearch(const char* text);
// 测试用：下一帧把键盘焦点放到编辑页（猫饭 / 套装）的名字框上（开始打字）；套装名字不合规的提示
void DebugFocusName();
std::string DebugNameWarn();

} // namespace ui
