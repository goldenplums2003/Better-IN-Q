// ============================================================================
//  inq.cpp —— INQ.CT 的移植
//
//  两个上下文钩子（ApplyResetAndEquip / QuestLoad）跑在游戏线程里，逻辑和 CT
//  的汇编逐路径对应，连「改了寄存器再直接返回」的分支也照搬 —— 游戏后续代码
//  看到的寄存器值必须和用 CT 时一样。CT 里用 db 手写的相对跳转，按字节数核对过，
//  跳到的就是注释里写的 Z / Y / [Restart],0 那几处。
// ============================================================================
#include "inq.h"

#include "foodgate.h"
#include "game.h"
#include "hook.h"
#include "log.h"

#include <windows.h>
#include <cstdlib>
#include <cstring>

namespace inq {

State g;

namespace {

using namespace game;
using hook::Ctx;

bool g_ready = false;

// 诊断：钩子在游戏线程里只记个数，Tick 里再写日志
volatile LONG g_evEquip = 0, g_evReset = 0, g_evAccept = 0, g_evDepart = 0;
volatile LONG64 g_mantleCalls = 0, g_mantleSkips = 0;   // 衣装修复那个函数：被调几次 / 被我们提前返回几次

enum MsgBit : std::uint32_t { kMsgEquip = 1, kMsgQuest = 2, kMsgCant = 4, kMsgWindow = 8, kMsgNoQuest = 16 };

// ---------------------------------------------------------------------------
//  ApplyResetAndEquip：原指令 mov rcx,rbx ; call [rax+70]
//  只在 ax==71D0 && bx==02A0（某个 UI 对象的那一次调用）时介入：
//    · 有待套用的装备套装 → 借这次调用让游戏自己换装（换虚表、换对象）
//    · 否则如果请求了重开 → 判断当前能不能重开，能就借这次调用触发游戏的重置
// ---------------------------------------------------------------------------
void CantQ()
{
    g.message |= kMsgCant;
    g.restart = 0;
    g.resetOnly = 0;
}

void OnApplyResetAndEquip(Ctx* c)
{
    if ((c->rax & 0xFFFF) != 0x71D0 || (c->rbx & 0xFFFF) != 0x02A0) return;

    // ---------------- 装备套装 ----------------
    if (g.equipLoadout != 0) {
        const std::uint32_t n = (std::uint32_t)g.equipLoadout;
        g.equipLoadout = 0;
        if (n > 224) { g.message |= kMsgEquip; return; }           // popp：寄存器原样
        const std::uintptr_t slot = SaveSlot();
        if (!slot) { g.message |= kMsgEquip; return; }
        const std::uint32_t i = n - 1;
        if (Rd<std::uint32_t>(slot + std::uintptr_t(i) * 0x2B0 + 0x10CCF4) == 0xFFFFFFFF) {
            g.message |= kMsgEquip;                                  // 这一格没放武器 = 空套装
            return;
        }
        const std::uint32_t page = i / 8, idx = i % 8;
        const std::uintptr_t ui = Ui();
        const std::uintptr_t rcx = ui ? P(ui + 0x310) : 0;
        const std::uintptr_t a60 = ui ? P(ui + 0x60) : 0;
        if (!rcx || !a60) { g.message |= kMsgEquip; return; }
        std::uintptr_t r10 = P(rcx + 0x2960);
        Wr<std::uint32_t>(r10 + 0x240, page);
        r10 = P(rcx + 0x2958);
        Wr<std::uint32_t>(r10 + 0x274, idx);
        const std::uintptr_t rdi = a60 - 0x80;
        const std::uintptr_t rbx = P(a60 + 0x470);
        const std::uintptr_t rsi = rbx - 0x5E0;
        Wr<std::uint64_t>(rdi + 0x2A60, rdi + 0x2A28);
        Wr<std::uint64_t>(rdi + 0x2A30, rcx);
        Wr<std::uint64_t>(rdi + 0x2A28, kEquipCmd);
        c->rax = kVtEquipApply;
        c->rbx = rbx;
        c->rcx = rcx;
        c->rdx = idx;          // div ebx 之后 edx = 余数（高 32 位清零）
        c->rdi = rdi;
        c->rsi = rsi;
        c->r10 = r10;
        ::InterlockedIncrement(&g_evEquip);
        return;
    }

    // ---------------- 重开 ----------------
    if (g.restart != 1) return;
    const std::uintptr_t ui = Ui();
    if (!ui) { CantQ(); return; }
    const std::uintptr_t load = P(ui + 0x20);
    if (Rd<std::uint32_t>(load + 0x1D04) != 0) { CantQ(); return; }   // 正在读盘
    const std::uintptr_t q = P(pQuest);
    if (!q) { CantQ(); return; }
    const std::uint32_t st = Rd<std::uint32_t>(q + 0x38);

    if (st == 4) {                                                     // skipR：结算画面
        if (Rd<std::uint32_t>(q + 0x58) != 4) { CantQ(); return; }
        Wr<float>(q + 0x131A0, 3601.0f);                               // 把结算计时拨到头
        if (g.resetOnly) { g.resetOnly = 0; g.restart = 0; }           // 只重置：照常回据点就完了
        return;                                                        // restart 保持 1，回据点后接着走
    }
    if (st == 5) {                                                     // skipR2
        if (Rd<std::uint32_t>(q + 0x58) != 4) { CantQ(); return; }
        Wr<std::uint32_t>(q + 0x38, 7);
        g.restart = 0;
        g.resetOnly = 0;
        return;
    }
    if ((st == 1 || st == 0xD) && g.resetOnly) {                       // 只重置，但本来就在据点 / 调查
        g.resetOnly = 0;
        g.restart = 0;
        g.message |= kMsgNoQuest;
        return;
    }
    g.restart = 3;
    if (st == 1 || st == 0xD) return;                                  // 在据点 / 调查 → 直接去接任务
    if (st != 2 || Rd<std::uint32_t>(q + 0x54) != 2) { CantQ(); return; }   // 不在任务里

    // optioncheck（CT 的 Restart+6：这一次只重置 → restart 归 0，不拦回据点、也不接任务）
    g.restart = 3;
    if (g.resetOnly) {
        g.resetOnly = 0;
        g.restart = 0;
    } else if (!(g.optAcceptOnly == 1 || g.optReset == 1)) {
        g.restart = 2;                                                 // 直接重开：拦住回据点
    }

    // ifquest：死了的话先补满血、把动作清零，免得重置卡在倒地状态
    const std::uintptr_t pl = P(P(pPlayerA) + 0x108);
    const std::uintptr_t hp = pl ? P(pl + 0x7670) : 0;
    if (hp && Rd<std::uint32_t>(hp + 0x64) == 0) {
        Wr<float>(hp + 0x64, 100.0f);
        Wr<std::uint32_t>(pl + 0x62B8, 0);
    }

    // resetmain：借这次调用触发游戏自己的重置函数
    const std::uintptr_t a = P(ui + 0xA8);
    if (!a) { CantQ(); return; }
    const std::uintptr_t rdi = a - 0xB0;
    const std::uintptr_t rsi = P(a + 0x260);
    if (!rsi) { CantQ(); return; }
    Wr<std::uint64_t>(rsi + 0x2478, kResetCmd);
    Wr<std::uint64_t>(rsi + 0x2480, rsi + 0x1850);
    Wr<std::uint64_t>(rsi + 0x24B0, rsi + 0x2478);
    Wr<std::uint64_t>(rdi + 0x2AD0, kResetCmd2);
    Wr<std::uint32_t>(rdi + 0x1DB4, 2);
    Wr<std::uint8_t>(rdi + 0x2348, 0xC9);
    Wr<float>(rsi + 0x2410, 10.0f);
    Wr<std::uint32_t>(rsi + 0x2888, 0xA);
    c->rax = kVtReset;
    c->rbx = rsi + 0x2270;
    c->rdi = rdi;
    c->rsi = rsi;
    ::InterlockedIncrement(&g_evReset);
}

// ---------------------------------------------------------------------------
//  QuestLoad：原指令 test dword [rcx+2298],80000011（rcx = 任务柜台的 UI 对象）
//    restart==3 → 接任务（任务 ID 无效就用上一次的）
//    restart==4 → 出发
// ---------------------------------------------------------------------------
void Stop(Ctx* c, std::uintptr_t rax)
{
    g.message |= kMsgQuest;
    g.restart = 0;
    c->rax = rax;
}

void OnQuestLoad(Ctx* c)
{
    const std::uintptr_t o = c->rcx;
    const std::uintptr_t ui = Ui();

    if (g.restart == 4) {                                              // goquest
        const std::uintptr_t r = ui ? P(ui + 0x3B8) : 0;
        if (!r) return;
        c->rax = r;
        if (Rd<std::uint8_t>(o + 0x2298) == 0xEA) { Stop(c, r); return; }
        if (Rd<std::uint32_t>(r + 0x28F0) == 0) return;
        Wr<std::uint32_t>(r + 0x28F0, 8);                              // 出发
        const std::uintptr_t q = P(pQuest);
        c->rbx = q;
        const std::uint32_t stage = Rd<std::uint32_t>(q + 0x291B0);
        // 普通地图（101~109）默认营地起始；勾了翼龙起始就不改
        if (stage > 100 && stage <= 109 && g.optWyvern != 1) Wr<std::uint32_t>(r + 0x28F8, 1);
        g.restart = 0;
        ::InterlockedIncrement(&g_evDepart);
        return;
    }
    if (g.restart != 3 || !ui) return;

    const std::uintptr_t load = P(ui + 0x20);
    c->rax = load;
    if (Rd<std::uint32_t>(load + 0x1D04) != 0) return;                // 还在读盘
    const std::uintptr_t pb = P(P(pPlayerB) + 0x108);
    const std::uintptr_t q = P(pQuest);
    c->rbx = pb;
    c->rax = q;
    if (g.optReset | g.optAcceptOnly) {                                // word [Restart+4] != 0
        const std::uint32_t st = Rd<std::uint32_t>(q + 0x38);
        if (st == 1) { if (Rd<std::uint32_t>(pb + 0x1138) != 3) return; }      // 据点：等角色站稳
        else if (st == 0xD) { if (Rd<std::uint32_t>(pb + 0x10F0) != 3) return; } // 调查：同上
        else return;
    }
    // checkaccept
    const bool acceptOnly = g.optAcceptOnly == 1;
    g.restart = acceptOnly ? 0 : 4;

    // accept
    const std::uintptr_t rbx = P(ui + 0x3C8);
    c->rbx = rbx;
    std::uint32_t qid = g.questId;
    if (qid < 101 || qid > 67809) {                                    // Z：用上一次的任务
        const std::uintptr_t last = P(ui + 0x3B8);
        c->rax = last;
        if (Rd<std::uint32_t>(last + 0x2B94) == 0xFFFFFFFF) { Stop(c, last); return; }
        qid = Rd<std::uint32_t>(last + 0x2B94);
        g.questId = qid;
    }
    // Y
    Wr<std::uint32_t>(rbx + 0x292C, qid);
    Wr<std::uint64_t>(o + 0x2938, rbx);
    Wr<std::uint32_t>(o + 0x2940, 0xFFFFFFFF);
    Wr<std::uint64_t>(o + 0x2968, o + 0x2930);
    Wr<float>(o + 0x2AC8, 1.0f);
    Wr<std::uint64_t>(o + 0x2930, kQuestAcceptCmd);
    Wr<std::uint8_t>(o + 0x2298, 0xC9);
    Wr<std::uint32_t>(o + 0x1D04, 2);
    c->rax = kQuestAcceptCmd;
    ::InterlockedIncrement(&g_evAccept);
}

// ---------------------------------------------------------------------------
//  安装
// ---------------------------------------------------------------------------
bool BytesAre(std::uintptr_t a, const char* hex)
{
    std::uint8_t want[32];
    std::size_t n = 0;
    for (const char* p = hex; *p && n < sizeof(want);) {
        if (*p == ' ') { ++p; continue; }
        want[n++] = (std::uint8_t)std::strtoul(std::string(p, 2).c_str(), nullptr, 16);
        p += 2;
    }
    return Readable(a, n) && std::memcmp(reinterpret_cast<const void*>(a), want, n) == 0;
}

std::string Hex(std::uintptr_t a, std::size_t n)
{
    if (!Readable(a, n)) return "(读不到)";
    std::string s;
    char b[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02X ", reinterpret_cast<const std::uint8_t*>(a)[i]);
        s += b;
    }
    return s;
}

template <class T> bool PatchValue(std::uintptr_t a, T expect, T to, const char* what, std::string& rep)
{
    T cur{};
    std::memcpy(&cur, reinterpret_cast<const void*>(a), sizeof(T));
    if (std::memcmp(&cur, &expect, sizeof(T)) != 0) {
        Log("skip %s: 当前值不是原值（%s）", what, Hex(a, sizeof(T)).c_str());
        rep += std::string("·") + what + "没改（被改过或版本不同）\n";
        return false;
    }
    hook::WriteMem(a, &to, sizeof(T));
    Log("patched %s", what);
    return true;
}

} // namespace

bool Ready() { return g_ready; }

void ReserveArena()
{
    std::uintptr_t tb = 0;
    std::size_t tl = 0;
    if (hook::TextRange(EXE, tb, tl)) hook::InitArena(tb, tb + tl);
}

bool Install(const Config& cfg, std::string& report)
{
    report.clear();
    std::uintptr_t tb = 0;
    std::size_t tl = 0;
    if (!hook::TextRange(EXE, tb, tl)) { report = "读不到游戏的代码段"; return false; }

    // Steam 的壳要等游戏自己把代码解开。等到固定点位的原始字节出现（最多 3 分钟）。
    struct Fixed { std::uintptr_t at; const char* bytes; const char* name; };
    const Fixed fixed[] = {
        {kBaseLoad,   "E8 2C 71 7A FF", "BaseLoad"},
        {kBaseLoad2,  "E8 3B A2 F5 FF", "BaseLoad2"},
        {kQuickLoad,  "E8 B6 DA FF FF", "QuickLoad"},
        {kMantleLoad, "40 53 48 83 EC 40 48 8B D9 E8 82 9D", "MantleLoad"},
    };
    const ULONGLONG t0 = ::GetTickCount64();
    for (;;) {
        int ok = 0;
        for (const auto& f : fixed) ok += BytesAre(f.at, f.bytes);
        if (ok > 0) break;
        if (::GetTickCount64() - t0 > 180000) break;
        ::Sleep(200);
    }
    ::Sleep(1500);   // 让同时启动的别的模组先打完它们的补丁，再看现场
    Log("代码段就绪（等了 %llu ms）", (unsigned long long)(::GetTickCount64() - t0));

    if (!hook::InitArena(tb, tb + tl)) { report = "要不到可执行内存"; return false; }
    if (hook::ArenaFar())
        Log("arena = %p（游戏模块附近已经没有空地址，挂钩改走 .text 尾部的跳板，跳板区 %zu 字节）",
            (void*)hook::ArenaBase(), hook::CaveFree());
    else
        Log("arena = %p", (void*)hook::ArenaBase());

    bool core = true;
    auto need = [&](bool ok, const char* name, const std::string& why) {
        if (ok) { Log("hooked %s", name); return; }
        core = false;
        Log("FAILED %s: %s", name, why.c_str());
        report += std::string("·") + name + "：" + why + "\n";
    };

    // 特征码：必须恰好一处
    const auto h1 = hook::FindAll(tb, tl, kAobApplyResetAndEquip);
    const auto h2 = hook::FindAll(tb, tl, kAobQuestLoad);
    Log("AOB ApplyResetAndEquip: %zu 处%s", h1.size(), h1.size() == 1 ? "" : "（必须恰好 1 处）");
    Log("AOB QuestLoad: %zu 处", h2.size());

    // 固定点位
    bool fixedOk[4];
    for (int i = 0; i < 4; ++i) {
        fixedOk[i] = BytesAre(fixed[i].at, fixed[i].bytes);
        if (!fixedOk[i]) Log("%s 现场字节不对：%s", fixed[i].name, Hex(fixed[i].at, 12).c_str());
    }

    // 核心钩子要么全装，要么一个都不装 —— 装一半比不装更糟（会卡在重置中）
    if (h1.size() == 1 && h2.size() == 1 && fixedOk[0] && fixedOk[1] && fixedOk[2]) {
        static const std::uint8_t d1[] = {0x48, 0x8B, 0xCB, 0xFF, 0x50, 0x70};
        static const std::uint8_t d2[] = {0xF7, 0x81, 0x98, 0x22, 0x00, 0x00, 0x11, 0x00, 0x00, 0x80};
        need(hook::InstallCtxHook(h1[0], 6, OnApplyResetAndEquip, d1, sizeof(d1)), "ApplyResetAndEquip", "装钩子失败");
        need(hook::InstallCtxHook(h2[0], 10, OnQuestLoad, d2, sizeof(d2)), "QuestLoad", "装钩子失败");
        need(hook::InstallCallGate(kBaseLoad, &g.restart, 2, -1), "BaseLoad", "装钩子失败");
        need(hook::InstallCallGate(kBaseLoad2, &g.restart, 2, 3), "BaseLoad2", "装钩子失败");
        need(hook::InstallCallGate(kQuickLoad, &g.restart, 2, -1), "QuickLoad", "装钩子失败");
    } else {
        core = false;
        report += "·重开用到的代码和预期对不上：游戏版本不同，或者被别的模组改过"
                  "（同时开着 IN-Q.exe 也会这样）\n";
        if (h1.size() != 1) report += "  ApplyResetAndEquip 特征码找到 " + std::to_string(h1.size()) + " 处\n";
        if (h2.size() != 1) report += "  QuestLoad 特征码找到 " + std::to_string(h2.size()) + " 处\n";
        for (int i = 0; i < 3; ++i)
            if (!fixedOk[i]) report += std::string("  ") + fixed[i].name + " 被改过\n";
    }

    // 黑龙开局脱衣装的修复：函数开头，rsi==0 时直接返回。这是修 bug，一直开着
    {
        if (fixedOk[3]) {
            // 前后各数一下次数（诊断用，lock inc 不碰别的寄存器，标志位随后被 test 重写）
            auto count = [](hook::Emitter& e, volatile LONG64* p) {
                e.b({0x50, 0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(p))   // push rax ; mov rax,imm64
                 .b({0xF0, 0x48, 0xFF, 0x00, 0x58});                              // lock inc qword [rax] ; pop rax
            };
            hook::Emitter e;
            count(e, &g_mantleCalls);
            e.b({0x48, 0x85, 0xF6, 0x75, 0x11});            // test rsi,rsi ; jnz 继续（跳过下面 17 字节）
            count(e, &g_mantleSkips);                        // 16 字节
            e.b({0xC3})                                      // ret
             .b({0x40, 0x53, 0x48, 0x83, 0xEC, 0x40})       // 继续：push rbx ; sub rsp,40
             .jmpAbs(kMantleLoad + 6);
            const std::uintptr_t s = hook::Commit(e);
            if (s && hook::PatchJump(kMantleLoad, 6, s)) Log("hooked MantleLoad");
            else report += "·衣装修复没装上\n";
        } else {
            report += "·衣装修复没装（那里被别的模组改过，比如 MantleRemover）\n";
        }
    }

    // 缩短转场黑屏 / 快速重置
    if (core && cfg.quickReset) PatchValue<std::uint32_t>(kQuickReset, 4, 5, "快速重置", report);
    else if (core) Log("快速重置：配置里关了，不改");
    if (cfg.quickBlackout) {
        PatchValue<float>(kBlackOut1, 1.4f, 0.1f, "转场黑屏1", report);
        PatchValue<float>(kBlackOut2, 0.5f, 0.1f, "转场黑屏2", report);
    }

    ApplyOptions(cfg);
    g_ready = core;
    return core;
}

void ApplyOptions(const Config& cfg)
{
    g.optReset = cfg.reset ? 1 : 0;
    g.optAcceptOnly = cfg.acceptOnly ? 1 : 0;
    g.optWyvern = cfg.wyvern ? 1 : 0;
    g.questId = (std::uint32_t)cfg.questId;   // 0 = 用上一次的任务（QuestLoad 里会去读）
    g.food = cfg.food;
}

// ---------------------------------------------------------------------------
//  套装名字 → 编号
//
//  装备套装：每格 0x2B0，第 n 格的武器字段在 存档+10CCF4+(n-1)*2B0（CT 用它判空）。
//  名字字段的位置 CT 没用到，先按「256 字节名字紧挨在武器字段前面」找，
//  找不到再在整张表里搜一遍，把实际位置写进日志 —— 第一次实机跑完就能定下来。
// ---------------------------------------------------------------------------
namespace {
bool IsNumber(const std::string& s, int& v)
{
    if (s.empty()) return false;
    for (char ch : s) if (ch < '0' || ch > '9') return false;
    v = std::atoi(s.c_str());
    return true;
}

// 在 [lo, hi) 里找以 \0 结尾、前面是 \0（或刚好在 4 字节边界）的 name
std::uintptr_t FindName(std::uintptr_t lo, std::uintptr_t hi, const std::string& name)
{
    if (!Readable(lo, hi - lo)) return 0;
    const char* p = reinterpret_cast<const char*>(lo);
    const std::size_t n = name.size();
    for (std::uintptr_t a = lo; a + n + 1 <= hi; ++a) {
        const char* q = reinterpret_cast<const char*>(a);
        if (q[0] != name[0] || std::memcmp(q, name.data(), n) != 0 || q[n] != '\0') continue;
        if (a == lo || q[-1] == '\0') return a;
    }
    (void)p;
    return 0;
}
} // namespace

int ResolveEquip(const std::string& s)
{
    int v = 0;
    if (IsNumber(s, v)) return (v >= 1 && v <= 224) ? v : 0;
    const std::uintptr_t slot = SaveSlot();
    if (!slot || s.empty()) return 0;
    const std::uintptr_t w0 = slot + 0x10CCF4;
    for (int n = 1; n <= 224; ++n) {
        const std::uintptr_t w = w0 + std::uintptr_t(n - 1) * 0x2B0;
        const char* name = reinterpret_cast<const char*>(w - 0x100);
        if (Readable(w - 0x100, 0x100) && std::strncmp(name, s.c_str(), 0x100) == 0) {
            Log("equip '%s' → %d（名字在武器字段前 0x100）", s.c_str(), n);
            return n;
        }
    }
    const std::uintptr_t a = FindName(w0 - 0x2B0, w0 + 224 * 0x2B0, s);
    if (a) {
        const long long off = (long long)a - (long long)w0;
        Log("equip '%s' 不在预想位置；实际在 武器字段0 %+lld（= 第 %lld 格 %+lld）", s.c_str(),
            off, off / 0x2B0 + 1, off % 0x2B0);
    } else {
        Log("equip '%s' 在装备套装表里没找到", s.c_str());
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  道具套装有两套编号（实机确认）：
//    存放格 s（1..80）：数据在存档里的位置。名字在 存档 + s*768 - 28，
//                       CT 的「是否有效」字段在 存档 + s*768 + 278。
//    显示编号 d（1..80）：游戏列表里看到的顺序，玩家排过序就和存放格不一样。
//                       套用命令要的是显示编号。
//  两者由 存档+37FF8 的 80 字节表连起来：表[d-1] = s-1。
//  老 IN-Q 把按名字查到的存放格直接当显示编号用，排过序的人就会套错（用户实测：
//  选「米拉2」（存放 5 / 显示 20）结果套上了显示第 5 号「重弩」）。
// ---------------------------------------------------------------------------
std::uintptr_t ItemNameAt(std::uintptr_t slot, int s) { return slot + std::uintptr_t(s) * 0x768 - 0x28; }
constexpr std::uintptr_t kItemOrder = 0x37FF8;

// 读顺序表；不是 0..79 的排列就当没排过序（显示编号 = 存放格）
bool ItemOrder(std::uintptr_t slot, std::uint8_t order[80])
{
    if (!Readable(slot + kItemOrder, 80)) return false;
    std::memcpy(order, reinterpret_cast<const void*>(slot + kItemOrder), 80);
    bool seen[80] = {};
    for (int i = 0; i < 80; ++i) {
        if (order[i] >= 80 || seen[order[i]]) {
            for (int j = 0; j < 80; ++j) order[j] = (std::uint8_t)j;
            Log("道具套装顺序表不是排列，按未排序处理");
            return false;
        }
        seen[order[i]] = true;
    }
    return true;
}

int ItemStoreOfDisplay(std::uintptr_t slot, int d)
{
    std::uint8_t order[80];
    ItemOrder(slot, order);
    return (d >= 1 && d <= 80) ? order[d - 1] + 1 : 0;
}

// 返回显示编号（套用命令要的就是它）。数字按游戏里看到的编号理解；名字先找存放格再换算。
int ResolveItem(const std::string& s)
{
    int v = 0;
    if (IsNumber(s, v)) return (v >= 1 && v <= 80) ? v : 0;
    const std::uintptr_t slot = SaveSlot();
    if (!slot || s.empty()) return 0;
    std::uint8_t order[80];
    ItemOrder(slot, order);
    for (int st = 1; st <= 80; ++st) {
        const std::uintptr_t a = ItemNameAt(slot, st);
        if (!Readable(a, 0x100) || std::strncmp(reinterpret_cast<const char*>(a), s.c_str(), 0x100) != 0)
            continue;
        for (int d = 1; d <= 80; ++d) {
            if (order[d - 1] + 1 == st) {
                Log("item '%s' → 存放第 %d 格 = 显示第 %d 号", s.c_str(), st, d);
                return d;
            }
        }
    }
    Log("item '%s' 在道具套装表里没找到", s.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
//  快捷键
// ---------------------------------------------------------------------------
namespace {
int g_pendingSetItem = 0;
bool g_pendingLink = false;
bool g_hasPendingLink = false;
int g_savedRadial = -1;   // CT 的 wassetting：套道具前「联动快捷栏」原来的值
FoodGate g_foodGate;      // 读盘时写不写猫饭：只在插件重开的那一轮写（见 foodgate.h）
constexpr ULONGLONG kStuckMs = 12000;   // 重开流程在一步上停这么久（不算读盘）就当卡住了

// 诊断：重开后 30 秒里，任务状态和钩子事件有变化就写日志
constexpr ULONGLONG kTraceMs = 30000;
ULONGLONG g_traceUntil = 0;
struct TraceSeen {
    std::uint32_t st = ~0u, ui = ~0u, load = ~0u;
    std::int32_t restart = -1;
    LONG64 mantleCalls = -1, mantleSkips = -1;
} g_tseen;

void TraceTick()
{
    if (::GetTickCount64() >= g_traceUntil) return;
    const std::uintptr_t q = P(pQuest);
    const std::uintptr_t ui = Ui();
    const std::uintptr_t load = ui ? P(ui + 0x20) : 0;
    TraceSeen n = g_tseen;
    n.st = q ? Rd<std::uint32_t>(q + 0x38) : ~0u;
    n.ui = q ? Rd<std::uint32_t>(q + 0x54) : ~0u;
    n.load = load ? Rd<std::uint32_t>(load + 0x1D04) : ~0u;
    n.restart = g.restart;
    if (n.st != g_tseen.st || n.ui != g_tseen.ui || n.load != g_tseen.load || n.restart != g_tseen.restart)
        Log("状态：场景 %d / 柜台 %d / 读盘 %d / restart %d", (int)n.st, (int)n.ui, (int)n.load, n.restart);
    if (LONG v = ::InterlockedExchange(&g_evEquip, 0)) Log("游戏线程：换了装备套装（%ld 次）", v);
    if (LONG v = ::InterlockedExchange(&g_evReset, 0)) Log("游戏线程：触发了重置（%ld 次）", v);
    if (LONG v = ::InterlockedExchange(&g_evAccept, 0)) Log("游戏线程：接了任务（%ld 次）", v);
    if (LONG v = ::InterlockedExchange(&g_evDepart, 0)) Log("游戏线程：出发（%ld 次）", v);
    n.mantleCalls = g_mantleCalls;
    n.mantleSkips = g_mantleSkips;
    if (n.mantleCalls != g_tseen.mantleCalls || n.mantleSkips != g_tseen.mantleSkips)
        Log("衣装修复：函数共被调 %lld 次，其中 rsi=0 提前返回 %lld 次", (long long)n.mantleCalls,
            (long long)n.mantleSkips);
    g_tseen = n;
}
}

// 把一套全套里的装备/道具/猫饭/任务排进待办。装备由 ApplyResetAndEquip 钩子在游戏线程里换，
// 道具由下一次 Tick 发 UI 命令，猫饭在下次读盘时写。
void QueueSet(const LoadoutSet& s)
{
    const int e = ResolveEquip(s.equip);
    const int it = ResolveItem(s.item);
    if (!s.equip.empty() && !e) Chat("Better-IN-Q：装备套装「" + s.equip + "」找不到");
    if (!s.item.empty() && !it) Chat("Better-IN-Q：道具套装「" + s.item + "」找不到");
    if (e) g.equipLoadout = e;
    if (it) { g_pendingSetItem = it; g_pendingLink = s.linkRadial; g_hasPendingLink = true; }
    if (s.food) g.food = s.food;
    if (s.quest > 100 && s.quest < 67809) g.questId = (std::uint32_t)s.quest;
}

// 把猫饭结构写成预设（CT：pfood+19A4+i*4，i=1..8）。成功返回 true。
bool WriteFood(const Config& cfg, int id)
{
    auto it = cfg.foods.find(id);
    const std::uintptr_t pf = P(pFood);
    if (it == cfg.foods.end() || !pf) return false;
    const FoodPreset& f = it->second;
    const std::int32_t v[8] = {f.hp, f.stamina, f.attack, f.defense, f.elemRes, f.skill[0], f.skill[1], f.skill[2]};
    for (int i = 0; i < 8; ++i) Wr<std::int32_t>(pf + 0x19A8 + i * 4, v[i]);
    return true;
}

// 面板里「原地换上这套」：不管选的什么任务，原地套用这一套的装备、道具、猫饭
void ApplySetInPlace(const Config& cfg, const std::string& name)
{
    if (!g_ready) { Chat("Better-IN-Q：重开功能没装上，详情见 BetterINQ.log"); return; }
    const LoadoutSet* set = cfg.FindSet(name);
    if (!set) return;
    QueueSet(*set);
    if (g.food > 0 && WriteFood(cfg, (int)g.food)) Log("猫饭 %d 已原地写入", (int)g.food);
    g_foodGate.Arm();   // 换完接着进任务：那次读盘也按这套的猫饭来
    Chat("Better-IN-Q：已原地套用「" + name + "」");
}

void RequestRestart(const Config& cfg, bool resetOnly)
{
    if (!g_ready) { Chat("Better-IN-Q：重开功能没装上，详情见 BetterINQ.log"); return; }
    if (g.restart != 0) return;
    g_traceUntil = ::GetTickCount64() + kTraceMs;
    g_tseen = TraceSeen();
    Log("—— 开始跟踪 %llu 秒（诊断手柄问题）——", (unsigned long long)(kTraceMs / 1000));
    if (resetOnly) {                       // 只重置回据点：不套全套
        g.resetOnly = 1;
        g.restart = 1;
        return;
    }
    const LoadoutSet* set = cfg.activeSet.empty() ? nullptr : cfg.FindSet(cfg.activeSet);
    if (!cfg.activeSet.empty() && !set) Chat("Better-IN-Q：配置里没有叫「" + cfg.activeSet + "」的全套");
    if (set) QueueSet(*set);
    // 猫饭先原地写一次：之前只在读盘时写，不读盘（任务选「无」、没选任务接不上）就换不上。
    // 读盘时照旧再写，进任务时以它为准。
    const bool food = g.food > 0 && WriteFood(cfg, (int)g.food);
    if (food) Log("猫饭 %d 已原地写入", (int)g.food);
    if (cfg.EffectiveQuest() == kQuestNone) {   // 任务选「无」：不重开
        std::string what = set ? "「" + set->name + "」" : (food ? cfg.FoodName((int)g.food) : "");
        g_foodGate.Arm();   // 原地换装后接着进任务：那次读盘也按插件的猫饭来
        Chat(what.empty() ? "Better-IN-Q：未选择任务、套装以及猫饭"
                          : "Better-IN-Q：已原地套用" + what);
        return;
    }
    g_foodGate.Arm();
    g.restart = 1;
}

std::vector<NamedLoadout> ListEquipLoadouts()
{
    std::vector<NamedLoadout> r;
    const std::uintptr_t slot = SaveSlot();
    if (!slot) return r;
    for (int n = 1; n <= 224; ++n) {
        const std::uintptr_t w = slot + 0x10CCF4 + std::uintptr_t(n - 1) * 0x2B0;
        if (!Readable(w - 0x100, 0x104) || Rd<std::uint32_t>(w) == 0xFFFFFFFF) continue;   // 空格子
        char name[0x101] = {};
        std::memcpy(name, reinterpret_cast<const void*>(w - 0x100), 0x100);
        r.push_back({n, name[0] ? std::string(name) : "装备套装 " + std::to_string(n)});
    }
    return r;
}

std::vector<NamedLoadout> ListItemLoadouts()
{
    std::vector<NamedLoadout> r;
    const std::uintptr_t slot = SaveSlot();
    if (!slot) return r;
    std::uint8_t order[80];
    ItemOrder(slot, order);
    for (int d = 1; d <= 80; ++d) {
        const int st = order[d - 1] + 1;
        if (Rd<std::uint32_t>(slot + std::uintptr_t(st) * 0x768 + 0x278) == 0) continue;   // 空套装
        const std::uintptr_t a = ItemNameAt(slot, st);
        char name[0x101] = {};
        if (Readable(a, 0x100)) std::memcpy(name, reinterpret_cast<const void*>(a), 0x100);
        r.push_back({d, name[0] ? std::string(name) : "道具套装 " + std::to_string(d)});
    }
    return r;
}

// ---------------------------------------------------------------------------
//  每 100ms（CT 的 Lua 定时器）
// ---------------------------------------------------------------------------
void Tick(const Config& cfg)
{
    TraceTick();
    const std::uintptr_t uib = P(pUiBase);
    if (!uib) return;
    const std::uintptr_t pui = P(uib + 0x278);
    if (!pui) return;
    const std::uintptr_t pload = P(pui + 0x20);
    const std::uintptr_t piui = P(pui + 0x170);
    const std::uintptr_t slot = SaveSlot();

    // 上一拍为套道具临时改的「联动快捷栏」，这一拍改回去。只动 1 个字节：
    // CT 按 4 字节读写这个奇数偏移，会把后面 3 个字节一起覆盖成 0。
    if (g_savedRadial != -1 && slot) {
        Log("联动快捷栏改回 %d（改之前读到 %d）", g_savedRadial, Rd<std::uint8_t>(slot + 0x140415));
        Wr<std::uint8_t>(slot + 0x140415, (std::uint8_t)g_savedRadial);
        g_savedRadial = -1;
    }

    // 道具套装（通过 UI 命令让游戏自己套用）
    if (g_pendingSetItem > 0 && slot && piui) {
        const int n = g_pendingSetItem;                    // 显示编号（套用命令要它）
        g_pendingSetItem = 0;
        const int st = ItemStoreOfDisplay(slot, n);         // 数据真正在哪一格
        const std::uint32_t valid = st ? Rd<std::uint32_t>(slot + std::uintptr_t(st) * 0x768 + 0x278) : 0;
        if (valid != 0) {
            if (g_hasPendingLink) {
                g_savedRadial = Rd<std::uint8_t>(slot + 0x140415);
                Wr<std::uint8_t>(slot + 0x140415, g_pendingLink ? 1 : 2);
                g_hasPendingLink = false;
            }
            Wr<std::uint64_t>(uib + 0x4F60, kItemLoadoutCmd);
            Wr<std::uint64_t>(uib + 0x4F68, (std::uint64_t)(n - 1));
            Wr<std::uint64_t>(uib + 0x4F98, uib + 0x4F60);
            Wr<std::uint64_t>(uib + 0x4FA0, piui);
            Wr<std::uint64_t>(uib + 0x4FA8, uib + 0xF6B0);
            Wr<std::uint64_t>(uib + 0x4FB0, uib + 0x4FB8);
            Wr<std::uint64_t>(uib + 0x4FB8, piui);
            Wr<std::uint64_t>(uib + 0x4FC0, uib + 0x4FA0);
            Wr<std::uint64_t>(uib + 0x4FD0, piui);
            Wr<std::uint64_t>(uib + 0x4FD8, 1);
            Wr<std::uint64_t>(uib + 0x4FE0, kItemLoadoutCmd2);
            Wr<std::uint64_t>(uib + 0x5018, uib + 0x4FE0);
            Wr<std::uint64_t>(uib + 0x8F60, 1);
            Log("item loadout %d 已提交（联动快捷栏 %d，原值 %d）", n, g_savedRadial == -1 ? -1 : (g_pendingLink ? 1 : 2),
                g_savedRadial);
        } else {
            Chat("Better-IN-Q：道具套装 " + std::to_string(n) + " 是空的");
        }
    }

    // 猫饭：读盘期间一直写成预设（进任务时游戏按它给饭的效果）
    const bool loading = pload && Rd<std::uint32_t>(pload + 0x1D04) != 0;
    if (g_foodGate.Step(loading, g.restart, cfg.foodEveryQuest) && g.food > 0) WriteFood(cfg, (int)g.food);

    // 看门狗：重开流程卡在某一步太久（读盘不算）就复位。比如营地接任务时游戏弹「已超出当前人数」
    // 把接任务挡住了，状态机一直等下去，之后再按重开全被挡掉，只能退游戏
    {
        static int s_last = 0;
        static ULONGLONG s_since = 0;
        const ULONGLONG now = ::GetTickCount64();
        const int r = g.restart;
        if (r == 0 || loading || r != s_last) {
            s_last = r;
            s_since = now;
        } else if (now - s_since > kStuckMs) {
            Log("重开卡在第 %d 步 %llu 秒没动，复位", r, (unsigned long long)((now - s_since) / 1000));
            g.restart = 0;
            g.resetOnly = 0;
            g_foodGate.Disarm();
            Chat("Better-IN-Q：重开被游戏挡住了，已复位，可以再按一次");
            s_last = 0;
            s_since = now;
        }
    }

    // 钩子里攒下的提示
    const std::uint32_t m = g.message;
    if (m) {
        g.message = 0;
        if (m & kMsgEquip) Chat("Better-IN-Q：这个装备套装是空的或无效");
        if (m & kMsgQuest) Chat("Better-IN-Q：接任务失败（没选过任务？）");
        if (m & (kMsgQuest | kMsgCant | kMsgNoQuest)) g_foodGate.Disarm();   // 重开没成：之后的读盘不归插件管
        if (m & kMsgCant) Chat("Better-IN-Q：现在不能重开");
        if (m & kMsgWindow) Chat("Better-IN-Q：请先关闭所有窗口");
        if (m & kMsgNoQuest) Chat("Better-IN-Q：目前不在任务中，无法重置回据点");
    }
}

} // namespace inq
