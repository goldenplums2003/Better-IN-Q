#pragma once
// ImGui 的编译配置：build.sh 用 -DIMGUI_USER_CONFIG 带进来，ImGui 本体和我们的代码用的是同一份。
//
// 断言：ImGui 默认用 assert()，我们没定义 NDEBUG，失败了就 abort()，在游戏里就是闪退。
// 改成记一笔日志接着跑：ImGui 的断言基本都是「用法不对」的提醒，跳过去顶多这一帧画错，好过把游戏带崩
void ImGuiAssertFailed(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : ImGuiAssertFailed(#_EXPR, __FILE__, __LINE__))
