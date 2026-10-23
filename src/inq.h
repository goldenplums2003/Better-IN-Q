#pragma once
// ============================================================================
//  inq.h —— IN-Q 的核心逻辑（移植自 INQ.CT）
//
//  CT 里所有状态都放在 alloc 出来的 INQ 这块内存里，钩子和 Lua 定时器通过它
//  通信；这里换成一个结构体，含义和偏移一一对应（注释里的 INQ+x）。
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

namespace inq {

struct State {
    // INQ+0：重开流程的状态机
    //   0 空闲  1 请求重开  2 重置中（拦住回据点的加载）  3 等接任务  4 等出发
    volatile std::int32_t restart = 0;
    volatile std::uint8_t optReset = 0;       // INQ+4  重置后从据点重接（不勾 = 直接重开）
    volatile std::uint8_t optAcceptOnly = 0;  // INQ+5  只接任务不出发
    volatile std::uint8_t resetOnly = 0;      // INQ+6  这一次只重置回据点，不接任务（快捷键给的）
    volatile std::uint8_t optWyvern = 0;      // INQ+7  普通地图用翼龙起始
    volatile std::uint32_t questId = 0;       // INQ+8  0 = 用上一次的任务
    volatile std::int32_t food = 0;           // INQ+C  进任务时套用的猫饭（编号，0 = 不管）
    volatile std::int32_t equipLoadout = 0;   // INQ+10 待套用的装备套装 1..224
    volatile std::int32_t itemLoadout = 0;    // INQ+14 待套用的道具套装 1..80
    volatile std::uint32_t message = 0;       // INQ+50 提示位：1 装备套装无效 2 接任务失败
                                              //                4 现在不能用 8 请先关闭窗口
};
extern State g;

// 等游戏代码解密后核对字节、装钩子。report 是给人看的结果（写日志、发聊天框）。
// 返回 false = 核心钩子没装上，重开功能不可用。
bool Install(const Config& cfg, std::string& report);
// 插件一加载就调：趁游戏还没把模块附近的地址占满，先把挂钩要的那块内存要下来
void ReserveArena();
bool Ready();

// 重开（快捷键或面板）。会先套用 Active 那套全套配置。重开方式看配置里的 Reset / AcceptOnly。
// 任务选的是「无」：不重开，只原地套用装备、道具、猫饭。
// resetOnly：只重置回据点，不接任务、不套全套。
void RequestRestart(const Config& cfg, bool resetOnly = false);

// 每 100ms 调一次（CT 里 Lua 定时器做的事）
void Tick(const Config& cfg);

// 把配置里的开关同步进状态（启动时和改配置后）
void ApplyOptions(const Config& cfg);

// 装备/道具套装：配置里写的是编号或游戏里的名字，这里解析成编号（0 = 找不到）
int ResolveEquip(const std::string& s);
int ResolveItem(const std::string& s);

// 游戏里的套装名（给面板列选项用）。装备按存放格，道具按游戏里的显示编号，只列有内容的。
struct NamedLoadout { int number; std::string name; };
std::vector<NamedLoadout> ListEquipLoadouts();
std::vector<NamedLoadout> ListItemLoadouts();

} // namespace inq
