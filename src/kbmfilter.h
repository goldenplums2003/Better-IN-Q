#pragma once
// ============================================================================
//  kbmfilter.h —— 游戏读键盘、鼠标按键时经过这里
//
//  窗口消息（WM_KEYDOWN、WM_LBUTTONDOWN 这些）overlay 的 WndProc 已经吃掉了，但游戏
//  读键鼠还有两条路，面板开着时打字、点鼠标会漏过去（按 M 开地图）：
//    1. DirectInput：键盘、鼠标设备的 GetDeviceState（当前状态）/ GetDeviceData（事件）
//    2. GetAsyncKeyState / GetKeyState：直接查某个键
//
//  面板开着时：
//    GetDeviceState  键盘、鼠标读到「什么都没按、鼠标没动」
//    GetDeviceData   扔掉按下和鼠标移动的事件，松开的事件照常给（开面板前按着的键能松开）
//    按键查询        读到「没按」（Caps Lock 这类开关状态照旧）
//  关面板后，面板开着时按下的键要等松开才放行（和 padfilter 一样，免得面板里按的键漏给游戏）。
//
//  DirectInput：同一个 dinput8 里的设备对象共用一张虚表，自己建一个键盘、一个鼠标设备
//  拿到虚表地址，把那两格换成我们的函数。按键查询和 cursorfilter 一样，在游戏模块的
//  数据段里按值换指针；面板自己（ImGui、快捷键）调的是我们 DLL 自己的导入，不受影响。
// ============================================================================
#include <string>

namespace kbmfilter {

// dinput8 还没加载、或者游戏模块里还没有按键查询函数的指针时返回 false，可以过一会儿再试。
// 每次调用都会把能装的先装上（DirectInput 装上了就不再动）。
bool Install(std::string& info);
bool Installed();

void SetBlock(bool block);   // 面板开着 → 游戏读不到键鼠

// 测试用：不经过 DirectInput，直接对一份数据跑一遍过滤。kind：1 键盘 2 鼠标
void TestState(int kind, void* data, unsigned long cb);
unsigned long TestData(int kind, void* objectData, unsigned long cbObject, unsigned long count);
short TestKey(int vk, short real, bool getKeyState);

} // namespace kbmfilter
