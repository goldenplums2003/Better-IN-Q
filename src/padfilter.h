#pragma once
// ============================================================================
//  padfilter.h —— 游戏读手柄时经过这里
//
//  1. 面板开着：游戏读到的是「什么都没按」。关面板后要等所有键松开才恢复，
//     免得在面板里按的 A 漏给游戏。
//  2. 面板组合键（默认 视图键+菜单键）：这两个键单按在游戏里有用（地图、菜单）。
//     按下其中一个时先扣住最多 100ms：期间凑齐整个组合 → 游戏收不到这两个键；
//     没凑齐 → 照常放行（晚 0.1 秒）；没等到就松开了 → 补发一次点按。
//     组合里别的键（出招键）不扣，照常立刻给游戏，免得连招被吞。
//
//  做法：在游戏模块里找到「指向 XInputGetState 的指针」（导入表或它自己存的函数指针），
//  换成我们的函数。exe 有 Steam 壳，真正的导入表要等游戏跑起来才解开，所以按值找。
//  我们自己的快捷键和 ImGui 都是自己 GetProcAddress 拿的原函数，不受影响。
// ============================================================================
#include <string>

namespace padfilter {

// 找到并替换游戏的 XInputGetState 指针。xinput 还没加载时返回 false，可以过一会儿再试。
bool Install(std::string& info);
bool Installed();

void SetBlock(bool block);            // 面板开着 → 挡住全部输入
void SetChord(unsigned short mask);   // 面板组合键里的手柄按钮（XINPUT_GAMEPAD_* 位），0 = 不扣

// 诊断：接下来 ms 毫秒里，游戏每次读到的按键有变化就记下来（重开时调）。
// 记录在游戏线程里只排队，FlushTrace（主循环里调）才写进日志。
void Trace(unsigned long ms);
void FlushTrace();

// 测试用：直接对一份手柄状态跑一遍过滤（不经过 XInput）
void TestFilter(unsigned long pad, void* xinputState);

} // namespace padfilter
