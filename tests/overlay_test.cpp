// 在游戏外模拟「游戏每帧 Present」，测面板挂载、改分辨率，以及手柄过滤器。
//   overlay_test.exe <输出目录>
#include <windows.h>
#include <imm.h>
#include <msctf.h>
#include <objbase.h>
#include <d3d11.h>
#include <wincodec.h>
#include <xinput.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "../src/config.h"
#include "../src/overlay.h"
#include "../src/padfilter.h"
#include "../src/ui.h"
#include "imgui.h"
#include "imgui_internal.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

// ---- 模拟游戏：它自己的导入表里有 XInputGetState，padfilter 要能找到并换掉 ----
static DWORD GameReadPad(DWORD i, XINPUT_STATE* s) { return XInputGetState(i, s); }

static Config g_cfg;
static ui::Model g_model;
static int g_draws = 0, g_opened = 0;
static std::wstring g_shot;
static ID3D11Device* g_dev = nullptr;

static void Save(void* tex)
{
    if (g_shot.empty()) return;
    ID3D11Texture2D* bb = static_cast<ID3D11Texture2D*>(tex);
    ID3D11DeviceContext* ctx = nullptr;
    g_dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    g_dev->CreateTexture2D(&d, nullptr, &st);
    ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE map{};
    ctx->Map(st, 0, D3D11_MAP_READ, 0, &map);
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    IWICStream* s = nullptr; f->CreateStream(&s); s->InitializeFromFilename(g_shot.c_str(), GENERIC_WRITE);
    IWICBitmapEncoder* enc = nullptr; f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(s, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* fr = nullptr; enc->CreateNewFrame(&fr, nullptr); fr->Initialize(nullptr);
    fr->SetSize(d.Width, d.Height);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    fr->WritePixels(d.Height, map.RowPitch, map.RowPitch * d.Height, (BYTE*)map.pData);
    fr->Commit(); enc->Commit();
    fr->Release(); enc->Release(); s->Release(); f->Release();
    ctx->Unmap(st, 0); st->Release(); ctx->Release();
    std::wprintf(L"  截图 %ls\n", g_shot.c_str());
    g_shot.clear();
}

// 假的输入法候选表（TSF 的 ITfCandidateListUIElement），虚表顺序照 Windows SDK：
// 23 个词，每页 9 个，现在翻到第 2 页，选中的是第 11 个（这一页的第 2 个）。
// 游戏里出过一次崩溃：取候选表的调用被编译器优化成了空调用，离线又没跑到。用它把那段代码真跑一遍
struct FakeCandList {
    void** vt;
    static HRESULT STDMETHODCALLTYPE QI(FakeCandList* self, REFIID, void** out) { *out = self; return S_OK; }
    static ULONG STDMETHODCALLTYPE Ref(FakeCandList*) { return 1; }
    static HRESULT STDMETHODCALLTYPE Nope(FakeCandList*) { return E_NOTIMPL; }
    static HRESULT STDMETHODCALLTYPE Count(FakeCandList*, UINT* n) { *n = 23; return S_OK; }
    static HRESULT STDMETHODCALLTYPE Sel(FakeCandList*, UINT* n) { *n = 10; return S_OK; }
    static HRESULT STDMETHODCALLTYPE Str(FakeCandList*, UINT i, BSTR* out)
    {
        typedef BSTR(WINAPI * Alloc)(const OLECHAR*);
        static const Alloc alloc = reinterpret_cast<Alloc>(::GetProcAddress(::LoadLibraryW(L"oleaut32.dll"), "SysAllocString"));
        wchar_t b[16];
        std::swprintf(b, 16, L"\u8BCD%u", i);   // 词0、词1……
        *out = alloc ? alloc(b) : nullptr;
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    static HRESULT STDMETHODCALLTYPE PageIdx(FakeCandList*, UINT* idx, UINT size, UINT* pages)
    {
        static const UINT starts[3] = {0, 9, 18};
        *pages = 3;
        if (idx)
            for (UINT i = 0; i < size && i < 3; ++i) idx[i] = starts[i];
        return S_OK;
    }
    static HRESULT STDMETHODCALLTYPE Page(FakeCandList*, UINT* n) { *n = 1; return S_OK; }
};

static void OnInit(float scale) { ui::Init(scale); }
static void OnOpened() { ui::OnOpen(g_model); ++g_opened; }
// 和插件里一样：画完把「要不要输入法」告诉 overlay
static void OnDraw(float w, float h)
{
    overlay::ImeText ime = overlay::GetImeText();   // 和插件里一样：输入法的拼音、候选词拿来给面板画
    g_model.imeComp = ime.comp;
    g_model.imeCands = ime.cands;
    g_model.imeSel = ime.sel;
    ui::Draw(g_model, w, h);
    overlay::SetImeWanted(g_model.wantIme);
    ++g_draws;
}

static void Pad(XINPUT_STATE& s, WORD buttons) { std::memset(&s, 0, sizeof(s)); s.Gamepad.wButtons = buttons; }

int wmain(int argc, wchar_t** argv)
{
    const std::wstring out = argc > 1 ? argv[1] : L".";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // ---------------- 手柄过滤器 ----------------
    std::printf("== padfilter\n");
    const unsigned short V = XINPUT_GAMEPAD_BACK, M = XINPUT_GAMEPAD_START, A = XINPUT_GAMEPAD_A;
    padfilter::SetChord(V | M);
    XINPUT_STATE s;
    Pad(s, A); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == A, "不在组合里的键原样放行");
    Pad(s, 0); padfilter::TestFilter(0, &s);
    Pad(s, V); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == 0, "刚按下视图键：先扣住");
    Pad(s, V | M); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == 0, "凑齐视图+菜单：游戏收不到");
    Pad(s, V); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == 0, "组合松开一半：还是扣着");
    Pad(s, 0); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == 0, "全松开：没有补发（组合不算点按）");

    Pad(s, M); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == 0, "单按菜单键：先扣住");
    Pad(s, 0); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == M, "100ms 内松开：补发一次点按（第 1 拍）");
    int pulses = 1;
    for (int k = 0; k < 6; ++k) { Pad(s, 0); padfilter::TestFilter(0, &s); if (s.Gamepad.wButtons == M) ++pulses; }
    CHECK(pulses == 4, "补发持续 4 拍（实际 %d）", pulses);

    Pad(s, M); padfilter::TestFilter(0, &s);
    ::Sleep(130);
    Pad(s, M); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == M, "按住超过 100ms：放行给游戏（开菜单）");
    Pad(s, M | V); padfilter::TestFilter(0, &s);
    CHECK((s.Gamepad.wButtons & M) == M, "已经放行后再凑组合：不再拦（游戏那边菜单已经开了）");
    Pad(s, 0); padfilter::TestFilter(0, &s);

    // 组合里有出招键（Y+视图键）：Y 照常立刻给游戏，只扣视图键
    {
        const WORD Y = XINPUT_GAMEPAD_Y;
        padfilter::SetChord(Y | V);
        Pad(s, Y); padfilter::TestFilter(0, &s);
        CHECK(s.Gamepad.wButtons == Y, "Y+视图键 当组合：单按 Y 立刻给游戏（登龙之类的连招不被吞）");
        Pad(s, Y | XINPUT_GAMEPAD_B); padfilter::TestFilter(0, &s);
        CHECK(s.Gamepad.wButtons == (Y | XINPUT_GAMEPAD_B), "Y+B 一起按：原样给游戏");
        Pad(s, 0); padfilter::TestFilter(0, &s);
        Pad(s, Y); padfilter::TestFilter(0, &s);
        Pad(s, Y | V); padfilter::TestFilter(0, &s);
        CHECK(s.Gamepad.wButtons == Y, "凑齐 Y+视图键：视图键扣住，Y 照给（游戏那边收到一下 Y）");
        Pad(s, 0); padfilter::TestFilter(0, &s);
        CHECK(s.Gamepad.wButtons == 0, "松开：视图键不补发（组合不算点按）");
        Pad(s, V); padfilter::TestFilter(0, &s);
        CHECK(s.Gamepad.wButtons == 0, "单按视图键：照旧先扣住");
        Pad(s, 0); padfilter::TestFilter(0, &s);
        CHECK(s.Gamepad.wButtons == V, "松开：补发一次视图键");
        for (int k = 0; k < 6; ++k) { Pad(s, 0); padfilter::TestFilter(0, &s); }
        padfilter::SetChord(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_A);
        Pad(s, XINPUT_GAMEPAD_LEFT_SHOULDER); padfilter::TestFilter(0, &s);
        CHECK(s.Gamepad.wButtons == XINPUT_GAMEPAD_LEFT_SHOULDER, "组合里没有视图键 / 菜单键：什么都不扣");
        Pad(s, 0); padfilter::TestFilter(0, &s);
        padfilter::SetChord(V | M);
    }
    Pad(s, 0); padfilter::TestFilter(0, &s);

    padfilter::SetBlock(true);
    Pad(s, A | XINPUT_GAMEPAD_DPAD_UP); s.Gamepad.bRightTrigger = 255; s.Gamepad.sThumbLX = 30000;
    padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == 0 && s.Gamepad.bRightTrigger == 0 && s.Gamepad.sThumbLX == 0, "面板开着：全部挡住");
    padfilter::SetBlock(false);
    Pad(s, A); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == 0, "面板关了但 A 还按着：继续挡，免得漏给游戏");
    Pad(s, 0); padfilter::TestFilter(0, &s);
    Pad(s, A); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == A, "全部松开过之后：恢复正常");

    std::string info;
    XINPUT_STATE dummy;
    GameReadPad(0, &dummy);   // 让导入表项确实被用到
    // 先另外加载一个别的版本的 XInput（模拟我们自己的快捷键代码加载了 xinput1_4，
    // 而「游戏」导入的是另一个）：上一版就是栽在只认第一个找到的库上
    LoadLibraryW(L"xinput1_4.dll");
    LoadLibraryW(L"xinput9_1_0.dll");
    const bool pf = padfilter::Install(info);
    CHECK(pf, "在本进程的导入表里找到并替换 XInputGetState：%s", info.c_str());
    // 替换之后，「游戏」读手柄要经过过滤器：面板开着时读到的是全 0
    padfilter::SetBlock(true);
    XINPUT_STATE after{};
    const DWORD r = GameReadPad(0, &after);
    CHECK(r != ERROR_SUCCESS || after.Gamepad.wButtons == 0, "替换后游戏的调用经过过滤器（手柄%s接）", r == ERROR_SUCCESS ? "已" : "未");
    padfilter::SetBlock(false);

    // ---------------- 面板挂到 Present ----------------
    std::printf("== overlay\n");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"binq_test";
    RegisterClassExW(&wc);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"MHW", WS_OVERLAPPEDWINDOW, 0, 0, 1936, 1119, nullptr, nullptr, wc.hInstance, nullptr);
    // 手动模式（第二个参数 --ime，不在 build.sh 里跑）：用本机真实的输入法打几个字母，看拼音和候选词取不取得到。
    // 会把测试窗口切到前台几秒钟、往它身上发按键（不在前台就不发）。
    // 游戏的窗口线程上开着 TSF、输入法界面由程序自己画；测试窗口默认没有，这里照样开一个
    const bool realIme = argc > 2 && std::wstring(argv[2]) == L"--ime";
    if (realIme) {
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        static const CLSID kTfThreadMgr = {0x529a9e6b, 0x6587, 0x4f23, {0xab, 0x9e, 0x9c, 0x7d, 0x68, 0x3e, 0x3c, 0x50}};
        ITfThreadMgrEx* tm = nullptr;
        TfClientId cid = 0;
        HRESULT hr = ::CoCreateInstance(kTfThreadMgr, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITfThreadMgrEx), (void**)&tm);
        if (SUCCEEDED(hr) && tm) hr = tm->ActivateEx(&cid, TF_TMAE_UIELEMENTENABLEDONLY);
        std::printf("  ..   真输入法：给窗口线程开 TSF（界面自己画）hr=0x%08lx\n", (unsigned long)hr);
    }
    RECT rc; GetClientRect(hw, &rc);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Width = rc.right; sd.BufferDesc.Height = rc.bottom;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &g_dev, nullptr, &ctx);
    CHECK(SUCCEEDED(hr), "建「游戏」的交换链 %ldx%ld", rc.right, rc.bottom);

    std::string err;
    LoadConfig(L"config\\BetterINQ.ini", g_cfg, err);
    g_model.cfg = &g_cfg;
    g_model.ready = true;
    g_model.version = "0.2";
    overlay::Callbacks cb;
    cb.init = OnInit; cb.opened = OnOpened; cb.draw = OnDraw;
    CHECK(overlay::Install(cb, err), "挂 Present / ResizeBuffers %s", err.c_str());
    overlay::SetDebugAfterDraw(Save);

    auto frame = [&](float r, float g, float b) {
        ID3D11Texture2D* bb = nullptr;
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
        ID3D11RenderTargetView* rtv = nullptr;
        g_dev->CreateRenderTargetView(bb, nullptr, &rtv);
        const float col[4] = {r, g, b, 1};
        ctx->ClearRenderTargetView(rtv, col);
        rtv->Release(); bb->Release();
        sc->Present(0, 0);
    };

    for (int i = 0; i < 3; ++i) frame(0.25f, 0.35f, 0.25f);
    CHECK(g_draws == 0, "面板关着：一次都没画");
    // 不开面板就不碰游戏的 Present：交换链虚表里那一格还是系统原来的（别的模组启动时看到的是原样）
    void** const scVt = *reinterpret_cast<void***>(sc);
    void* const presentOrig = scVt[8];
    void* const resizeOrig = scVt[13];
    HMODULE presentMod = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(presentOrig), &presentMod);
    CHECK(presentMod && presentMod != ::GetModuleHandleW(nullptr), "面板没开过：Present 还是系统的（不在测试程序里）");
    auto pump = [] {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    };
    HIMC imc0 = ImmGetContext(hw);   // 窗口原来挂的输入法（系统没装输入法时是空的）
    if (imc0) ImmReleaseContext(hw, imc0);
    // 拿不到交换链虚表（游戏里出过一次：临时交换链建不出来）：这一次打不开，但不算整局失败，Present 也没动
    overlay::SetDebugVtableFail(3);
    overlay::SetOpen(true);
    CHECK(!overlay::IsOpen() && !overlay::Failed(), "拿不到虚表：这一次没打开，没判整局失败");
    CHECK(scVt[8] == presentOrig, "拿不到虚表：Present 没动");
    // 带窗口的办法不行时，改用不带窗口的办法：下面整段都是靠它拿到的虚表挂的钩子
    overlay::SetDebugVtableFail(1);
    overlay::SetOpen(true);
    CHECK(overlay::IsOpen() && scVt[8] != presentOrig, "再开一次：用不带窗口的办法拿到虚表，Present 挂上了");
    overlay::SetDebugVtableFail(0);
    for (int i = 0; i < 6; ++i) {
        if (i == 5) g_shot = out + L"\\overlay_1080p.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    CHECK(g_opened == 1 && g_draws == 6, "打开后每帧都画（opened=%d draws=%d）", g_opened, g_draws);
    // 输入法：面板开着时从窗口上摘掉（拼音字母直接进搜索框，不被候选框吃掉），关面板还回去
    pump();
    HIMC imcOpen = ImmGetContext(hw);
    CHECK(!imcOpen, "面板开着：游戏窗口上没挂输入法（原来%s）", imc0 ? "挂着" : "就没有");
    if (imcOpen) ImmReleaseContext(hw, imcOpen);

    // 猫饭改名：名字框打字时把输入法挂上，输入法打完的字（WM_IME_CHAR）进名字框；打完再摘掉
    if (!g_cfg.foods.empty()) {
        const int fid = g_cfg.foods.begin()->first;
        const std::string nameBefore = g_cfg.foods[fid].name;
        ui::DebugGo(5, fid);
        for (int i = 0; i < 4; ++i) frame(0.25f, 0.35f, 0.25f);
        ui::DebugFocusName();
        for (int i = 0; i < 3; ++i) frame(0.25f, 0.35f, 0.25f);
        pump();
        HIMC on = ImmGetContext(hw);
        CHECK(on || !imc0, "名字框在打字：游戏窗口挂上了输入法（系统%s输入法）", imc0 ? "有" : "没有");
        if (on) ImmReleaseContext(hw, on);

        // 拼音和候选词由面板自己画：塞一份假的，面板拿得到、画得出来
        overlay::SetDebugImeText(L"hei'long", {L"\u9ED1\u9F99", L"\u9ED1\u7B3C", L"\u9ED1\u9686", L"\u563F"}, 0);
        for (int i = 0; i < 3; ++i) {
            if (i == 2) g_shot = out + L"\\overlay_ime.png";
            frame(0.25f, 0.35f, 0.25f);
        }
        CHECK(g_model.imeComp == "hei'long" && g_model.imeCands.size() == 4 && g_model.imeCands[0] == "\xE9\xBB\x91\xE9\xBE\x99" &&
                  g_model.imeSel == 0,
              "输入法的拼音和 4 个候选词到了面板（%s / %zu 个）", g_model.imeComp.c_str(), g_model.imeCands.size());
        overlay::SetDebugImeText(L"", {}, -1);
        for (int i = 0; i < 2; ++i) frame(0.25f, 0.35f, 0.25f);
        CHECK(g_model.imeComp.empty() && g_model.imeCands.empty(), "清掉之后面板里也没了");

        // TSF 输入法给候选表的那条路：喂一个假的候选表对象，取出来的应当是当前页那 9 个
        {
            void* vt[15] = {
                reinterpret_cast<void*>(&FakeCandList::QI),   reinterpret_cast<void*>(&FakeCandList::Ref),
                reinterpret_cast<void*>(&FakeCandList::Ref),  reinterpret_cast<void*>(&FakeCandList::Nope),
                reinterpret_cast<void*>(&FakeCandList::Nope), reinterpret_cast<void*>(&FakeCandList::Nope),
                reinterpret_cast<void*>(&FakeCandList::Nope), reinterpret_cast<void*>(&FakeCandList::Nope),
                reinterpret_cast<void*>(&FakeCandList::Nope), reinterpret_cast<void*>(&FakeCandList::Count),
                reinterpret_cast<void*>(&FakeCandList::Sel),  reinterpret_cast<void*>(&FakeCandList::Str),
                reinterpret_cast<void*>(&FakeCandList::PageIdx), reinterpret_cast<void*>(&FakeCandList::Nope),
                reinterpret_cast<void*>(&FakeCandList::Page)};
            FakeCandList fake{vt};
            overlay::DebugReadCandidateElement(&fake);
            for (int i = 0; i < 2; ++i) frame(0.25f, 0.35f, 0.25f);
            CHECK(g_model.imeCands.size() == 9 && g_model.imeCands[0] == "\xE8\xAF\x8D" "9" &&
                      g_model.imeCands[8] == "\xE8\xAF\x8D" "17" && g_model.imeSel == 1,
                  "TSF 候选表：取到第 2 页的 9 个（%zu 个，第一个「%s」，选中第 %d 个；%s）", g_model.imeCands.size(),
                  g_model.imeCands.empty() ? "" : g_model.imeCands[0].c_str(), g_model.imeSel, overlay::DebugImeRaw().c_str());
            overlay::SetDebugImeText(L"", {}, -1);
            for (int i = 0; i < 2; ++i) frame(0.25f, 0.35f, 0.25f);
        }

        if (realIme) {
            ::ShowWindow(hw, SW_SHOWNORMAL);
            const HWND was = ::GetForegroundWindow();
            const DWORD fgTid = was ? ::GetWindowThreadProcessId(was, nullptr) : 0, me = ::GetCurrentThreadId();
            if (fgTid && fgTid != me) ::AttachThreadInput(fgTid, me, TRUE);
            ::SetForegroundWindow(hw);
            ::SetFocus(hw);
            if (fgTid && fgTid != me) ::AttachThreadInput(fgTid, me, FALSE);
            auto spin = [&](int n) {
                for (int i = 0; i < n; ++i) {
                    pump();
                    frame(0.25f, 0.35f, 0.25f);
                    ::Sleep(15);
                }
            };
            spin(20);
            const bool fg = ::GetForegroundWindow() == hw;
            std::printf("  ..   真输入法：测试窗口%s前台，键盘布局 %08llX\n", fg ? "在" : "没到",
                        (unsigned long long)(ULONG_PTR)::GetKeyboardLayout(0));
            if (fg) {
                if (HIMC c = ImmGetContext(hw)) {
                    DWORD conv = 0, sent = 0;
                    ImmSetOpenStatus(c, TRUE);
                    ImmGetConversionStatus(c, &conv, &sent);
                    ImmSetConversionStatus(c, conv | IME_CMODE_NATIVE, sent);
                    ImmReleaseContext(hw, c);
                }
                auto key = [&](WORD vk) {
                    if (::GetForegroundWindow() != hw) return false;   // 不在前台就不发，免得打到别的程序里
                    INPUT in[2] = {};
                    in[0].type = in[1].type = INPUT_KEYBOARD;
                    in[0].ki.wVk = in[1].ki.wVk = vk;
                    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    ::SendInput(2, in, sizeof(INPUT));
                    return true;
                };
                int sent = 0;
                for (const char* k = "HEILONG"; *k; ++k) {
                    if (!key((WORD)*k)) break;
                    ++sent;
                    spin(6);
                }
                spin(30);
                const overlay::ImeText r = overlay::GetImeText();
                std::string all;
                for (std::size_t i = 0; i < r.cands.size(); ++i) all += (i ? " | " : "") + r.cands[i];
                std::printf("  ..   真输入法：发了 %d 个键；拼音「%s」；候选 %zu 个（选中第 %d 个）：%s\n", sent, r.comp.c_str(),
                            r.cands.size(), r.sel, all.c_str());
                std::printf("  ..   真输入法：%s\n", overlay::DebugImeRaw().c_str());
                g_shot = out + L"\\overlay_ime_real.png";
                frame(0.25f, 0.35f, 0.25f);
                key(VK_ESCAPE);   // 取消这次组字
                spin(10);
            }
            // 关面板（会写一行 ime 诊断日志），窗口收起来。手动模式到此为止，后面的测试不跑
            overlay::SetOpen(false);
            for (int i = 0; i < 3; ++i) { frame(0.25f, 0.35f, 0.25f); pump(); }
            ::ShowWindow(hw, SW_HIDE);
            if (was) ::SetForegroundWindow(was);
            std::printf("\n手动模式结束（%d 项失败）\n", g_fail);
            return g_fail ? 1 : 0;
        }

        SendMessageW(hw, WM_IME_CHAR, 0x732B, 1);   // 猫
        SendMessageW(hw, WM_IME_CHAR, 0x996D, 1);   // 饭
        pump();                                      // 系统把它们转成 WM_CHAR 放进消息队列，游戏的消息循环会取
        for (int i = 0; i < 3; ++i) frame(0.25f, 0.35f, 0.25f);
        CHECK(g_cfg.foods[fid].name == "\xE7\x8C\xAB\xE9\xA5\xAD", "输入法送来的「猫饭」进了名字框（%s）",
              g_cfg.foods[fid].name.c_str());
        SendMessageW(hw, WM_KEYDOWN, VK_RETURN, 0x001C0001);
        SendMessageW(hw, WM_KEYUP, VK_RETURN, 0xC01C0001);
        for (int i = 0; i < 3; ++i) frame(0.25f, 0.35f, 0.25f);
        pump();
        HIMC off = ImmGetContext(hw);
        CHECK(!off, "回车打完：又把输入法摘掉了");
        if (off) ImmReleaseContext(hw, off);
        g_cfg.foods[fid].name = nameBefore;
        ui::DebugGo(0, 0);
        for (int i = 0; i < 4; ++i) frame(0.25f, 0.35f, 0.25f);
    }

    // 看样子用：设置页拉到底（快捷键那几格），以及从设置页切回主页途中的一帧
    ui::DebugGo(6, 0);
    for (int i = 0; i < 20; ++i) frame(0.25f, 0.35f, 0.25f);
    for (ImGuiWindow* w : GImGui->Windows)
        if (std::strstr(w->Name, "/##content_")) ImGui::SetScrollY(w, w->ScrollMax.y);
    for (int i = 0; i < 3; ++i) {
        if (i == 2) g_shot = out + L"\\overlay_settings.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    ui::DebugGo(3, 0);
    for (int i = 0; i < 30; ++i) {
        if (i == 29) g_shot = out + L"\\overlay_setedit.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    // 删除确认卡片
    ui::DebugOpenDelete();
    for (int i = 0; i < 4; ++i) {
        if (i == 3) g_shot = out + L"\\overlay_delete.png";
        ::Sleep(80);   // 背后压暗是 ImGui 按时间渐变的，等它到位再截
        frame(0.25f, 0.35f, 0.25f);
    }
    SendMessageW(hw, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
    SendMessageW(hw, WM_KEYUP, VK_ESCAPE, 0xC0010001);
    for (int i = 0; i < 3; ++i) frame(0.25f, 0.35f, 0.25f);
    ui::DebugGo(6, 0);
    for (int i = 0; i < 30; ++i) frame(0.25f, 0.35f, 0.25f);
    ui::DebugGo(0, 0);
    for (int i = 0; i < 4; ++i) {
        if (i == 3) g_shot = out + L"\\overlay_anim.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    for (int i = 0; i < 30; ++i) frame(0.25f, 0.35f, 0.25f);
    // 进程里还有别人建的 D3D11 交换链（别的模组、工具窗口）：同一张虚表，Present 也会走到钩子里，
    // 但面板只能画在游戏自己的交换链上
    {
        HWND hw2 = CreateWindowExW(0, wc.lpszClassName, L"other", WS_OVERLAPPEDWINDOW, 0, 0, 656, 519, nullptr, nullptr,
                                   wc.hInstance, nullptr);
        DXGI_SWAP_CHAIN_DESC sd2 = sd;
        RECT rc2; GetClientRect(hw2, &rc2);
        sd2.BufferDesc.Width = rc2.right; sd2.BufferDesc.Height = rc2.bottom; sd2.OutputWindow = hw2;
        IDXGISwapChain* sc2 = nullptr; ID3D11Device* dev2 = nullptr; ID3D11DeviceContext* ctx2 = nullptr;
        D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd2, &sc2,
                                      &dev2, nullptr, &ctx2);
        const int before = g_draws;
        for (int i = 0; i < 5; ++i) if (sc2) sc2->Present(0, 0);
        CHECK(sc2 && g_draws == before, "别人的交换链 Present 了 5 次：面板一次都没往上画（draws %d → %d）", before, g_draws);
        frame(0.25f, 0.35f, 0.25f);
        CHECK(g_draws == before + 1, "游戏自己的交换链照常画（draws=%d）", g_draws);
        if (sc2) sc2->Release();
        if (ctx2) ctx2->Release();
        if (dev2) dev2->Release();
        DestroyWindow(hw2);
    }
    const int drawsBefore = g_draws;

    // 改分辨率：游戏切窗口大小时会调 ResizeBuffers，之前的后台缓冲引用必须先放掉，否则会失败
    hr = sc->ResizeBuffers(0, 1280, 720, DXGI_FORMAT_UNKNOWN, 0);
    CHECK(SUCCEEDED(hr), "ResizeBuffers 成功（hr=%08lx）", (unsigned long)hr);
    for (int i = 0; i < 4; ++i) {
        if (i == 3) g_shot = out + L"\\overlay_720p.png";
        frame(0.30f, 0.30f, 0.40f);
    }
    CHECK(g_draws == drawsBefore + 4, "改分辨率后照常画（draws=%d）", g_draws);
    overlay::SetOpen(false);
    frame(0, 0, 0);
    CHECK(g_draws == drawsBefore + 4, "关掉后不再画");
    CHECK(scVt[8] == presentOrig && scVt[13] == resizeOrig, "关面板后过一帧：Present / ResizeBuffers 都还回去了");
    hr = sc->ResizeBuffers(0, 1600, 900, DXGI_FORMAT_UNKNOWN, 0);
    CHECK(SUCCEEDED(hr), "关着面板时游戏改分辨率：成功（手上没留后台缓冲，hr=%08lx）", (unsigned long)hr);
    for (int i = 0; i < 2; ++i) frame(0, 0, 0);
    overlay::SetOpen(true);
    CHECK(scVt[8] != presentOrig, "再打开：又挂上了");
    for (int i = 0; i < 3; ++i) frame(0.25f, 0.35f, 0.25f);
    CHECK(g_draws == drawsBefore + 7, "再打开照常画（draws=%d）", g_draws);
    // 关着的时候有别的模组接手了 Present（把我们当原函数存着）：不能还，否则把它的链弄断
    overlay::SetOpen(false);
    static void* s_otherOrig = nullptr;
    struct Other {
        static HRESULT WINAPI Present(IDXGISwapChain* s, UINT a, UINT b)
        {
            return reinterpret_cast<HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT)>(s_otherOrig)(s, a, b);
        }
    };
    {
        DWORD old = 0;
        ::VirtualProtect(&scVt[8], sizeof(void*), PAGE_READWRITE, &old);
        s_otherOrig = scVt[8];
        scVt[8] = reinterpret_cast<void*>(&Other::Present);
        ::VirtualProtect(&scVt[8], sizeof(void*), old, &old);
    }
    for (int i = 0; i < 3; ++i) frame(0, 0, 0);
    CHECK(scVt[8] == reinterpret_cast<void*>(&Other::Present), "Present 被别的模组接手了：我们不还，留在它的链上");
    CHECK(g_draws == drawsBefore + 7, "关着：照常不画");
    overlay::SetOpen(true);
    for (int i = 0; i < 2; ++i) frame(0.25f, 0.35f, 0.25f);
    CHECK(g_draws == drawsBefore + 9, "再打开：经它的链照样画（draws=%d）", g_draws);
    overlay::SetOpen(false);
    frame(0, 0, 0);
    pump();
    HIMC imc1 = ImmGetContext(hw);
    CHECK(imc1 == imc0, "关面板：输入法还回去了");
    if (imc1) ImmReleaseContext(hw, imc1);

    sc->Release(); ctx->Release(); g_dev->Release();
    DestroyWindow(hw);
    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
