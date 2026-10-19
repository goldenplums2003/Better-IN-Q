// 挂钩引擎的单元测试：在本进程里摆出和游戏里一模一样的指令字节，挂上钩子，
// 看寄存器、xmm、标志位、被覆盖的原指令是不是都对。
// 带 --far 运行时，arena 故意放到 rel32 够不着的地方，所有挂钩都要经过跳板。
#include "../src/hook.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

extern "C" {
// ---- 测试 1：mov rcx,rbx ; call [rax+70] ; mov ecx,[rbx+58]（ApplyResetAndEquip 的样子）----
// int t1(void* obj, void* vtbl, double x)  返回 [obj'+58] + vfunc 的返回值；xmm 原样带出来
int  t1(void* obj, void** vtbl, double* io);
extern unsigned char t1_site[];
// ---- 测试 2：test dword [rcx+2298],80000011 ; mov rax,rcx ; je（QuestLoad 的样子）----
int  t2(void* obj);
extern unsigned char t2_site[];
// ---- 测试 3：call rel32（BaseLoad 的样子）----
int  t3(void);
extern unsigned char t3_site[];
int  g_t3calls = 0;
void t3_target(void) { ++g_t3calls; }
// ---- 测试 4：函数开头 push rbx ; sub rsp,40（MantleLoad 的样子），rsi 由调用方给 ----
int  t4_with_rsi(std::uint64_t rsi);
extern unsigned char t4_func[];
// --far 用的跳板区：.text 里一段 0（测试程序 .text 尾部空白的大小不固定，用它保证每次都一样）
extern unsigned char test_cave[];
}

asm(R"(
    .intel_syntax noprefix
    .text
    .globl t1, t1_site, t2, t2_site, t3, t3_site, t4_with_rsi, t4_func, test_cave
t1:                         # rcx=obj rdx=vtbl r8=io(double*)
    push rbx
    push rdi
    push rsi
    sub  rsp, 0x20
    mov  rbx, rcx
    mov  rax, rdx
    movsd xmm0, qword ptr [r8]
    mov  rdi, r8
    mov  rsi, 0x1122334455667788
t1_site:
    .byte 0x48,0x8B,0xCB, 0xFF,0x50,0x70      # mov rcx,rbx ; call [rax+0x70]
    .byte 0x8B,0x4B,0x58                      # mov ecx,[rbx+0x58]
    add  eax, ecx
    movsd qword ptr [rdi], xmm0
    mov  rcx, 0x1122334455667788
    cmp  rsi, rcx
    je   1f
    mov  eax, -999
1:
    add  rsp, 0x20
    pop  rsi
    pop  rdi
    pop  rbx
    ret

t2:
t2_site:
    .byte 0xF7,0x81,0x98,0x22,0x00,0x00,0x11,0x00,0x00,0x80   # test dword [rcx+2298],80000011
    .byte 0x48,0x89,0xC8                                      # mov rax,rcx（不动标志位）
    je   2f
    mov  eax, 1
    ret
2:
    xor  eax, eax
    ret

t3:
    sub  rsp, 0x28
t3_site:
    call t3_target
    mov  eax, 5
    add  rsp, 0x28
    ret

t4_with_rsi:                # rcx = 要放进 rsi 的值
    push rsi
    sub  rsp, 0x20
    mov  rsi, rcx
    mov  eax, -1
    call t4_func
    add  rsp, 0x20
    pop  rsi
    ret
t4_func:
    .byte 0x40,0x53, 0x48,0x83,0xEC,0x40     # push rbx ; sub rsp,0x40
    mov  eax, 7
    add  rsp, 0x40
    pop  rbx
    ret
    .p2align 4
test_cave:
    .fill 256, 1, 0
    .att_syntax prefix
)");

// ---- 测试 1 的道具 ----
struct Obj { unsigned char pad[0x58]; int v; };
static Obj objA{{}, 100}, objB{{}, 200};
static int VfA(void*) { return 1; }
static int VfB(void* self) { return self == &objB ? 20 : -50; }
static void* vtA[16];
// ReplaceModulePointers：数据段里的函数指针表，第 3 项当作「存原函数的表」不能换
static int RpA() { return 1; }
static int RpB() { return 2; }
static int RpC() { return 3; }
static int (*volatile g_rp[3])() = {RpA, RpB, RpA};
static void* vtB[16];
static void H1(hook::Ctx* c)
{
    // 换虚表、换对象 —— 和 CT 里的装备套装分支一样
    c->rax = reinterpret_cast<std::uint64_t>(vtB);
    c->rbx = reinterpret_cast<std::uint64_t>(&objB);
}

// ---- 测试 2：handler 直接改内存，test 的结果要跟着变 ----
static unsigned char obj2[0x3000];
static int h2Mode = 0;
static void H2(hook::Ctx* c)
{
    std::uint32_t* f = reinterpret_cast<std::uint32_t*>(c->rcx + 0x2298);
    if (h2Mode == 1) *f = 0;          // 改成 0 → test 得 ZF=1 → je → 返回 0
}

volatile std::int32_t g_flag = 0;

// 挂钩点的 jmp rel32 跳到哪
static std::uintptr_t JmpTarget(const unsigned char* site)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, site + 1, 4);
    return reinterpret_cast<std::uintptr_t>(site) + 5 + rel;
}

int main(int argc, char** argv)
{
    const bool farMode = argc > 1 && std::strcmp(argv[1], "--far") == 0;
    std::printf("== %s\n", farMode ? "arena 放远（经跳板）" : "arena 在附近");
    const std::uintptr_t mod = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    std::uintptr_t tb = 0; std::size_t tl = 0;
    CHECK(hook::TextRange(mod, tb, tl), "TextRange .text=%p+%zx", (void*)tb, tl);
    CHECK(hook::InitArena(tb, tb + tl, 0x4000, farMode), "arena=%p（模块=%p）", (void*)hook::ArenaBase(), (void*)mod);
    const std::uintptr_t caveLo = reinterpret_cast<std::uintptr_t>(test_cave), caveHi = caveLo + 256;
    if (farMode) {
        const std::int64_t d = (std::int64_t)hook::ArenaBase() - (std::int64_t)tb;
        CHECK(hook::ArenaFar() && (d > INT32_MAX || d < INT32_MIN), "arena 离 .text %+lld，rel32 够不着", (long long)d);
        const std::size_t tail = hook::CaveFree();
        CHECK(tail < 0x1000, ".text 尾部空白算出来 %zu 字节（不超过一页）", tail);
        hook::TestSetCave(caveLo, caveHi);
        CHECK(hook::CaveFree() == 256, "跳板区换成测试用的 256 字节");
        // 跳板从高往低分；第二格先被「别人」写过，应该跳过它
        const std::uint8_t used = 0x90;
        hook::WriteMem(caveLo + 224 + 3, &used, 1);
    } else {
        CHECK(!hook::ArenaFar(), "arena 在 rel32 范围内，不用跳板");
    }
    // 挂钩点跳到哪：放远时应是跳板区里的第 slot 格（从高往低数），否则直接进 arena
    auto lands = [&](const unsigned char* site, int slotFromTop) {
        const std::uintptr_t t = JmpTarget(site);
        if (farMode) return t == caveHi - 16 * (slotFromTop + 1);
        return t >= hook::ArenaBase() && t < hook::ArenaBase() + 0x4000;
    };

    // FindAll：每个挂钩点的字节在本模块里都只该找到一处
    CHECK(hook::FindAll(tb, tl, "48 8B CB FF 50 70 8B 4B 58").size() == 1, "FindAll 找到 t1 的特征码");
    CHECK(hook::FindAll(tb, tl, "F7 81 98 22 00 00 11 00 00 80 48 89 C8").size() == 1, "FindAll 找到 t2 的特征码");
    CHECK(hook::FindAll(tb, tl, "F7 81 ?? 22 00 00 11 ?? 00 80 48 89 C8").size() == 1, "FindAll 通配符");

    // ---- 1 ----
    for (int i = 0; i < 16; ++i) { vtA[i] = (void*)VfA; vtB[i] = (void*)VfB; }
    double io = 3.25;
    int r = t1(&objA, vtA, &io);
    CHECK(r == 101 && io == 3.25, "t1 挂钩前 = %d（期望 101）", r);
    static const std::uint8_t d1[] = {0x48, 0x8B, 0xCB, 0xFF, 0x50, 0x70};
    CHECK(hook::InstallCtxHook((std::uintptr_t)t1_site, 6, H1, d1, sizeof(d1)), "装上 t1 钩子");
    CHECK(t1_site[0] == 0xE9 && t1_site[5] == 0x90, "t1 现场被改成 jmp + nop");
    CHECK(lands(t1_site, 0), "t1 的 jmp 落在%s", farMode ? "跳板第 1 格" : " arena");
    io = 3.25;
    r = t1(&objA, vtA, &io);
    CHECK(r == 220, "t1 换虚表换对象后 = %d（期望 20 + 200）", r);
    CHECK(io == 3.25, "xmm0 跨钩子保持 = %g", io);

    // ---- 2 ----
    std::uint32_t* f2 = reinterpret_cast<std::uint32_t*>(obj2 + 0x2298);
    *f2 = 0x10;
    CHECK(t2(obj2) == 1, "t2 挂钩前：位被置上 → 1");
    static const std::uint8_t d2[] = {0xF7, 0x81, 0x98, 0x22, 0x00, 0x00, 0x11, 0x00, 0x00, 0x80};
    CHECK(hook::InstallCtxHook((std::uintptr_t)t2_site, 10, H2, d2, sizeof(d2)), "装上 t2 钩子");
    CHECK(lands(t2_site, 2), "t2 的 jmp 落在%s", farMode ? "跳板第 3 格（第 2 格被占了，跳过）" : " arena");
    h2Mode = 0; *f2 = 0x10;
    CHECK(t2(obj2) == 1, "t2 handler 不动内存 → 仍是 1（标志位来自原 test）");
    *f2 = 0;
    CHECK(t2(obj2) == 0, "t2 位为 0 → 0");
    h2Mode = 1; *f2 = 0x80000000;
    CHECK(t2(obj2) == 0 && *f2 == 0, "t2 handler 改内存后 test 跟着变 → 0");

    // ---- 3 ----
    g_t3calls = 0; g_flag = 0;
    CHECK(t3() == 5 && g_t3calls == 1, "t3 挂钩前正常调用");
    CHECK(hook::InstallCallGate((std::uintptr_t)t3_site, &g_flag, 2, 3), "装上 call 闸门（=2 时跳过，并改成 3）");
    CHECK(lands(t3_site, 3), "t3 的 jmp 落在%s", farMode ? "跳板第 4 格" : " arena");
    g_t3calls = 0; g_flag = 0;
    CHECK(t3() == 5 && g_t3calls == 1 && g_flag == 0, "flag=0 → 照常调用，flag 不变");
    g_flag = 2;
    CHECK(t3() == 5 && g_t3calls == 1 && g_flag == 3, "flag=2 → 跳过调用，flag 改成 3");
    g_flag = 3;
    CHECK(t3() == 5 && g_t3calls == 2 && g_flag == 3, "flag=3 → 照常调用");

    // ---- 4：MantleLoad 修复 —— rsi==0 直接 ret ----
    CHECK(t4_with_rsi(1) == 7 && t4_with_rsi(0) == 7, "t4 挂钩前两种 rsi 都执行函数体");
    hook::Emitter e;
    e.b({0x48, 0x85, 0xF6, 0x75, 0x01, 0xC3})            // test rsi,rsi ; jnz +1 ; ret
     .b({0x40, 0x53, 0x48, 0x83, 0xEC, 0x40})            // push rbx ; sub rsp,40
     .jmpAbs((std::uintptr_t)t4_func + 6);
    const std::uintptr_t s4 = hook::Commit(e);
    CHECK(s4 && hook::PatchJump((std::uintptr_t)t4_func, 6, s4), "装上 t4 钩子");
    CHECK(lands(t4_func, 4), "t4 的 jmp 落在%s", farMode ? "跳板第 5 格" : " arena");
    if (farMode) CHECK(hook::CaveFree() == 256 - 5 * 16, "跳板区还剩 %zu 字节", hook::CaveFree());
    CHECK(t4_with_rsi(1) == 7, "rsi!=0 → 函数照常执行 = 7");
    CHECK(t4_with_rsi(0) == -1, "rsi==0 → 立刻返回，eax 保持调用前的 -1");

    // ---- 5：按值替换数据段里的函数指针 ----
    std::string where;
    const int rn = hook::ReplaceModulePointers(mod, (void*)RpA, (void*)RpC, (const void*)&g_rp[2],
                                               (const void*)&g_rp[3], &where);
    CHECK(rn == 1, "ReplaceModulePointers 换了 %d 处（期望 1）：%s", rn, where.c_str());
    CHECK(g_rp[0]() == 3 && g_rp[1]() == 2 && g_rp[2]() == 1, "第 1 项换成 C，第 2 项不相干，第 3 项在跳过范围里没动");

    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
