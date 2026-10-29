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
#include <atomic>
#include <mutex>
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
bool g_inited = false;
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
IDXGISwapChain3* g_sc3 = nullptr;
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

void SyncIme(HWND h, UINT msg, bool open)
{
    if (!open) {
        if (g_imeOwned) ::ImmAssociateContext(h, g_gameImc);   // 原样还回去（原来是空的就还是关着）
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
        } else {
            ::ImmAssociateContext(h, nullptr);
        }
        g_imeOn = want;
    } else if (!want && (msg == WM_KEYDOWN || msg == WM_INPUTLANGCHANGE || msg == WM_IME_STARTCOMPOSITION)) {
        if (HIMC prev = ::ImmAssociateContext(h, nullptr)) g_gameImc = prev;   // 游戏刚又挂上的
    }
}

bool IsImeMessage(UINT msg)
{
    return (msg >= WM_IME_STARTCOMPOSITION && msg <= WM_IME_KEYLAST) || (msg >= WM_IME_SETCONTEXT && msg <= WM_IME_KEYUP);
}

// 面板开着时把键鼠消息吃掉，游戏收不到（手柄另由 padfilter 挡，DirectInput 和按键查询由 kbmfilter 挡）
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    SyncIme(h, msg, g_open.load());
    if (g_open.load()) {
        const LRESULT im = ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            return 0;
        case WM_INPUT:
            return ::DefWindowProcW(h, msg, w, l);   // 原始输入：交给系统收尾，不给游戏
        }
        // 输入法的消息交给系统默认处理：组字、候选框照常显示，打完的字变成 WM_CHAR 进面板。
        // 不给游戏（游戏会当成聊天框在打字）。
        // WM_IME_COMPOSITION 在 ImGui 的处理里已经调过一次 DefWindowProc，再调一次，同一个词就会
        // 进两遍（打 heilong 选「黑龙」出来「黑龙黑龙」）；非 Unicode 窗口的 WM_IME_CHAR 它也自己收了
        if (IsImeMessage(msg)) {
            if (msg == WM_IME_COMPOSITION || (msg == WM_IME_CHAR && im)) return im;
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
void ReleaseRtv()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CreateRtv(IDXGISwapChain* sc)
{
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
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&g_sc3))) || !g_sc3) return 0;
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
    if (!g_sc3 && (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&g_sc3))) || !g_sc3)) return;
    const UINT i = g_sc3->GetCurrentBackBufferIndex();
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
            g_foreign = sc;
            return 0;
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
    if (ok && g_api == kD3D11) {
        ok = ImGui_ImplDX11_Init(g_dev, g_ctx);
    } else if (ok) {
        ImGui_ImplDX12_InitInfo info;
        info.Device = g_dev12;
        info.CommandQueue = g_queue.load();
        info.NumFramesInFlight = (int)(d.BufferCount < 2 ? 2 : d.BufferCount);
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
    g_wndProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
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
        if (same) {
            if (g_sc3) g_sc3->Release();
            g_sc3 = sc3;
        }
    }
    if (!same) { g_foreign = sc; return false; }
    g_sc = sc;
    ReleaseRtv();
    return true;
}

void RenderPanel(IDXGISwapChain* sc)
{
    const bool open = g_open.load();
    if (open && !g_wasOpen && g_cb.opened) g_cb.opened();
    g_wasOpen = open;
    if (!open) return;
    if (g_api == kD3D11 && !g_rtv && !CreateRtv(sc)) return;

    if (g_api == kD3D11) ImGui_ImplDX11_NewFrame();
    else ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
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
    if (g_cb.draw) g_cb.draw(io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();
    if (g_api == kD3D12) {
        Render12(sc, ImGui::GetDrawData());
        return;
    }
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

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
            if (r != 1 && !g_failed.load() && ::GetTickCount64() - g_openAt.load() > limit) {
                Log(r == 2 ? "overlay: 等不到游戏的 D3D12 命令队列，面板不可用；重开快捷键不受影响"
                           : "overlay: 没找到能画面板的交换链（D3D11 / D3D12 都不是），面板不可用；重开快捷键不受影响");
                g_failed = true;
            }
        }
        if (g_inited && IsGameSwapChain(sc)) RenderPanel(sc);
    }
    // 面板关了：放掉手上的后台缓冲，把钩子还回去，之后游戏的 Present 不再经过这里。
    // 只在游戏自己的交换链上做（就是渲染线程，不会和正在画的那一帧打架）
    if (!g_open.load() && !g_cleaned.load() && (!g_inited || sc == g_sc)) {
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

HRESULT WINAPI HkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    BeforeResize(sc);
    return g_resize(sc, n, w, h, f, flags);
}

HRESULT WINAPI HkResize1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT* mask,
                         IUnknown* const* queues)
{
    BeforeResize(sc);
    return g_resize1(sc, n, w, h, f, flags, mask, queues);
}

// ---- 交换链的钩子：第一次打开面板时才挂，关面板后还回去 ----
// 不开面板就完全不碰游戏的 Present。别的模组（比如 DLSS4 的 NVIDIA Reflex）启动时检查、挂钩
// 看到的都是原样的交换链；我们开面板时接在它们后面，关面板再还回去。
Slot g_slotPresent, g_slotResize, g_slotResize1;
bool g_vtTried = false;
std::atomic<bool> g_cleaned{true};   // 这次关面板后已经收拾过了（被别人接手、还不掉时，每帧都会进来，只收拾一次）

void ReleaseForClose()
{
    g_wasOpen = false;
    if (g_api == kD3D11) {
        ReleaseRtv();
    } else if (g_api == kD3D12) {
        WaitGpu12();   // 我们提交的命令跑完，才不再碰后台缓冲
        if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
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

// 建一个临时窗口和交换链，只为拿到交换链的虚表（D3D11 和 D3D12 的交换链是同一个类，共用这张表）
void** GetSwapChainVtable(std::string& err, void*** vt3)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BetterINQ_dummy";
    ::RegisterClassExW(&wc);
    HWND hw = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);
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
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                 D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr))
        hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    void** vt = nullptr;
    *vt3 = nullptr;
    if (SUCCEEDED(hr) && sc) {
        vt = *reinterpret_cast<void***>(sc);
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) && sc3) {
            *vt3 = *reinterpret_cast<void***>(sc3);
            sc3->Release();
        }
    } else {
        err = "建临时交换链失败 hr=" + std::to_string((long)hr);
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (hw) ::DestroyWindow(hw);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return vt;
}

} // namespace

bool Install(const Callbacks& cb, std::string&)
{
    g_cb = cb;
    Log("overlay: 等第一次打开面板时再挂 Present（不开面板就不碰游戏的画面）");
    return true;
}

namespace {
// 主循环里开面板时调。第一次先建临时交换链拿虚表
bool HookSwapChain()
{
    std::lock_guard<std::mutex> lk(g_hookMx);
    if (!g_vtTried) {
        g_vtTried = true;
        std::string err;
        void** vt3 = nullptr;
        void** vt = GetSwapChainVtable(err, &vt3);
        if (!vt) { Log("overlay: %s，面板不可用", err.c_str()); return false; }
        // IDXGISwapChain 虚表：8 = Present，13 = ResizeBuffers；IDXGISwapChain3：39 = ResizeBuffers1（DX12 游戏可能用它）
        g_slotPresent = {&vt[8], reinterpret_cast<void*>(HkPresent), reinterpret_cast<void**>(&g_present), false};
        g_slotResize = {&vt[13], reinterpret_cast<void*>(HkResize), reinterpret_cast<void**>(&g_resize), false};
        if (vt3) g_slotResize1 = {&vt3[39], reinterpret_cast<void*>(HkResize1), reinterpret_cast<void**>(&g_resize1), false};
    }
    if (!g_slotPresent.at) return false;
    if (g_slotPresent.on) return true;
    if (!Hook(g_slotPresent) || !Hook(g_slotResize)) { Log("overlay: 改交换链虚表失败，面板不可用"); return false; }
    if (g_slotResize1.at && !Hook(g_slotResize1)) Log("overlay: ResizeBuffers1 没挂上（不影响 D3D11）");
    Log("overlay: 挂上 Present（原 %p）", reinterpret_cast<void*>(g_present));
    return true;
}
} // namespace

void SetOpen(bool open)
{
    if (open && !g_open.load()) g_openAt = ::GetTickCount64();
    if (open && !g_failed.load() && !HookSwapChain()) g_failed = true;
    if (open) g_cleaned = false;
    g_open = open && !g_failed.load();
    if (g_wndProc && g_hwnd) ::PostMessageW(g_hwnd, WM_NULL, 0, 0);   // 输入法摘下 / 还回去
}
bool IsOpen() { return g_open.load() && !g_failed.load(); }
void SetImeWanted(bool want)
{
    if (g_imeWanted.exchange(want) != want && g_wndProc && g_hwnd) ::PostMessageW(g_hwnd, WM_NULL, 0, 0);
}
bool Failed() { return g_failed.load(); }
void SetDebugAfterDraw(void (*fn)(void*)) { g_debugAfterDraw = fn; }
void SetDebugAfterDraw12(void (*fn)(void*, void*)) { g_debugAfterDraw12 = fn; }

} // namespace overlay
