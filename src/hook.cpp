#include "hook.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace hook {
namespace {

std::uintptr_t g_arena = 0;
std::size_t g_arenaSize = 0;
std::size_t g_arenaUsed = 0;
bool g_far = false;
// 跳板区：[g_caveLo, g_caveNext + 16) 还没用，从高往低分（别的工具找空白一般从低往高）
std::uintptr_t g_caveLo = 0, g_caveNext = 0;
std::mutex g_mx;

bool AllZero(std::uintptr_t a, std::size_t n)
{
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(a);
    for (std::size_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

// .text 的 VirtualSize 之后到页尾：映射成和代码一样可执行、内容是 0、镜像里没人用
void SetCaveAfter(std::uintptr_t hi)
{
    const std::uintptr_t lo = (hi + 16 + 15) & ~std::uintptr_t(15);   // 和最后一条指令隔开 16 字节
    const std::uintptr_t end = (hi + 0xFFF) & ~std::uintptr_t(0xFFF);
    MEMORY_BASIC_INFORMATION mbi{};
    const bool exec = ::VirtualQuery(reinterpret_cast<void*>(hi), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
                      (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    if (!exec || end < lo + 16) { g_caveLo = g_caveNext = 0; return; }
    g_caveLo = lo;
    g_caveNext = end - 16;
}

// 在跳板区放一条远跳，返回跳板地址（0 = 没地方了）。调用方持锁。
std::uintptr_t RelayLocked(std::uintptr_t to)
{
    for (; g_caveLo && g_caveNext >= g_caveLo; g_caveNext -= 16) {
        const std::uintptr_t a = g_caveNext;
        if (!AllZero(a, 16)) continue;   // 被别人用了：跳过
        std::uint8_t r[16] = {0xFF, 0x25, 0, 0, 0, 0};   // jmp [rip+0]
        std::memcpy(r + 6, &to, 8);
        r[14] = r[15] = 0xCC;
        DWORD old = 0;
        if (!::VirtualProtect(reinterpret_cast<void*>(a), 16, PAGE_EXECUTE_READWRITE, &old)) return 0;
        std::memcpy(reinterpret_cast<void*>(a), r, 16);
        DWORD tmp = 0;
        ::VirtualProtect(reinterpret_cast<void*>(a), 16, old, &tmp);
        ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(a), 16);
        g_caveNext -= 16;
        return a;
    }
    return 0;
}

bool InRel32(std::uintptr_t from, std::uintptr_t to)
{
    const std::int64_t d = (std::int64_t)to - (std::int64_t)from;
    return d >= INT32_MIN && d <= INT32_MAX;
}

std::uintptr_t TryAllocAt(std::uintptr_t addr, std::size_t size)
{
    void* p = ::VirtualAlloc(reinterpret_cast<void*>(addr), size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    return reinterpret_cast<std::uintptr_t>(p);
}

int HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool InitArena(std::uintptr_t lo, std::uintptr_t hi, std::size_t size, bool forceFar)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_arena) return true;
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;   // 64KB

    // 先往模块下面找（CT 用的就是 0x13FF80000），再往上面找。整段都要在 rel32 内。
    const std::uintptr_t span = 0x70000000;   // 留余量，别贴着 2GB 边
    for (std::uintptr_t a = (lo - gran) & ~(gran - 1); !forceFar && a > gran && lo - a < span; a -= gran) {
        if (!InRel32(a, hi) || !InRel32(a + size, lo)) continue;
        if (std::uintptr_t p = TryAllocAt(a, size)) { g_arena = p; break; }
    }
    if (!g_arena && !forceFar) {
        for (std::uintptr_t a = (hi + gran) & ~(gran - 1); a - hi < span; a += gran) {
            if (!InRel32(lo, a + size)) break;
            if (std::uintptr_t p = TryAllocAt(a, size)) { g_arena = p; break; }
        }
    }
    if (!g_arena) {
        // 附近全被占了（游戏起来后会一大片一大片地预留）：内存随便放，挂钩走 .text 尾部的跳板
        g_arena = reinterpret_cast<std::uintptr_t>(
            ::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_EXECUTE_READWRITE));
        if (!g_arena) return false;
        g_far = true;
        SetCaveAfter(hi);
    }
    g_arenaSize = size;
    g_arenaUsed = 0;
    std::memset(reinterpret_cast<void*>(g_arena), 0xCC, size);
    return true;
}

std::uintptr_t ArenaBase() { return g_arena; }
bool ArenaFar() { return g_far; }

std::size_t CaveFree()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return (g_caveLo && g_caveNext >= g_caveLo) ? g_caveNext + 16 - g_caveLo : 0;
}

void TestSetCave(std::uintptr_t lo, std::uintptr_t hi)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_caveLo = (lo + 15) & ~std::uintptr_t(15);
    g_caveNext = (hi & ~std::uintptr_t(15)) - 16;
}

std::uintptr_t Commit(const Emitter& e)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_arena) return 0;
    const std::size_t at = (g_arenaUsed + 15) & ~std::size_t(15);
    if (at + e.size() > g_arenaSize) return 0;
    std::memcpy(reinterpret_cast<void*>(g_arena + at), e.bytes().data(), e.size());
    g_arenaUsed = at + e.size();
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(g_arena + at), e.size());
    return g_arena + at;
}

bool WriteMem(std::uintptr_t addr, const void* bytes, std::size_t n)
{
    DWORD old = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(addr), bytes, n);
    DWORD tmp = 0;
    ::VirtualProtect(reinterpret_cast<void*>(addr), n, old, &tmp);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
    return true;
}

bool PatchJump(std::uintptr_t site, std::size_t len, std::uintptr_t stub)
{
    if (len < 5 || len > 16 || !stub) return false;
    if (!InRel32(site + 5, stub)) {   // arena 太远：经 .text 尾部的跳板过去
        std::lock_guard<std::mutex> lk(g_mx);
        stub = RelayLocked(stub);
        if (!stub || !InRel32(site + 5, stub)) return false;
    }
    std::uint8_t p[16];
    p[0] = 0xE9;
    const std::int32_t rel = (std::int32_t)((std::int64_t)stub - (std::int64_t)(site + 5));
    std::memcpy(p + 1, &rel, 4);
    for (std::size_t i = 5; i < len; ++i) p[i] = 0x90;
    return WriteMem(site, p, len);
}

bool InstallCtxHook(std::uintptr_t site, std::size_t len, Handler h,
                    const std::uint8_t* displaced, std::size_t dlen)
{
    Emitter e(256);
    // ---- 保存现场 ----
    e.b({0x9C});                                        // pushfq
    e.b({0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57});    // push rax rcx rdx rbx rbp rsi rdi
    for (std::uint8_t r = 0; r < 8; ++r) e.b({0x41, std::uint8_t(0x50 + r)});   // push r8..r15
    e.b({0x48, 0x89, 0xE5});                            // mov rbp, rsp      (rbp = &Ctx)
    e.b({0x48, 0x83, 0xEC, 0x60});                      // sub rsp, 0x60
    e.b({0x48, 0x83, 0xE4, 0xF0});                      // and rsp, -16
    for (std::uint8_t n = 0; n < 6; ++n)                // movdqa [rsp+n*16], xmmN
        e.b({0x66, 0x0F, 0x7F, std::uint8_t(0x44 | (n << 3)), 0x24, std::uint8_t(n * 16)});
    e.b({0x48, 0x83, 0xEC, 0x20});                      // sub rsp, 0x20     (shadow space)
    e.b({0x48, 0x89, 0xE9});                            // mov rcx, rbp
    e.b({0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(h));   // mov rax, handler
    e.b({0xFF, 0xD0});                                  // call rax
    e.b({0x48, 0x83, 0xC4, 0x20});                      // add rsp, 0x20
    for (std::uint8_t n = 0; n < 6; ++n)                // movdqa xmmN, [rsp+n*16]
        e.b({0x66, 0x0F, 0x6F, std::uint8_t(0x44 | (n << 3)), 0x24, std::uint8_t(n * 16)});
    e.b({0x48, 0x89, 0xEC});                            // mov rsp, rbp
    for (int r = 7; r >= 0; --r) e.b({0x41, std::uint8_t(0x58 + r)});   // pop r15..r8
    e.b({0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58});    // pop rdi rsi rbp rbx rdx rcx rax
    e.b({0x9D});                                        // popfq
    // ---- 被覆盖的原指令，然后回去 ----
    e.raw(displaced, dlen);
    e.jmpAbs(site + len);

    const std::uintptr_t stub = Commit(e);
    return stub && PatchJump(site, len, stub);
}

bool InstallCallGate(std::uintptr_t site, volatile std::int32_t* flag,
                     std::int32_t skipWhen, std::int32_t setOnSkip)
{
    const std::uint8_t* s = reinterpret_cast<const std::uint8_t*>(site);
    if (s[0] != 0xE8) return false;
    std::int32_t rel = 0;
    std::memcpy(&rel, s + 1, 4);
    const std::uintptr_t target = site + 5 + (std::intptr_t)rel;
    if (skipWhen < -128 || skipWhen > 127) return false;

    Emitter e(64);
    e.b({0x50});                                                         // push rax
    e.b({0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(flag));        // mov rax, flag
    e.b({0x83, 0x38, std::uint8_t(skipWhen)});                           // cmp dword [rax], skipWhen
    if (setOnSkip >= 0) {
        e.b({0x75, 0x06});                                               // jne +6
        e.b({0xC7, 0x00}).u32((std::uint32_t)setOnSkip);                 // mov dword [rax], setOnSkip
    }                                                                    // （mov 不动标志位）
    e.b({0x58});                                                         // pop rax
    e.b({0x74, 0x10});                                                   // je skip（跳过下面 16 字节）
    e.callAbs(target);                                                   // call 原目标
    e.jmpAbs(site + 5);                                                  // skip: 回去
    const std::uintptr_t stub = Commit(e);
    return stub && PatchJump(site, 5, stub);
}

std::vector<std::uintptr_t> FindAll(std::uintptr_t begin, std::size_t len, const char* pattern)
{
    std::vector<int> pat;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { pat.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        const int hi = HexNibble(p[0]), lo = HexNibble(p[1]);
        if (hi < 0 || lo < 0) return {};
        pat.push_back(hi * 16 + lo);
        p += 2;
    }
    std::vector<std::uintptr_t> hits;
    if (pat.empty() || pat[0] < 0 || len < pat.size()) return hits;
    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(begin);
    const std::uint8_t* end = base + len - pat.size() + 1;
    for (const std::uint8_t* q = base; q < end;) {
        q = static_cast<const std::uint8_t*>(std::memchr(q, pat[0], end - q));
        if (!q) break;
        bool ok = true;
        for (std::size_t i = 1; i < pat.size(); ++i)
            if (pat[i] >= 0 && q[i] != pat[i]) { ok = false; break; }
        if (ok) hits.push_back(reinterpret_cast<std::uintptr_t>(q));
        ++q;
    }
    return hits;
}

int ReplaceModulePointers(std::uintptr_t base, void* oldPtr, void* newPtr, const void* skipLo, const void* skipHi,
                          std::string* where)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (WORD k = 0; k < nt->FileHeader.NumberOfSections; ++k) {
        if (sec[k].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        const std::uintptr_t a = base + sec[k].VirtualAddress;
        const std::size_t len = sec[k].Misc.VirtualSize & ~std::size_t(7);
        for (std::size_t o = 0; o < len; o += 8) {
            void** slot = reinterpret_cast<void**>(a + o);
            if (*slot != oldPtr) continue;
            if (skipLo && reinterpret_cast<const void*>(slot) >= skipLo && reinterpret_cast<const void*>(slot) < skipHi)
                continue;
            WriteMem(reinterpret_cast<std::uintptr_t>(slot), &newPtr, sizeof(newPtr));
            if (where) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.8s+%zx ", reinterpret_cast<const char*>(sec[k].Name), o);
                *where += buf;
            }
            ++n;
        }
    }
    return n;
}

bool TextRange(std::uintptr_t moduleBase, std::uintptr_t& begin, std::size_t& len)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            begin = moduleBase + sec[i].VirtualAddress;
            len = sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

} // namespace hook
