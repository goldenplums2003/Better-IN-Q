#pragma once
// ============================================================================
//  hook.h —— 往游戏代码中间插一刀
//
//  CT 版的做法是在游戏代码里写一条 jmp，跳到 CE 在模块旁边分配的内存里执行
//  一段汇编。这里一样：在游戏模块 ±2GB 内要一块可执行内存（rel32 跳得到），
//  里面放「保存寄存器 → 调 C++ → 恢复寄存器 → 执行被覆盖的原指令 → 跳回」
//  的小段机器码。逻辑都写在 C++ 里，寄存器通过 Ctx 读写。
// ============================================================================
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace hook {

// 和 stub 里的压栈顺序一一对应（低地址在前）。handler 改了哪个，恢复时就是哪个。
struct Ctx {
    std::uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    std::uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    std::uint64_t rflags;
};
using Handler = void (*)(Ctx*);

// 在 [lo, hi)（模块的 .text）±2GB 内要一块可执行内存。只需调一次，越早越好：
// 游戏起来后会把模块附近的地址一大片一大片地占掉。
// 附近实在要不到时，内存随便放，改用跳板：.text 尾部（hi 到页尾）那段没用的空白里
// 放 14 字节的「jmp [rip+0] ; 目标」，挂钩点先 rel32 跳到跳板，跳板再远跳进 arena。
// forceFar 只给测试用。
bool InitArena(std::uintptr_t lo, std::uintptr_t hi, std::size_t size = 0x4000, bool forceFar = false);
std::uintptr_t ArenaBase();
bool ArenaFar();                       // arena 不在 rel32 范围内（挂钩要走跳板）
std::size_t CaveFree();                // 跳板区还剩多少字节
void TestSetCave(std::uintptr_t lo, std::uintptr_t hi);   // 测试用：指定跳板区

// 往 arena 里写机器码的小工具
class Emitter {
public:
    explicit Emitter(std::size_t reserve = 256) { buf_.reserve(reserve); }
    Emitter& b(std::initializer_list<std::uint8_t> bytes) { buf_.insert(buf_.end(), bytes); return *this; }
    Emitter& raw(const std::uint8_t* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); return *this; }
    Emitter& u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) buf_.push_back((v >> (8 * i)) & 0xFF); return *this; }
    Emitter& u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) buf_.push_back((v >> (8 * i)) & 0xFF); return *this; }
    Emitter& jmpAbs(std::uint64_t to) { return b({0xFF, 0x25, 0, 0, 0, 0}).u64(to); }   // jmp [rip+0]
    // call [rip+2] ; jmp +8 ; dq target —— 不依赖距离的绝对 call
    Emitter& callAbs(std::uint64_t to) { return b({0xFF, 0x15, 2, 0, 0, 0, 0xEB, 8}).u64(to); }
    const std::vector<std::uint8_t>& bytes() const { return buf_; }
    std::size_t size() const { return buf_.size(); }
private:
    std::vector<std::uint8_t> buf_;
};

// 把 Emitter 的内容放进 arena，返回地址（0 = 放不下）
std::uintptr_t Commit(const Emitter& e);

// site 处 len(>=5) 字节改成 jmp stub + nop 填满。stub 必须在 rel32 范围内。
bool PatchJump(std::uintptr_t site, std::size_t len, std::uintptr_t stub);

// 写代码段/数据段（临时改页保护，写完刷指令缓存）
bool WriteMem(std::uintptr_t addr, const void* bytes, std::size_t n);

// 上下文钩子：site 处 len 字节换成跳转。stub 保存全部通用寄存器、rflags 和 xmm0-5，
// 调 handler(Ctx*)，恢复后执行 displaced（被覆盖的原指令，原样复制，不得含 RIP 相对寻址），
// 再跳回 site+len。
bool InstallCtxHook(std::uintptr_t site, std::size_t len, Handler h,
                    const std::uint8_t* displaced, std::size_t dlen);

// 「call 闸门」：site 是一条 5 字节 call rel32。*flag == skipWhen 时跳过这次调用
// （setOnSkip >= 0 时顺手把 *flag 改成它），否则照常调用原目标。
bool InstallCallGate(std::uintptr_t site, volatile std::int32_t* flag,
                     std::int32_t skipWhen, std::int32_t setOnSkip);

// 特征码查找，pattern 形如 "48 8B CB ?? 50"
std::vector<std::uintptr_t> FindAll(std::uintptr_t begin, std::size_t len, const char* pattern);

// 当前模块的 .text 范围
bool TextRange(std::uintptr_t moduleBase, std::uintptr_t& begin, std::size_t& len);

// 在模块的数据段（不可执行的节：导入表、全局变量）里找所有等于 oldPtr 的 8 字节对齐指针，
// 换成 newPtr。用来拦游戏调用的系统函数：exe 有 Steam 壳，导入表要运行时才解开，只能按值找。
// [skipLo, skipHi) 里的不换（存原函数指针的表本身）。返回换了几处，where 里追加「节名+偏移 」。
int ReplaceModulePointers(std::uintptr_t moduleBase, void* oldPtr, void* newPtr,
                          const void* skipLo = nullptr, const void* skipHi = nullptr,
                          std::string* where = nullptr);

} // namespace hook
