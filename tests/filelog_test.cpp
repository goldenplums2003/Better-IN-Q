// 记音效文件（filelog）的测试：过滤器编进测试程序自己，被换掉的就是测试程序导入表里的
// CreateFileW / GetFileAttributesW，直接调就经过钩子，和游戏一样。用的都是不存在的路径，不碰真文件。
#include "filelog.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

static bool Has(const std::vector<std::string>& v, const std::string& s)
{
    for (const auto& x : v)
        if (x.find(s) != std::string::npos) return true;
    return false;
}

int main()
{
    // 装钩子之前别调这两个函数：编译器会把导入表里读到的地址留着复用，后面的调用就绕过钩子了
    //（下面用到它们，导入表里自然就有）
    std::string info;
    CHECK(filelog::Install(info), "Install：%s", info.c_str());
    filelog::Drain();

    HANDLE h = ::CreateFileW(L"nativePC\\sound\\wwise\\Windows\\em050_test_bt.nbnk", GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    CHECK(h == INVALID_HANDLE_VALUE, "不存在的文件照常打不开（原函数照常调）");
    ::GetFileAttributesW(L"nativePC\\em\\em050\\00\\mod\\em050_00.mod3");
    ::GetFileAttributesW(L"C:\\Windows\\win.ini");   // 和 nativePC、sound 都不沾：不记
    std::vector<std::string> out = filelog::Drain();
    for (const auto& s : out) std::printf("       %s\n", s.c_str());
    CHECK(Has(out, "em050_test_bt.nbnk") && Has(out, "打开（没有）"), "路径带 sound 的：逐条记下（哪个调用、没打开成）");
    CHECK(!Has(out, "em050_00.mod3") && !Has(out, "win.ini"), "别的文件不逐条记");

    ::Sleep(1100);
    ::GetFileAttributesW(L"nativePC\\em\\em050\\00\\mod\\em050_00.mrl3");
    out = filelog::Drain();
    for (const auto& s : out) std::printf("       %s\n", s.c_str());
    CHECK(Has(out, "别的 nativePC 查询 2 次"), "别的 nativePC 查询：攒满一秒报一次次数");

    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
