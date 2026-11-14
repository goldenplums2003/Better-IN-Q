#pragma once
// ============================================================================
//  game.h —— 游戏版本 15.23.00 的地址，全部出自 INQ.CT（原作者 年糕狗三世，
//  QCanon 中文化）。MonsterHunterWorld.exe 没有开 ASLR，基址固定 0x140000000。
// ============================================================================
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace game {

constexpr std::uintptr_t EXE = 0x140000000;

// ---- 挂钩点（固定 RVA 的，后面跟着原始字节，用来核对版本和是否被别的模组改过）----
constexpr std::uintptr_t kBaseLoad   = EXE + 0x1B6797F;   // call +130EAB0
constexpr std::uintptr_t kBaseLoad2  = EXE + 0x1B66D80;   // call +1AC0FC0
constexpr std::uintptr_t kQuickLoad  = EXE + 0x1B71825;   // call +1B6F2E0
constexpr std::uintptr_t kMantleLoad = EXE + 0x1195720;   // push rbx ; sub rsp,40 ; mov rbx,rcx ; call ...
// ---- 用特征码找的 ----
constexpr const char* kAobApplyResetAndEquip = "48 8B CB FF 50 70 8B 4B 58";
constexpr const char* kAobQuestLoad = "F7 81 98 22 00 00 11 00 00 80 48 8B D9 0F 84 8F 00 00 00 F3";

// ---- 常量补丁 ----
constexpr std::uintptr_t kQuickReset  = EXE + 0x1B71596;  // dd 4 → 5（代码段里的立即数）
constexpr std::uintptr_t kBlackOut1   = EXE + 0x2FC6A1C;  // float 1.4 → 0.1
constexpr std::uintptr_t kBlackOut2   = EXE + 0x33F4F58;  // float 0.5 → 0.1

// ---- 全局指针 ----
constexpr std::uintptr_t pSave      = EXE + 0x5013950;    // 存档：+A8 基址，+A0 当前存档号，每档 0x26CC00
constexpr std::uintptr_t pUiBase    = EXE + 0x51C4640;    // +278 → UI 管理
constexpr std::uintptr_t pQuest     = EXE + 0x500ED30;    // 任务状态机
constexpr std::uintptr_t pFood      = EXE + 0x500ECA0;    // 猫饭
constexpr std::uintptr_t pPlayerA   = EXE + 0x50EC7E8;    // +108 → 玩家
constexpr std::uintptr_t pPlayerB   = EXE + 0x50EC750;    // +108 → 玩家（据点状态）
constexpr std::uintptr_t pPlayerRoot = EXE + 0x50139A0;   // 和模组制作器同一个：+50 = 玩家实体（在场景里）

// ---- 游戏里的函数/虚表（CT 里直接写成 exe+偏移的那些）----
constexpr std::uintptr_t kVtEquipApply   = EXE + 0x30CB600;
constexpr std::uintptr_t kEquipCmd       = EXE + 0x346D358;
constexpr std::uintptr_t kVtReset        = EXE + 0x30CBC90;   // CT 里写成绝对地址 0x1430CBC90
constexpr std::uintptr_t kResetCmd       = EXE + 0x30CA9B8;
constexpr std::uintptr_t kResetCmd2      = EXE + 0x3FC0EE0;
constexpr std::uintptr_t kQuestAcceptCmd = EXE + 0x32263D0;
constexpr std::uintptr_t kItemLoadoutCmd = EXE + 0x3460708;
constexpr std::uintptr_t kItemLoadoutCmd2 = EXE + 0x33F96F0;
constexpr std::uintptr_t kSystemMessage  = EXE + 0x1A540D0;   // 模组制作器在用的那个
constexpr std::uintptr_t kSystemMessageMgr = EXE + 0x500CE70;

constexpr std::uint32_t kSaveSlotSize = 0x26CC00;

// ---- 内存读写。处理函数跑在游戏线程里，指针链断了就当没事发生，别把游戏带崩 ----
inline bool Readable(std::uintptr_t a, std::size_t n)
{
    if (a < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!::VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return a + n <= end;
}

template <class T> inline T Rd(std::uintptr_t a)
{
    T v{};
    if (Readable(a, sizeof(T))) std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}
// 可写：整段在同一块已提交、可读写的内存里（往只读页上写会直接把游戏带崩）
inline bool Writable(std::uintptr_t a, std::size_t n)
{
    if (a < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!::VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    return a + n <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

template <class T> inline bool Wr(std::uintptr_t a, T v)
{
    if (!Writable(a, sizeof(T))) return false;
    std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T));
    return true;
}
inline std::uintptr_t P(std::uintptr_t a) { return Rd<std::uintptr_t>(a); }

// 当前存档的基址（CT：[[+5013950]+A8] + [[+5013950]+A0]*26CC00）
inline std::uintptr_t SaveSlot()
{
    const std::uintptr_t s = P(pSave);
    if (!s) return 0;
    const std::uintptr_t base = P(s + 0xA8);
    return base ? base + Rd<std::uint64_t>(s + 0xA0) * kSaveSlotSize : 0;
}
inline std::uintptr_t Ui() { const std::uintptr_t b = P(pUiBase); return b ? P(b + 0x278) : 0; }
inline bool InScene() { const std::uintptr_t m = P(pPlayerRoot); return m && P(m + 0x50); }

} // namespace game
