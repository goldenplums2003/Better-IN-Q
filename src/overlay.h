#pragma once
// ============================================================================
//  overlay.h —— 把面板画到游戏画面上（DX11）
//
//  D3D11 和 D3D12 都支持（游戏的「DirectX 12」开关开不开都能用）。
//  挂的是交换链虚表里的 Present / ResizeBuffers 两项（同一个类的交换链共用一张虚表，
//  拿临时建的交换链就能找到它）。不改函数开头的字节：Steam 叠加层之类挂了 Present
//  函数本身的话，我们调用原指针时自然经过它们，大家串在一起互不干扰。
//  面板关着的时候 Present 里什么都不做，零开销。
// ============================================================================
#include <string>

namespace overlay {

struct Callbacks {
    void (*init)(float scale) = nullptr;           // ImGui 上下文建好之后（渲染线程）
    void (*opened)() = nullptr;                    // 面板刚打开（渲染线程）
    void (*draw)(float w, float h) = nullptr;      // 每帧（渲染线程，面板开着时）
};

bool Install(const Callbacks& cb, std::string& err);
void SetOpen(bool open);
// 面板开着时游戏窗口上默认不挂输入法（拼音字母直接进搜索框）；want = 正在打名字，要挂上
void SetImeWanted(bool want);
bool IsOpen();
bool Failed();   // 渲染端初始化失败（比如游戏开的是 DX12），面板不可用；这时 IsOpen() 一直是 false

// 测试用：每帧画完、交给游戏 Present 之前回调一次，参数是 ID3D11Texture2D*（后台缓冲）
void SetDebugAfterDraw(void (*fn)(void* backbuffer));
// 同上，D3D12：参数是 ID3D12Resource*（后台缓冲，处于 PRESENT 状态）和 ID3D12CommandQueue*（游戏的队列）
void SetDebugAfterDraw12(void (*fn)(void* backbuffer, void* queue));

} // namespace overlay
