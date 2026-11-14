#pragma once
// ============================================================================
//  filelog.h —— 把游戏读了哪些音效文件、什么时候读的写进日志（排查用）
//
//  查煌黑龙终结审判的音效在 INQ 重置后丢失：游戏加载资源时会先去 nativePC 里看有没有同名的
//  散文件（mod 就是这样替换音效的），所以截下它查文件用的系统调用，就能看到每个音效库
//  （sound\...\*.nbnk 之类）是什么时候加载的。对比正常进任务和 INQ 重置，看少读了哪个。
//
//  做法和 cursorfilter 一样：在游戏模块的数据段里按值换 CreateFile / GetFileAttributes /
//  FindFirstFile 的指针。路径里带 sound 的逐条记下（时刻、哪个调用、成没成），别的只计数。
//  游戏线程里只排队，Drain（主循环里调）才拿出来写日志。
// ============================================================================
#include <string>
#include <vector>

namespace filelog {

bool Install(std::string& info);
bool Installed();
// 取出攒下的记录（已经排成一行行日志文字）。每秒最多再附一行「别的 nativePC 查询 N 次」
std::vector<std::string> Drain();

} // namespace filelog
