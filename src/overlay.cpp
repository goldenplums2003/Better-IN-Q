// MinGW 的 d3d12.h 默认按 GCC 的规矩返回结构体（GetDesc、GetCPUDescriptorHandleForHeapStart 之类），
// 和 COM 实际的调用约定（隐藏的返回指针）对不上，会读到错的值。打开这个开关就按 COM 的来。
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include "overlay.h"

#include "log.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <imm.h>
#include <msctf.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <cfloat>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {
namespace {

typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(WINAPI* Resize1Fn)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
typedef void(WINAPI* ExecuteFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

enum Api { kNone, kD3D11, kD3D12 };

Callbacks g_cb;
PresentFn g_present = nullptr;
ResizeFn g_resize = nullptr;
Resize1Fn g_resize1 = nullptr;
std::atomic<bool> g_open{false};
bool g_wasOpen = false;
std::atomic<bool> g_inited{false};          // 主循环的看门狗也要读
// ImGui 的输入队列：WndProc 在窗口线程里往里塞，渲染线程里 NewFrame 消费。两边都拿这把锁。
// 可重入：同一个线程里 ImGui 设输入法候选框位置时，系统可能同步发消息回 WndProc
std::recursive_mutex g_imguiMx;
Api g_api = kNone;
std::atomic<bool> g_failed{false};
std::atomic<unsigned long long> g_openAt{0};   // 这次打开面板的时间（还没初始化成功时，用来判断要不要放弃）
IDXGISwapChain* g_sc = nullptr;                // 游戏的交换链（只比指针，不持有引用）
IDXGISwapChain* g_foreign = nullptr;           // 上一个确认过不是游戏的交换链，免得每帧再查
HWND g_hwnd = nullptr;
WNDPROC g_wndProc = nullptr;
void (*g_debugAfterDraw)(void*) = nullptr;
void (*g_debugAfterDraw12)(void*, void*) = nullptr;
ImU32 g_lastScaledEvent = 0;
std::atomic<unsigned long long> g_lastDrawAt{0};   // 面板 / 特效上一次真的画出一帧的时间（看门狗用）

// ---- D3D11
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

// ---- D3D12
// 交换链拿不到它的命令队列，只能挂 ExecuteCommandLists，等游戏自己提交时把队列记下来
ExecuteFn g_execute = nullptr;
std::atomic<ID3D12Device*> g_wantQueueOf{nullptr};   // 要找哪个设备的队列
std::atomic<ID3D12CommandQueue*> g_queue{nullptr};
ID3D12Device* g_dev12 = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
UINT g_rtvInc = 0, g_srvInc = 0;
std::vector<int> g_srvFree;                    // ImGui 要的纹理描述符，从 g_srvHeap 里分
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceVal = 0;
HANDLE g_fenceEvent = nullptr;
constexpr UINT kMaxBuffers = 16;
ID3D12CommandAllocator* g_alloc[kMaxBuffers] = {};
UINT64 g_allocFence[kMaxBuffers] = {};
DXGI_FORMAT g_bbFormat = DXGI_FORMAT_UNKNOWN;

// 面板开着时输入法归我们管，关面板再原样还给游戏：
//  · 平时把输入法从游戏窗口上摘掉。搜任务打的是拼音字母，中文输入法开着会把字母吃进候选框
//    （全屏时候选框还看不见），面板就收不到字。
//  · 正在打名字（猫饭改名）时挂上，能打中文。游戏自己把输入法关着的话挂系统默认的那个。
// 游戏自己也会挂输入法（聊天框），所以该摘着的时候每次按键前都再摘一次，记下游戏最新挂的那个。
// Imm* 要在窗口所在的线程里调，所以放在 WndProc 里做（SetOpen / SetImeWanted 会补发 WM_NULL 触发）。
std::atomic<bool> g_imeWanted{false};
HIMC g_gameImc = nullptr;   // 游戏窗口原来挂的输入法（空 = 游戏把输入法关着）
bool g_imeOwned = false;    // 面板开着、输入法归我们管
int g_imeOn = -1;           // 我们现在设成了：0 摘掉 1 挂上

// ---- 打名字时：拼音和候选词由面板自己画（和游戏的聊天框一样）----
// 输入法自己的候选窗口在游戏里出不来（独占全屏盖不上来；游戏进程的窗口对象还常常用光，窗口建不出来）。
// 游戏聊天框的候选是游戏向输入法要了词自己画的，面板也这么做：
//  · 正在打的拼音：WM_IME_COMPOSITION 时用 ImmGetCompositionString 取
//  · 候选词：TSF 的输入法（微软拼音、新版搜狗）接一个 ITfUIElementSink，从 ITfCandidateListUIElement 取当前页；
//            IMM32 的输入法在 IMN_OPENCANDIDATE / IMN_CHANGECANDIDATE 时用 ImmGetCandidateList 取
//  · 输入法自己的界面叫它别出（WM_IME_SETCONTEXT 的显示标志清掉、BeginUIElement 里回「不用你显示」），
//    免得窗口模式下出两份
// 选词、翻页还是输入法自己处理按键，这里只管拿来显示。都在窗口线程里取；面板在渲染线程里用 GetImeText 拿一份拷贝
//  · 位置：ImGui 给的输入框位置换算成窗口坐标，在窗口线程里设（Imm* 跨线程调不动，ImGui 自己是在渲染线程里设的）
std::atomic<int> g_imeX{0}, g_imeY{0}, g_imeH{0};   // 输入框光标处（窗口客户区坐标）和行高
std::atomic<bool> g_imePosDirty{false};

// 诊断：打一次名字记一行，下次有人说「打不了中文」时看是哪一步没到
struct ImeDiag {
    ULONGLONG since = 0;
    int start = 0, result = 0, imeChar = 0, chr = 0, openCand = 0;
    int comp = 0, candTsf = 0, candImm = 0, candMax = 0;   // 拼音取了几次；候选从 TSF / IMM32 取到了几次，最多一页几个
    bool tsf = false, sink = false;                        // 这个线程有没有 TSF；候选的接收器接上了没有
} g_imeDiag;

constexpr UINT kMaxCands = 10;   // 一页最多画这么多个

struct ImeView {
    std::wstring comp;                 // 正在打的拼音
    std::vector<std::wstring> cands;   // 当前页的候选词
    int sel = -1;                      // 当前页里选中的是第几个
    bool fromTsf = false;              // 候选是 TSF 给的（给了就不再用 IMM32 的那份）
};
std::mutex g_imeViewMx;   // 窗口线程写、渲染线程读（g_imeRaw 也归它管）
ImeView g_imeView;
std::string g_imeRaw;     // 诊断：输入法最近一次给候选表时的原始数

void ImeClear(bool compToo)
{
    std::lock_guard<std::mutex> lk(g_imeViewMx);
    if (compToo) g_imeView.comp.clear();
    g_imeView.cands.clear();
    g_imeView.sel = -1;
    g_imeView.fromTsf = false;
}

void ImeReadComposition(HWND h)
{
    std::wstring s;
    if (HIMC c = ::ImmGetContext(h)) {
        const LONG n = ::ImmGetCompositionStringW(c, GCS_COMPSTR, nullptr, 0);   // 字节数
        if (n > 0) {
            s.resize((std::size_t)n / sizeof(wchar_t));
            ::ImmGetCompositionStringW(c, GCS_COMPSTR, &s[0], (DWORD)n);
        }
        ::ImmReleaseContext(h, c);
    }
    ++g_imeDiag.comp;
    std::lock_guard<std::mutex> lk(g_imeViewMx);
    g_imeView.comp = s;
}

// IMM32 的输入法：候选表里取当前页
void ImeReadCandidatesImm(HWND h)
{
    {
        std::lock_guard<std::mutex> lk(g_imeViewMx);
        if (g_imeView.fromTsf) return;
    }
    std::vector<std::wstring> list;
    int sel = -1;
    if (HIMC c = ::ImmGetContext(h)) {
        const DWORD size = ::ImmGetCandidateListW(c, 0, nullptr, 0);
        if (size >= sizeof(CANDIDATELIST)) {
            std::vector<BYTE> buf(size);
            CANDIDATELIST* cl = reinterpret_cast<CANDIDATELIST*>(buf.data());
            if (::ImmGetCandidateListW(c, 0, cl, size) && cl->dwCount &&
                offsetof(CANDIDATELIST, dwOffset) + (std::size_t)cl->dwCount * sizeof(DWORD) <= size) {
                const DWORD start = cl->dwPageStart < cl->dwCount ? cl->dwPageStart : 0;
                DWORD page = cl->dwPageSize ? cl->dwPageSize : 9;
                if (page > kMaxCands) page = kMaxCands;
                for (DWORD i = start; i < cl->dwCount && i < start + page; ++i) {
                    const DWORD off = cl->dwOffset[i];
                    if (off >= size) break;
                    const wchar_t* p = reinterpret_cast<const wchar_t*>(buf.data() + off);
                    const std::size_t maxc = (size - off) / sizeof(wchar_t);
                    std::size_t len = 0;
                    while (len < maxc && p[len]) ++len;
                    list.emplace_back(p, len);
                }
                sel = (int)cl->dwSelection - (int)start;
                char raw[128];
                std::snprintf(raw, sizeof(raw), "IMM32 共 %lu 个，这页从 %lu 起、每页 %lu 个，选中 %lu", (unsigned long)cl->dwCount,
                              (unsigned long)cl->dwPageStart, (unsigned long)cl->dwPageSize, (unsigned long)cl->dwSelection);
                std::lock_guard<std::mutex> lk(g_imeViewMx);
                g_imeRaw = raw;
            }
        }
        ::ImmReleaseContext(h, c);
    }
    if (!list.empty()) {
        ++g_imeDiag.candImm;
        g_imeDiag.candMax = std::max(g_imeDiag.candMax, (int)list.size());
    }
    std::lock_guard<std::mutex> lk(g_imeViewMx);
    if (g_imeView.fromTsf) return;
    g_imeView.cands = std::move(list);
    g_imeView.sel = sel;
}

// TSF 的候选表接口 ITfCandidateListUIElement。MinGW 的 msctf.h 里没有。
// 不把它声明成 C++ 的纯虚类再调：声明在这个文件里、又没有任何实现，GCC -O2 会认定「这些虚函数没人实现」，
// 把调用直接优化成 call 0（游戏里一打字就崩，就是这个）。所以按 Windows SDK 里的顺序直接走虚表：
//   0~2 IUnknown；3~6 ITfUIElement（GetDescription / GetGUID / Show / IsShown）；
//   7 GetUpdatedFlags  8 GetDocumentMgr  9 GetCount  10 GetSelection  11 GetString  12 GetPageIndex
//   13 SetPageIndex  14 GetCurrentPage
const GUID kIidCandidateListUIElement = {0xea1ea138, 0x19df, 0x11d7, {0xa6, 0xd2, 0x00, 0x06, 0x5b, 0x84, 0x43, 0x5c}};
template <class Fn> Fn VSlot(void* obj, int index)
{
    void** const vt = *reinterpret_cast<void***>(obj);
    return reinterpret_cast<Fn>(vt[index]);
}
HRESULT ClGetUint(void* cl, int slot, UINT* out)   // GetCount / GetSelection / GetCurrentPage
{
    return VSlot<HRESULT(STDMETHODCALLTYPE*)(void*, UINT*)>(cl, slot)(cl, out);
}
HRESULT ClGetString(void* cl, UINT index, BSTR* out)
{
    return VSlot<HRESULT(STDMETHODCALLTYPE*)(void*, UINT, BSTR*)>(cl, 11)(cl, index, out);
}
HRESULT ClGetPageIndex(void* cl, UINT* index, UINT size, UINT* pageCount)
{
    return VSlot<HRESULT(STDMETHODCALLTYPE*)(void*, UINT*, UINT, UINT*)>(cl, 12)(cl, index, size, pageCount);
}

void FreeBstr(BSTR s)   // SysFreeString，不为它多链一个库
{
    typedef void(WINAPI * Fn)(BSTR);
    static const Fn fn = reinterpret_cast<Fn>(::GetProcAddress(::LoadLibraryW(L"oleaut32.dll"), "SysFreeString"));
    if (fn && s) fn(s);
}

// 下面这几个只在窗口线程里用
ITfUIElementMgr* g_tsfMgr = nullptr;
ITfSource* g_tsfSource = nullptr;
DWORD g_tsfCookie = TF_INVALID_COOKIE;
DWORD g_tsfCandId = TF_INVALID_COOKIE;   // 候选表那个界面元素的编号

// el：输入法的一个界面元素。是候选表的话取出当前页，返回 true
bool ReadCandidateElement(IUnknown* el)
{
    void* cl = nullptr;
    if (FAILED(el->QueryInterface(kIidCandidateListUIElement, &cl)) || !cl) return false;
    UINT count = 0, selAbs = 0, page = 0, pages = 0;
    ClGetUint(cl, 9, &count);
    ClGetUint(cl, 10, &selAbs);
    ClGetUint(cl, 14, &page);
    UINT start = 0, end = count;
    if (SUCCEEDED(ClGetPageIndex(cl, nullptr, 0, &pages)) && pages > 0 && pages < 4096) {
        std::vector<UINT> idx(pages);
        UINT got = pages;
        if (SUCCEEDED(ClGetPageIndex(cl, idx.data(), pages, &got)) && page < pages) {
            start = idx[page];
            if (page + 1 < pages) end = std::min(count, idx[page + 1]);
        }
    }
    if (start > count) start = count;
    if (end < start) end = start;
    if (end - start > kMaxCands) end = start + kMaxCands;
    std::vector<std::wstring> list;
    for (UINT i = start; i < end; ++i) {
        BSTR s = nullptr;
        if (SUCCEEDED(ClGetString(cl, i, &s)) && s) {
            list.emplace_back(s);
            FreeBstr(s);
        } else {
            list.emplace_back();
        }
    }
    static_cast<IUnknown*>(cl)->Release();
    if (!list.empty()) {
        ++g_imeDiag.candTsf;
        g_imeDiag.candMax = std::max(g_imeDiag.candMax, (int)list.size());
    }
    char raw[128];
    std::snprintf(raw, sizeof(raw), "TSF 共 %u 个，第 %u 页 / 共 %u 页，这页 %u~%u，选中 %u", count, page, pages, start, end, selAbs);
    std::lock_guard<std::mutex> lk(g_imeViewMx);
    g_imeRaw = raw;
    g_imeView.fromTsf = !list.empty();
    g_imeView.cands = std::move(list);
    g_imeView.sel = (int)selAbs - (int)start;
    return true;
}

void TsfReadCandidates(DWORD id)
{
    if (!g_tsfMgr) return;
    ITfUIElement* el = nullptr;
    if (FAILED(g_tsfMgr->GetUIElement(id, &el)) || !el) return;
    if (ReadCandidateElement(el)) g_tsfCandId = id;
    el->Release();
}

// 输入法要出界面（候选表、读音）时会先问一声：回「不用你显示」，把候选表拿来自己画
struct UiSink final : public ITfUIElementSink {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualIID(r, __uuidof(IUnknown)) || IsEqualIID(r, __uuidof(ITfUIElementSink))) {
            *out = static_cast<ITfUIElementSink*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }    // 静态对象，不靠引用计数
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE BeginUIElement(DWORD id, WINBOOL* show) override
    {
        if (show) *show = FALSE;
        TsfReadCandidates(id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD id) override
    {
        TsfReadCandidates(id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD id) override
    {
        if (id == g_tsfCandId) {
            g_tsfCandId = TF_INVALID_COOKIE;
            ImeClear(false);
        }
        return S_OK;
    }
} g_uiSink;

// 这个线程开了 TSF 的话（游戏自己开的），把接收器接上。只用线程已有的，不新建
void TsfAdvise()
{
    if (g_tsfSource) return;
    typedef HRESULT(WINAPI * GetThreadMgrFn)(ITfThreadMgr**);
    static const GetThreadMgrFn getTim =
        reinterpret_cast<GetThreadMgrFn>(::GetProcAddress(::LoadLibraryW(L"msctf.dll"), "TF_GetThreadMgr"));
    if (!getTim) return;
    ITfThreadMgr* tim = nullptr;
    if (FAILED(getTim(&tim)) || !tim) return;
    g_imeDiag.tsf = true;
    bool ok = false;
    if (SUCCEEDED(tim->QueryInterface(__uuidof(ITfUIElementMgr), reinterpret_cast<void**>(&g_tsfMgr))) && g_tsfMgr &&
        SUCCEEDED(g_tsfMgr->QueryInterface(__uuidof(ITfSource), reinterpret_cast<void**>(&g_tsfSource))) && g_tsfSource)
        ok = SUCCEEDED(g_tsfSource->AdviseSink(__uuidof(ITfUIElementSink), &g_uiSink, &g_tsfCookie));
    if (!ok) {
        if (g_tsfSource) { g_tsfSource->Release(); g_tsfSource = nullptr; }
        if (g_tsfMgr) { g_tsfMgr->Release(); g_tsfMgr = nullptr; }
        g_tsfCookie = TF_INVALID_COOKIE;
    }
    g_imeDiag.sink = ok;
    tim->Release();
}

void TsfUnadvise()
{
    if (g_tsfSource) {
        if (g_tsfCookie != TF_INVALID_COOKIE) g_tsfSource->UnadviseSink(g_tsfCookie);
        g_tsfSource->Release();
        g_tsfSource = nullptr;
    }
    if (g_tsfMgr) { g_tsfMgr->Release(); g_tsfMgr = nullptr; }
    g_tsfCookie = TF_INVALID_COOKIE;
    g_tsfCandId = TF_INVALID_COOKIE;
}

void PlaceImeWindows(HWND h)
{
    HIMC c = ::ImmGetContext(h);
    if (!c) return;
    const int x = g_imeX.load(), y = g_imeY.load(), lh = g_imeH.load() > 0 ? g_imeH.load() : 20;
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_FORCE_POSITION;
    cf.ptCurrentPos = {x, y};
    ::ImmSetCompositionWindow(c, &cf);
    CANDIDATEFORM cand{};
    cand.dwIndex = 0;
    cand.dwStyle = CFS_EXCLUDE;   // 候选框摆在输入行旁边，别挡住正在打的字
    cand.ptCurrentPos = {x, y + lh};
    cand.rcArea = {x, y, x + 1, y + lh};
    ::ImmSetCandidateWindow(c, &cand);
    ::ImmReleaseContext(h, c);
}

} // namespace
std::string DebugImeRaw()
{
    std::lock_guard<std::mutex> lk(g_imeViewMx);
    return g_imeRaw.empty() ? "没给过" : g_imeRaw;
}
namespace {

void LogImeDiag()
{
    const HKL hkl = ::GetKeyboardLayout(0);
    wchar_t file[MAX_PATH] = {};
    ::ImmGetIMEFileNameW(hkl, file, MAX_PATH);
    char fileA[MAX_PATH * 3] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, file, -1, fileA, sizeof(fileA), nullptr, nullptr);
    const ImeDiag& d = g_imeDiag;
    Log("ime: 打名字 %llu 秒：开始组字 %d 次、上屏 %d 次、WM_IME_CHAR %d、WM_CHAR %d、候选框打开 %d 次；"
        "取拼音 %d 次，取到候选 TSF %d 次 / IMM32 %d 次（一页最多 %d 个；最近一次：%s）；键盘布局 %08llX（%s），TSF %s",
        (unsigned long long)((::GetTickCount64() - d.since) / 1000), d.start, d.result, d.imeChar, d.chr, d.openCand,
        d.comp, d.candTsf, d.candImm, d.candMax, DebugImeRaw().c_str(), (unsigned long long)(ULONG_PTR)hkl,
        fileA[0] ? fileA : "IMM32 没有输入法文件（多半是 TSF 输入法）",
        d.tsf ? (d.sink ? "有，候选接收器接上了" : "有，候选接收器没接上") : "没有");
}

// 打到一半的拼音、开着的候选框：交还输入法之前清掉，不然会留到游戏的聊天框里
void CancelComposition(HWND h)
{
    if (HIMC c = ::ImmGetContext(h)) {
        ::ImmNotifyIME(c, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ::ImmNotifyIME(c, NI_CLOSECANDIDATE, 0, 0);
        ::ImmReleaseContext(h, c);
    }
}

bool g_syncing = false;   // Imm* 会同步发 WM_IME_SETCONTEXT 回来，又进到这里：里面那一层什么都别做

void SyncIme(HWND h, UINT msg, bool open)
{
    if (g_syncing) return;
    g_syncing = true;
    struct Done { ~Done() { g_syncing = false; } } done;
    if (!open) {
        if (g_imeOwned) {
            if (g_imeOn == 1) {
                CancelComposition(h);
                TsfUnadvise();
                ImeClear(true);
                LogImeDiag();
                // 先摘掉再还：还的就是现在挂着的这个时，不换一下游戏收不到新的 WM_IME_SETCONTEXT，
                // 我们改过的显示标志就会一直留着
                ::ImmAssociateContext(h, nullptr);
            }
            ::ImmAssociateContext(h, g_gameImc);   // 原样还回去（原来是空的就还是关着）
        }
        g_gameImc = nullptr;
        g_imeOwned = false;
        g_imeOn = -1;
        return;
    }
    if (!g_imeOwned) {
        g_gameImc = ::ImmAssociateContext(h, nullptr);
        g_imeOwned = true;
        g_imeOn = 0;
    }
    const int want = g_imeWanted.load() ? 1 : 0;
    if (want != g_imeOn) {
        if (want) {
            if (g_gameImc) ::ImmAssociateContext(h, g_gameImc);
            else ::ImmAssociateContextEx(h, nullptr, IACE_DEFAULT);
            // 输入法自己的界面都别出（显示标志全清掉）：拼音和候选由面板画
            ::DefWindowProcW(h, WM_IME_SETCONTEXT, TRUE, 0);
            g_imeDiag = ImeDiag();
            g_imeDiag.since = ::GetTickCount64();
            g_imePosDirty = true;
            ImeClear(true);
            TsfAdvise();
        } else {
            CancelComposition(h);
            TsfUnadvise();
            ImeClear(true);
            ::ImmAssociateContext(h, nullptr);
            LogImeDiag();
        }
        g_imeOn = want;
    } else if (!want && (msg == WM_KEYDOWN || msg == WM_INPUTLANGCHANGE || msg == WM_IME_STARTCOMPOSITION)) {
        if (HIMC prev = ::ImmAssociateContext(h, nullptr)) g_gameImc = prev;   // 游戏刚又挂上的
    }
    if (g_imeOn != 1) return;
    if (g_imePosDirty.exchange(false)) PlaceImeWindows(h);
    switch (msg) {   // 诊断计数
    case WM_IME_STARTCOMPOSITION: ++g_imeDiag.start; break;
    case WM_IME_CHAR: ++g_imeDiag.imeChar; break;
    case WM_CHAR: ++g_imeDiag.chr; break;
    default: break;
    }
}

bool IsImeMessage(UINT msg)
{
    return (msg >= WM_IME_STARTCOMPOSITION && msg <= WM_IME_KEYLAST) || (msg >= WM_IME_SETCONTEXT && msg <= WM_IME_KEYUP);
}

// 这条原始输入是不是「松开」（键盘抬起，或者只有鼠标键抬起、没有按下和移动）
bool IsRawRelease(LPARAM l)
{
    RAWINPUT ri{};
    UINT sz = sizeof(ri);
    if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return false;
    if (ri.header.dwType == RIM_TYPEKEYBOARD) return (ri.data.keyboard.Flags & RI_KEY_BREAK) != 0;
    if (ri.header.dwType == RIM_TYPEMOUSE) {
        const USHORT f = ri.data.mouse.usButtonFlags;
        const USHORT ups = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP |
                           RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
        const USHORT downs = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN |
                             RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN;
        return (f & ups) && !(f & downs);   // 带着移动也给：真鼠标的松开常和移动在同一包里
    }
    return false;
}

// 面板开着时把键鼠消息吃掉，游戏收不到（手柄另由 padfilter 挡，DirectInput 和按键查询由 kbmfilter 挡）
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    SyncIme(h, msg, g_open.load());
    if (g_open.load()) {
        LRESULT im;
        {
            std::lock_guard<std::recursive_mutex> lk(g_imguiMx);
            im = ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        }
        switch (msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR:
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_XBUTTONDOWN:
            return 0;
        case WM_KEYUP: case WM_SYSKEYUP: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            break;   // 松开照常给游戏：开面板前按着的键要能松开（和 kbmfilter 一样）
        case WM_INPUT:
            // 原始输入：按下、移动不给游戏；松开照常给（开面板前按着的键要能松开，同上）
            if (IsRawRelease(l)) break;
            return ::DefWindowProcW(h, msg, w, l);
        }
        // 输入法的消息交给系统默认处理：打完的字变成 WM_CHAR 进面板；拼音和候选在这里顺手取出来给面板画。
        // 不给游戏（游戏会当成聊天框在打字）。
        // WM_IME_COMPOSITION 在 ImGui 的处理里已经调过一次 DefWindowProc，再调一次，同一个词就会
        // 进两遍（打 heilong 选「黑龙」出来「黑龙黑龙」）；非 Unicode 窗口的 WM_IME_CHAR 它也自己收了
        if (IsImeMessage(msg)) {
            if (g_imeOn == 1) {
                if (msg == WM_IME_COMPOSITION) {
                    if (l & GCS_RESULTSTR) ++g_imeDiag.result;
                    ImeReadComposition(h);   // 上屏之后取到的是空的，正好清掉
                } else if (msg == WM_IME_ENDCOMPOSITION) {
                    ImeClear(true);
                } else if (msg == WM_IME_NOTIFY) {
                    if (w == IMN_OPENCANDIDATE || w == IMN_CHANGECANDIDATE) {
                        if (w == IMN_OPENCANDIDATE) ++g_imeDiag.openCand;
                        ImeReadCandidatesImm(h);
                    } else if (w == IMN_CLOSECANDIDATE) {
                        bool tsf;
                        {
                            std::lock_guard<std::mutex> lk(g_imeViewMx);
                            tsf = g_imeView.fromTsf;
                        }
                        if (!tsf) ImeClear(false);   // TSF 给的那份由 EndUIElement 清
                    }
                }
            }
            if (msg == WM_IME_COMPOSITION || (msg == WM_IME_CHAR && im)) return im;
            if (msg == WM_IME_SETCONTEXT) l = 0;   // 输入法自己的界面都别出：拼音和候选由面板画
            return ::DefWindowProcW(h, msg, w, l);
        }
    }
    return ::CallWindowProcW(g_wndProc, h, msg, w, l);
}

// 虚表里的一格。挂的时候记下原来的；还的时候只有这一格还是我们的才还 —— 之后被别的模组接手了
// （它把我们当成「原函数」存着）就留在链上，关着面板时我们照常放行
struct Slot {
    void** at = nullptr;
    void* hook = nullptr;
    void** orig = nullptr;
    bool on = false;
};

bool WriteSlot(void** at, void* v)
{
    DWORD old = 0;
    if (!::VirtualProtect(at, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *at = v;
    ::VirtualProtect(at, sizeof(void*), old, &old);
    return true;
}

bool Hook(Slot& s)
{
    if (!s.at) return false;
    // 还在链上（上次关面板时这一格被别的模组接手了，没还掉）：别再挂一层，
    // 不然会把它的钩子记成我们的原函数，而它的原函数又是我们，一调就无限递归
    if (s.on) return true;
    if (*s.at == s.hook) { s.on = true; return true; }   // 上次没还掉，还挂着
    void* const prev = *s.at;
    *s.orig = prev;                                      // 先记原函数再换：换完的下一瞬间就可能有人调进来
    if (!WriteSlot(s.at, s.hook)) return false;
    s.on = true;
    return true;
}

// 还回去了返回 true；被别人接手了（留在链上）返回 false
bool Unhook(Slot& s)
{
    if (!s.on || !s.at) return true;
    if (*s.at != s.hook) return false;
    if (!WriteSlot(s.at, *s.orig)) return false;
    s.on = false;
    return true;
}

bool PatchSlot(void** slot, void* hook, void** orig)
{
    DWORD old = 0;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = *slot;
    *slot = hook;
    ::VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// ================================================================ D3D11
// 画不了了（游戏换了设备、缓冲格式）：这一局面板不再用，记一笔就收手
void GiveUp(const char* why)
{
    if (g_failed.exchange(true)) return;
    Log("overlay: %s，这一局面板不可用（重开游戏恢复）", why);
    g_open = false;
}

void ReleaseRtv()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CreateRtv(IDXGISwapChain* sc)
{
    ReleaseRtv();
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return false;
    const HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    return SUCCEEDED(hr);
}

// ================================================================ D3D12
Slot g_slotExecute;
std::mutex g_hookMx;   // 挂 / 还交换链的钩子（主循环开面板时挂，渲染线程关面板后还）

void WINAPI HkExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    ID3D12Device* want = g_wantQueueOf.load();
    if (want && !g_queue.load() && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(q->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&d))) && d) {
            if (d == want) {
                ID3D12CommandQueue* expected = nullptr;
                if (g_queue.compare_exchange_strong(expected, q)) {
                    Log("overlay: 记下游戏的 D3D12 命令队列 %p", (void*)q);
                    std::lock_guard<std::mutex> lk(g_hookMx);
                    Unhook(g_slotExecute);   // 队列拿到了就不用再看游戏提交命令了（提交我们自己的命令用原函数）
                }
            }
            d->Release();
        }
    }
    g_execute(q, n, lists);
}

// 在游戏的设备上建一个临时队列，拿到队列的虚表，挂 ExecuteCommandLists（第 10 项）
bool HookExecute(ID3D12Device* dev)
{
    if (g_execute) return true;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* tmp = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&tmp))) || !tmp) return false;
    void** vt = *reinterpret_cast<void***>(tmp);
    g_slotExecute = {&vt[10], reinterpret_cast<void*>(HkExecute), reinterpret_cast<void**>(&g_execute), false};
    bool ok;
    {
        std::lock_guard<std::mutex> lk(g_hookMx);
        ok = Hook(g_slotExecute);
    }
    tmp->Release();
    Log("overlay: %s ExecuteCommandLists", ok ? "已挂" : "挂不上");
    return ok;
}

void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    const int i = g_srvFree.empty() ? 0 : g_srvFree.back();
    if (!g_srvFree.empty()) g_srvFree.pop_back();
    *cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += (SIZE_T)i * g_srvInc;
    gpu->ptr += (UINT64)i * g_srvInc;
}

void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    g_srvFree.push_back((int)((cpu.ptr - g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g_srvInc));
}

void WaitGpu12()
{
    ID3D12CommandQueue* q = g_queue.load();
    if (!q || !g_fence) return;
    q->Signal(g_fence, ++g_fenceVal);
    if (g_fence->GetCompletedValue() < g_fenceVal) {
        g_fence->SetEventOnCompletion(g_fenceVal, g_fenceEvent);
        ::WaitForSingleObject(g_fenceEvent, 2000);
    }
}

// 0 = 用不了，1 = 好了，2 = 是 D3D12 的，但还没等到游戏的命令队列
int InitD3D12Objects(IDXGISwapChain* sc, ID3D12Device* dev)
{
    if (!HookExecute(dev)) return 0;
    g_wantQueueOf = dev;
    ID3D12CommandQueue* q = g_queue.load();
    if (!q) return 2;
    {   // 只确认一下有 IDXGISwapChain3（要用它取当前缓冲号），不留引用
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) || !sc3) return 0;
        sc3->Release();
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kMaxBuffers;
    if (FAILED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g_rtvHeap)))) return 0;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 64;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g_srvHeap)))) return 0;
    for (int i = 63; i >= 0; --i) g_srvFree.push_back(i);
    g_rtvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_srvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g_fence)))) return 0;
    g_fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (UINT i = 0; i < kMaxBuffers; ++i)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                               reinterpret_cast<void**>(&g_alloc[i])))) return 0;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList),
                                      reinterpret_cast<void**>(&g_list)))) return 0;
    g_list->Close();
    g_dev12 = dev;
    return 1;
}

void Render12(IDXGISwapChain* sc, ImDrawData* dd)
{
    ID3D12CommandQueue* q = g_queue.load();
    // 交换链每帧现拿现放：长期攥着游戏的交换链，游戏重建交换链时（同一个窗口只能有一个）会建不出来
    UINT i = 0;
    {
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) || !sc3) return;
        i = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
    }
    DXGI_SWAP_CHAIN_DESC cur{};
    if (FAILED(sc->GetDesc(&cur)) || cur.BufferDesc.Format != g_bbFormat) {   // 换了缓冲格式：管线对不上，画了会把设备弄没
        GiveUp("游戏换了画面的缓冲格式（比如开了 HDR）");
        return;
    }
    if (i >= kMaxBuffers) return;
    // 每帧现拿后台缓冲、用完就放：不长期持有引用，游戏 ResizeBuffers / 重建交换链都不受影响
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&bb))) || !bb) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)i * g_rtvInc;
    g_dev12->CreateRenderTargetView(bb, nullptr, rtv);

    if (g_fence->GetCompletedValue() < g_allocFence[i]) {        // 这块分配器上次的命令还没跑完
        g_fence->SetEventOnCompletion(g_allocFence[i], g_fenceEvent);
        ::WaitForSingleObject(g_fenceEvent, 1000);
        // 等了一秒还没跑完：这一帧不画。接着 Reset 会把 GPU 还在用的命令和缓冲冲掉，设备直接没了
        if (g_fence->GetCompletedValue() < g_allocFence[i]) { bb->Release(); return; }
    }
    g_alloc[i]->Reset();
    g_list->Reset(g_alloc[i], nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_list->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(dd, g_list);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_execute(q, 1, lists);
    q->Signal(g_fence, ++g_fenceVal);
    g_allocFence[i] = g_fenceVal;
    if (g_debugAfterDraw12) g_debugAfterDraw12(bb, q);
    bb->Release();
}

// ================================================================ 公共
// 用这个交换链初始化。0 = 用不了（别人的、或者是 D3D11/12 以外的），1 = 好了，2 = 等一等（D3D12 还没拿到命令队列）。
// 3 = 前台是本进程别的窗口（比如 Stracker 的控制台）、不是这个交换链的：现在不认，也不算失败。
// 用不了不算失败：进程里所有交换链共用一张虚表，别人的 Present 也会走到这里。
int InitRenderer(IDXGISwapChain* sc)
{
    if (sc == g_foreign) return 0;
    // 按快捷键时游戏窗口在前台（快捷键只在游戏有焦点时才响应）：别的窗口的交换链（别的模组、工具）不认
    HWND fg = ::GetForegroundWindow();
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);
    if (fg && pid == ::GetCurrentProcessId()) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow != fg && ::GetAncestor(d.OutputWindow, GA_ROOT) != fg) {
            // 不记成「别人的」：前台也可能只是一下子点到了同进程的别的窗口（Stracker 的控制台就算游戏进程的），
            // 记下来的话游戏自己的交换链就再也不认了。等切回游戏那一帧再认
            return 3;
        }
    }
    ID3D12Device* dev12 = nullptr;
    g_dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev))) && g_dev) {
        g_dev->GetImmediateContext(&g_ctx);
        g_api = kD3D11;
    } else if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev12))) && dev12) {
        g_dev = nullptr;
        const int r = InitD3D12Objects(sc, dev12);
        dev12->Release();            // 设备归游戏管，交换链活着它就活着
        if (r == 0) {                // 是 D3D12 的游戏画面，但建不起我们要的东西：别每帧重试
            Log("overlay: D3D12 初始化失败，面板不可用；重开快捷键不受影响");
            g_failed = true;
        }
        if (r != 1) return r;
        g_api = kD3D12;
    } else {
        g_dev = nullptr;
        g_foreign = sc;
        DXGI_SWAP_CHAIN_DESC d{};
        sc->GetDesc(&d);
        Log("overlay: 跳过一个既不是 D3D11 也不是 D3D12 的交换链（缓冲 %ux%u）", d.BufferDesc.Width, d.BufferDesc.Height);
        return 0;
    }
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    g_hwnd = d.OutputWindow;
    g_sc = sc;
    g_bbFormat = d.BufferDesc.Format;
    RECT rc{};
    ::GetClientRect(g_hwnd, &rc);
    const float h = (float)d.BufferDesc.Height;   // 字号按真实分辨率定，不按缩小后的窗口

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
    bool ok = ImGui_ImplWin32_Init(g_hwnd);
    // ImGui 默认在渲染线程里直接设输入法位置：窗口不是这个线程的，设不上。改成记下来，交给窗口线程
    ImGui::GetPlatformIO().Platform_SetImeDataFn = [](ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* d) {
        if (!d->WantVisible || !g_hwnd) return;
        RECT rc{};
        ::GetClientRect(g_hwnd, &rc);
        const ImVec2 ds = ImGui::GetIO().DisplaySize;   // 我们按后台缓冲的分辨率排版，窗口坐标要换算回去
        const float fx = ds.x > 0 ? (rc.right - rc.left) / ds.x : 1.0f, fy = ds.y > 0 ? (rc.bottom - rc.top) / ds.y : 1.0f;
        g_imeX = (int)std::lround(d->InputPos.x * fx);
        g_imeY = (int)std::lround(d->InputPos.y * fy);
        g_imeH = (int)std::lround(d->InputLineHeight * fy);
        g_imePosDirty = true;
        ::PostMessageW(g_hwnd, WM_NULL, 0, 0);
    };
    if (ok && g_api == kD3D11) {
        ok = ImGui_ImplDX11_Init(g_dev, g_ctx);
    } else if (ok) {
        ImGui_ImplDX12_InitInfo info;
        info.Device = g_dev12;
        info.CommandQueue = g_queue.load();
        // 按我们能分到的最多缓冲数算：之后游戏加了缓冲数（ResizeBuffers / 重建交换链），ImGui 的每帧缓冲也不会被提前复用
        info.NumFramesInFlight = (int)kMaxBuffers;
        info.RTVFormat = d.BufferDesc.Format;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.SrvDescriptorHeap = g_srvHeap;
        info.SrvDescriptorAllocFn = SrvAlloc;
        info.SrvDescriptorFreeFn = SrvFree;
        ok = ImGui_ImplDX12_Init(&info);
    }
    if (!ok) {
        Log("overlay: ImGui 后端初始化失败");
        g_failed = true;
        return 0;
    }
    if (g_cb.init) g_cb.init(h > 0 ? h / 1080.0f : 1.0f);
    // 先记下原来的窗口过程再换：换上的那一刻窗口线程就可能来消息，这时 g_wndProc 还空着就转不出去
    g_wndProc = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC));
    if (const WNDPROC prev = reinterpret_cast<WNDPROC>(
            ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc))))
        g_wndProc = prev;
    ::PostMessageW(g_hwnd, WM_NULL, 0, 0);   // 让 WndProc 马上跑一次，先把输入法摘掉
    g_inited = true;
    Log("overlay: 就绪（%s，窗口 %ldx%ld，缓冲 %ux%u x%u，格式 %d）", g_api == kD3D11 ? "D3D11" : "D3D12",
        rc.right - rc.left, rc.bottom - rc.top, d.BufferDesc.Width, d.BufferDesc.Height, d.BufferCount, (int)d.BufferDesc.Format);
    return 1;
}

// 这一帧是不是游戏的交换链在 Present。游戏重建交换链（切全屏之类）时指针会变，
// 同一个窗口、同一个设备的就认作新的游戏交换链。
bool IsGameSwapChain(IDXGISwapChain* sc)
{
    if (sc == g_sc) return true;
    if (sc == g_foreign) return false;
    DXGI_SWAP_CHAIN_DESC d{};
    bool same = SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow == g_hwnd;
    if (same && g_api == kD3D11) {
        ID3D11Device* dev = nullptr;
        same = SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev == g_dev;
        if (dev) dev->Release();
    } else if (same) {
        ID3D12Device* dev = nullptr;
        IDXGISwapChain3* sc3 = nullptr;
        same = SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) && dev == g_dev12 &&
               d.BufferDesc.Format == g_bbFormat &&
               SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3)));
        if (dev) dev->Release();
        if (sc3) sc3->Release();
    }
    if (!same) {
        g_foreign = sc;
        // 是游戏的窗口、却对不上（游戏重建了设备，或者别的模组在同一个窗口上另建了交换链）：
        // 手上对旧交换链的引用先放掉（不然它一直活着，游戏可能建不出新的）。不判失败：还是旧的那个在画的话，
        // 下一帧会重新建；真的一直画不出来，看门狗 8 秒后关面板
        if (d.OutputWindow == g_hwnd) ReleaseRtv();
        return false;
    }
    g_sc = sc;
    ReleaseRtv();
    return true;
}

void RenderPanel(IDXGISwapChain* sc)
{
    const bool open = g_open.load();
    const bool justOpened = open && !g_wasOpen;
    // 打开时的回调也要拿 ImGui 的锁：它会动 ImGui 的状态，窗口线程这时可能正往输入队列里塞东西
    if (justOpened && g_cb.opened) {
        std::lock_guard<std::recursive_mutex> lk(g_imguiMx);
        g_cb.opened();
    }
    g_wasOpen = open;
    if (!open) return;
    // D3D11 的渲染目标视图每帧现建现放（和 D3D12 一样）：一直攥着它就攥着游戏的后台缓冲，
    // 游戏放掉旧交换链、在同一个窗口上建新的时会因此失败
    if (g_api == kD3D11 && !CreateRtv(sc)) return;

    std::lock_guard<std::recursive_mutex> lk(g_imguiMx);
    ImGuiIO& io = ImGui::GetIO();
    // 关着面板时的按键松开 ImGui 看不见：刚打开时清掉，免得 Esc、回车像一直按着（下一下按不出来）
    if (justOpened) { io.ClearInputKeys(); io.ClearInputMouse(); }
    if (g_api == kD3D11) ImGui_ImplDX11_NewFrame();
    else ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    io.MouseDrawCursor = true;   // 游戏把系统光标藏了，面板开着时自己画一个
    // 直接按后台缓冲的真实分辨率排版和渲染。游戏没声明高 DPI，系统缩放 125% 时
    // 它拿到的窗口大小是缩小过的（2560x1440 的屏报成 2048x1152），按那个排版再放大就糊了。
    // 鼠标消息里的坐标是缩小后的窗口坐标，排队的鼠标事件按比例换算到缓冲坐标。
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(sc->GetDesc(&d)) && io.DisplaySize.x > 0 && io.DisplaySize.y > 0 && d.BufferDesc.Width && d.BufferDesc.Height) {
        const float sx = d.BufferDesc.Width / io.DisplaySize.x, sy = d.BufferDesc.Height / io.DisplaySize.y;
        if (sx != 1.0f || sy != 1.0f) {
            for (ImGuiInputEvent& e : GImGui->InputEventsQueue) {
                if (e.Type != ImGuiInputEventType_MousePos || e.EventId <= g_lastScaledEvent) continue;
                if (e.MousePos.PosX != -FLT_MAX) { e.MousePos.PosX *= sx; e.MousePos.PosY *= sy; }
                g_lastScaledEvent = e.EventId;   // 没处理完留到下一帧的事件不能再乘一遍
            }
        }
        io.DisplaySize = ImVec2((float)d.BufferDesc.Width, (float)d.BufferDesc.Height);
        io.DisplayFramebufferScale = ImVec2(1, 1);
    }
    ImGui::NewFrame();
    if (g_imeWanted.load() && g_hwnd) ::PostMessageW(g_hwnd, WM_NULL, 0, 0);   // 打名字时：让窗口线程看看输入法界面
    // 窗口最小化时显示区是 0x0：按它排版会出 0 大小的东西（ImGui 会断言），这一帧什么都不画
    if (io.DisplaySize.x >= 1 && io.DisplaySize.y >= 1 && g_cb.draw) g_cb.draw(io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();
    g_lastDrawAt = ::GetTickCount64();
    if (g_api == kD3D12) {
        Render12(sc, ImGui::GetDrawData());
        return;
    }
    // 游戏原来绑着的渲染目标：画完还回去（ImGui 的后端不管这个；绑着我们的视图，游戏换后台缓冲会失败）
    ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
    for (ID3D11RenderTargetView* v : oldRtv) if (v) v->Release();
    if (oldDsv) oldDsv->Release();
    ReleaseRtv();

    if (g_debugAfterDraw) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb) {
            g_debugAfterDraw(bb);
            bb->Release();
        }
    }
}

void ReleaseForClose();
void UnhookSwapChain();
extern std::atomic<bool> g_cleaned;

HRESULT WINAPI HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    const PresentFn orig = g_present;
    if (!g_failed.load() && !(flags & DXGI_PRESENT_TEST)) {
        // 第一次打开面板才初始化（要读字体文件），从没打开过就完全不碰游戏的渲染
        if (!g_inited && g_open.load()) {
            const int r = InitRenderer(sc);
            // D3D12 要等游戏提交一次命令才知道它的队列，多给点时间
            const unsigned long long limit = r == 2 ? 4000 : 1500;
            if (r != 1 && r != 3 && !g_failed.load() && ::GetTickCount64() - g_openAt.load() > limit) {
                Log(r == 2 ? "overlay: 等不到游戏的 D3D12 命令队列，面板不可用；重开快捷键不受影响"
                           : "overlay: 没找到能画面板的交换链（D3D11 / D3D12 都不是），面板不可用；重开快捷键不受影响");
                g_failed = true;
            }
        }
        if (g_inited && IsGameSwapChain(sc)) RenderPanel(sc);
    }
    // 面板关了：放掉手上的后台缓冲，把钩子还回去，之后游戏的 Present 不再经过这里。
    // 只在游戏自己的交换链上做（就是渲染线程，不会和正在画的那一帧打架）
    // 放弃了（游戏换了交换链）也要收拾：这时 g_sc 是旧的，等不到它了
    if (!g_open.load() && !g_cleaned.load() && (!g_inited || sc == g_sc || g_failed.load())) {
        ReleaseForClose();
        UnhookSwapChain();
    }
    return orig(sc, sync, flags);
}

// 游戏要换后台缓冲：D3D11 先放掉手上的 RTV；D3D12 等自己提交的命令跑完（缓冲本来就不长期持有）
void BeforeResize(IDXGISwapChain* sc)
{
    if (sc != g_sc) return;
    if (g_api == kD3D11) ReleaseRtv();
    else if (g_api == kD3D12) WaitGpu12();
}

// D3D12：换缓冲格式的 ResizeBuffers（开关 HDR）。ImGui 的管线是按旧格式建的，之后不能再画
void NoteResizeFormat(IDXGISwapChain* sc, DXGI_FORMAT f)
{
    if (sc == g_sc && g_api == kD3D12 && f != DXGI_FORMAT_UNKNOWN && f != g_bbFormat)
        GiveUp("游戏换了画面的缓冲格式（比如开了 HDR）");
}

HRESULT WINAPI HkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    NoteResizeFormat(sc, f);
    BeforeResize(sc);
    return g_resize(sc, n, w, h, f, flags);
}

HRESULT WINAPI HkResize1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT* mask,
                         IUnknown* const* queues)
{
    NoteResizeFormat(sc, f);
    BeforeResize(sc);
    return g_resize1(sc, n, w, h, f, flags, mask, queues);
}

// ---- 交换链的钩子：第一次打开面板时才挂，关面板后还回去 ----
// 不开面板就完全不碰游戏的 Present。别的模组（比如 DLSS4 的 NVIDIA Reflex）启动时检查、挂钩
// 看到的都是原样的交换链；我们开面板时接在它们后面，关面板再还回去。
Slot g_slotPresent, g_slotResize, g_slotResize1;
std::atomic<bool> g_cleaned{true};   // 这次关面板后已经收拾过了（被别人接手、还不掉时，每帧都会进来，只收拾一次）

void ReleaseForClose()
{
    g_wasOpen = false;
    if (g_api == kD3D11) {
        ReleaseRtv();
    } else if (g_api == kD3D12) {
        WaitGpu12();   // 我们提交的命令跑完，才不再碰后台缓冲
    }
}

void UnhookSwapChain()
{
    std::lock_guard<std::mutex> lk(g_hookMx);
    if (g_open.load()) return;   // 这一瞬间又开了：别还
    g_cleaned = true;
    if (!g_slotPresent.on) return;
    const bool a = Unhook(g_slotPresent), b = Unhook(g_slotResize), c = Unhook(g_slotResize1);
    Log(a && b && c ? "overlay: 面板关了，Present 还回去了"
                    : "overlay: 面板关了；Present 已经被别的模组接手，我们留在它的链上照常放行");
}

// ---- 诊断：窗口管理器对象 ----
// 建临时窗口在游戏里报过 1158（ERROR_NO_MORE_USER_HANDLES：窗口、钩子、输入法上下文这类对象用光了，
// 每个进程最多一万个）。失败时记下现在有多少、最多到过多少、本进程都有哪些窗口，好看出是谁建的
struct WinTally {
    DWORD pid = 0;
    int total = 0;
    std::vector<std::pair<std::wstring, int>> cls;
};
void TallyOne(HWND h, WinTally& t)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid != t.pid) return;
    wchar_t name[80] = L"?";
    ::GetClassNameW(h, name, 80);
    ++t.total;
    for (auto& p : t.cls)
        if (p.first == name) { ++p.second; return; }
    t.cls.emplace_back(name, 1);
}
BOOL CALLBACK TallyChild(HWND h, LPARAM lp)
{
    TallyOne(h, *reinterpret_cast<WinTally*>(lp));
    return TRUE;
}
BOOL CALLBACK TallyTop(HWND h, LPARAM lp)
{
    TallyOne(h, *reinterpret_cast<WinTally*>(lp));
    ::EnumChildWindows(h, TallyChild, lp);
    return TRUE;
}
std::string DescribeUserObjects()
{
    const HANDLE me = ::GetCurrentProcess();
    // 0 = GDI，1 = USER，2 = GDI 峰值，4 = USER 峰值
    char buf[160];
    std::snprintf(buf, sizeof(buf), "窗口对象 %lu 个（最多到过 %lu），GDI %lu 个（最多到过 %lu）",
                  (unsigned long)::GetGuiResources(me, 1), (unsigned long)::GetGuiResources(me, 4),
                  (unsigned long)::GetGuiResources(me, 0), (unsigned long)::GetGuiResources(me, 2));
    std::string out = buf;
    WinTally t;
    t.pid = ::GetCurrentProcessId();
    ::EnumWindows(TallyTop, reinterpret_cast<LPARAM>(&t));
    for (HWND h = nullptr; (h = ::FindWindowExW(HWND_MESSAGE, h, nullptr, nullptr)) != nullptr;) TallyOne(h, t);   // 只收消息的窗口
    std::sort(t.cls.begin(), t.cls.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::snprintf(buf, sizeof(buf), "；本进程窗口 %d 个", t.total);
    out += buf;
    int shown = 0;
    for (const auto& p : t.cls) {
        if (shown++ == 8) { out += " …"; break; }
        char name[256] = "?";
        ::WideCharToMultiByte(CP_UTF8, 0, p.first.c_str(), -1, name, sizeof(name), nullptr, nullptr);
        std::snprintf(buf, sizeof(buf), "%s%s×%d", shown == 1 ? "：" : "、", name, p.second);
        out += buf;
    }
    return out;
}

// ---- 拿交换链的虚表（D3D11 和 D3D12 的交换链是同一个类，共用这张表）----
int g_debugVtFail = 0;   // 测试用：1 = 当作带窗口的办法失败，2 = 当作不带窗口的办法失败

// 从建好的交换链上读两张虚表
void** ReadVtables(IUnknown* any, void*** vt3)
{
    void** vt = nullptr;
    IDXGISwapChain* sc = nullptr;
    if (SUCCEEDED(any->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&sc))) && sc) {
        vt = *reinterpret_cast<void***>(sc);
        sc->Release();
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (vt && SUCCEEDED(any->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) && sc3) {
        *vt3 = *reinterpret_cast<void***>(sc3);
        sc3->Release();
    }
    return vt;
}

// 办法一：建一个临时窗口，在它上面建交换链
void** VtableViaWindow(std::string& err, void*** vt3)
{
    char buf[160];
    if (g_debugVtFail & 1) { err = "建临时窗口失败（测试）【" + DescribeUserObjects() + "】"; return nullptr; }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BetterINQ_dummy";
    const DWORD clsErr = ::RegisterClassExW(&wc) ? 0 : ::GetLastError();
    HWND hw = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hw) {   // 没有窗口，交换链一定建不出来（报的是 DXGI_ERROR_INVALID_CALL，看不出原因）：在这里就记清楚
        const DWORD e = ::GetLastError();
        std::snprintf(buf, sizeof(buf), "建临时窗口失败 err=%lu（注册窗口类 err=%lu）", (unsigned long)e, (unsigned long)clsErr);
        err = buf;
        err += "【" + DescribeUserObjects() + "】";
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return nullptr;
    }
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const HRESULT hrHw = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                         D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    HRESULT hr = hrHw;
    if (FAILED(hr))
        hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    void** vt = nullptr;
    if (SUCCEEDED(hr) && sc) vt = ReadVtables(sc, vt3);
    if (!vt) {
        std::snprintf(buf, sizeof(buf), "建临时交换链失败 hr=0x%08lx（软件渲染 hr=0x%08lx）", (unsigned long)hrHw, (unsigned long)hr);
        err = buf;
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    ::DestroyWindow(hw);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return vt;
}

// 办法二：不要窗口，建一个给合成用的交换链（CreateSwapChainForComposition）。同一个类，虚表是同一张
void** VtableViaComposition(std::string& err, void*** vt3)
{
    char buf[160];
    if (g_debugVtFail & 2) { err = "建不带窗口的交换链失败（测试）"; return nullptr; }
    ID3D11Device* dev = nullptr;
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    if (FAILED(hr))
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    const char* step = "建设备";
    IDXGIDevice* xd = nullptr;
    IDXGIAdapter* ad = nullptr;
    IDXGIFactory2* f2 = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(hr)) { step = "拿工厂"; hr = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&xd)); }
    if (SUCCEEDED(hr)) hr = xd->GetAdapter(&ad);
    if (SUCCEEDED(hr)) hr = ad->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2));
    if (SUCCEEDED(hr)) {
        step = "建交换链";
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 64;
        d.Height = 64;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = f2->CreateSwapChainForComposition(dev, &d, nullptr, &sc1);
    }
    void** vt = nullptr;
    if (SUCCEEDED(hr) && sc1) vt = ReadVtables(sc1, vt3);
    if (!vt) {
        std::snprintf(buf, sizeof(buf), "建不带窗口的交换链失败（%s）hr=0x%08lx", step, (unsigned long)hr);
        err = buf;
    }
    if (sc1) sc1->Release();
    if (f2) f2->Release();
    if (ad) ad->Release();
    if (xd) xd->Release();
    if (dev) dev->Release();
    return vt;
}

void** GetSwapChainVtable(std::string& err, void*** vt3)
{
    *vt3 = nullptr;
    std::string e1, e2;
    void** vt = VtableViaWindow(e1, vt3);
    if (vt) return vt;
    *vt3 = nullptr;
    vt = VtableViaComposition(e2, vt3);
    if (vt) { Log("overlay: %s；改用不带窗口的办法拿到了交换链虚表", e1.c_str()); return vt; }
    err = e1 + "；" + e2;
    return nullptr;
}

} // namespace

bool Install(const Callbacks& cb, std::string&)
{
    g_cb = cb;
    Log("overlay: 等第一次打开面板时再挂 Present（不开面板就不碰游戏的画面）");
    return true;
}

namespace {
unsigned long long g_vtRetryAt = 0;   // 拿虚表失败后，过了这个时刻才再试（别每一轮主循环都去建设备）
int g_vtFails = 0;

// 主循环里开面板时调。第一次先拿交换链的虚表。
// 拿不到不算整局失败：这一次打不开，下次再开时重试；连着 5 次都不行才放弃
bool HookSwapChain()
{
    std::lock_guard<std::mutex> lk(g_hookMx);
    if (!g_slotPresent.at) {
        const unsigned long long now = ::GetTickCount64();
        if (now < g_vtRetryAt) return false;
        std::string err;
        void** vt3 = nullptr;
        void** vt = GetSwapChainVtable(err, &vt3);
        if (!vt) {
            g_vtRetryAt = now + 2000;
            if (++g_vtFails >= 5) {
                g_failed = true;
                Log("overlay: %s。试了 %d 次都不行，这一局面板不可用（重开游戏恢复）", err.c_str(), g_vtFails);
            } else {
                Log("overlay: %s。这一次面板打不开，再开时会重试（第 %d 次）", err.c_str(), g_vtFails);
            }
            return false;
        }
        // IDXGISwapChain 虚表：8 = Present，13 = ResizeBuffers；IDXGISwapChain3：39 = ResizeBuffers1（DX12 游戏可能用它）
        g_slotPresent = {&vt[8], reinterpret_cast<void*>(HkPresent), reinterpret_cast<void**>(&g_present), false};
        g_slotResize = {&vt[13], reinterpret_cast<void*>(HkResize), reinterpret_cast<void**>(&g_resize), false};
        if (vt3) g_slotResize1 = {&vt3[39], reinterpret_cast<void*>(HkResize1), reinterpret_cast<void**>(&g_resize1), false};
    }
    // 每一格各挂各的：上次关面板时可能 Present 被别人接手还挂着、ResizeBuffers 却已经还回去了
    const bool was = g_slotPresent.on;
    if (!Hook(g_slotPresent) || !Hook(g_slotResize)) {
        g_failed = true;
        Log("overlay: 改交换链虚表失败，面板不可用");
        return false;
    }
    if (g_slotResize1.at && !Hook(g_slotResize1) && !was) Log("overlay: ResizeBuffers1 没挂上（不影响 D3D11）");
    if (!was) Log("overlay: 挂上 Present（原 %p）", reinterpret_cast<void*>(g_present));
    return true;
}
} // namespace

void SetOpen(bool open)
{
    if (open) {
        {   // 先在锁里标上「开着」：渲染线程正准备还钩子的话，看到它就不还了（还完了的话下面再挂上）
            std::lock_guard<std::mutex> lk(g_hookMx);
            if (!g_open.load()) g_openAt = ::GetTickCount64();
            g_cleaned = false;
            // 上次关的时候，渲染线程正在画的那一帧可能又把「在打字」置上了：打开时从没在打字开始
            g_imeWanted = false;
            g_open = !g_failed.load();
        }
        if (g_open.load() && !HookSwapChain()) g_open = false;   // 这一次没打开（整局不行的话 HookSwapChain 里会记）
    } else {
        g_open = false;
        g_imeWanted = false;   // 面板关了就不在打名字了（不然下次一打开会先挂一下输入法）
    }
    if (g_wndProc && g_hwnd) ::PostMessageW(g_hwnd, WM_NULL, 0, 0);   // 输入法摘下 / 还回去
}

bool Watchdog()
{
    if (!g_open.load() || g_failed.load()) return false;
    // 从打开（或上一次画出来）算起 8 秒没画出过一帧。初始化之后也会这样：游戏换了设备、换了缓冲格式，
    // 交换链对不上就一直不画
    const unsigned long long since = std::max(g_openAt.load(), g_lastDrawAt.load());
    if (::GetTickCount64() - since < 8000) return false;
    // 开了 8 秒还没画出过一帧：游戏没走挂上的 Present（切到了后台、用的是 Present1、代理交换链……）。
    // 这一次先关掉（别一直挡着输入），不算彻底失败：之后还能再开
    Log("overlay: 开了 8 秒还没轮到我们画（游戏没走挂上的 Present），先关掉；重开快捷键不受影响");
    g_open = false;
    return true;
}
bool IsOpen() { return g_open.load() && !g_failed.load(); }
void SetImeWanted(bool want)
{
    if (g_imeWanted.exchange(want) != want && g_wndProc && g_hwnd) ::PostMessageW(g_hwnd, WM_NULL, 0, 0);
}
bool Failed() { return g_failed.load(); }
void SetDebugAfterDraw(void (*fn)(void*)) { g_debugAfterDraw = fn; }
namespace {
std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
} // namespace

ImeText GetImeText()
{
    ImeView v;
    {
        std::lock_guard<std::mutex> lk(g_imeViewMx);
        v = g_imeView;
    }
    ImeText t;
    t.comp = Utf8(v.comp);
    for (const std::wstring& c : v.cands) t.cands.push_back(Utf8(c));
    t.sel = v.sel;
    return t;
}
void SetDebugImeText(const wchar_t* comp, const std::vector<std::wstring>& cands, int sel)
{
    std::lock_guard<std::mutex> lk(g_imeViewMx);
    g_imeView.comp = comp ? comp : L"";
    g_imeView.cands = cands;
    g_imeView.sel = sel;
    g_imeView.fromTsf = false;
}
void DebugReadCandidateElement(void* element) { ReadCandidateElement(static_cast<IUnknown*>(element)); }
void SetDebugVtableFail(int mask)
{
    std::lock_guard<std::mutex> lk(g_hookMx);
    g_debugVtFail = mask;
    g_vtRetryAt = 0;   // 测试里不等那 2 秒
}
void SetDebugAfterDraw12(void (*fn)(void*, void*)) { g_debugAfterDraw12 = fn; }

} // namespace overlay
