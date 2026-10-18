#!/bin/sh
# 用 w64devkit (MinGW-w64) 构建。  sh build.sh [dll|test|all]   默认 all
set -e
export PATH="/d/w64devkit/bin:$PATH"
cd "$(dirname "$0")"
mkdir -p out/obj
WHAT="${1:-all}"
COMMON="-O2 -std=c++17 -static -static-libgcc -static-libstdc++ -Wall -Wno-unused-function"
IM=third_party/imgui

# ImGui 本体和两个后端只在源码变了时重编
imgui_objs() {
    for f in imgui imgui_draw imgui_tables imgui_widgets; do
        [ out/obj/$f.o -nt $IM/$f.cpp ] || g++ -c -O2 -std=c++17 -I$IM -o out/obj/$f.o $IM/$f.cpp
    done
    for f in imgui_impl_dx11 imgui_impl_dx12 imgui_impl_win32; do
        # WIDL_EXPLICIT_AGGREGATE_RETURNS：让 MinGW 的 d3d12.h 按 COM 的规矩返回结构体（见 src/overlay.cpp 开头）
        [ out/obj/$f.o -nt $IM/backends/$f.cpp ] || g++ -c -O2 -std=c++17 -DWIDL_EXPLICIT_AGGREGATE_RETURNS -I$IM             -o out/obj/$f.o $IM/backends/$f.cpp
    done
}

if [ "$WHAT" = "dll" ] || [ "$WHAT" = "all" ]; then
    echo "==> BetterINQ.dll"
    imgui_objs
    g++ -shared $COMMON -s -I$IM -o out/BetterINQ.dll \
        src/main.cpp src/inq.cpp src/hook.cpp src/config.cpp src/input.cpp src/log.cpp \
        src/ui.cpp src/gamedata.cpp src/overlay.cpp src/padfilter.cpp src/cursorfilter.cpp src/kbmfilter.cpp \
        out/obj/imgui.o out/obj/imgui_draw.o out/obj/imgui_tables.o out/obj/imgui_widgets.o \
        out/obj/imgui_impl_dx11.o out/obj/imgui_impl_dx12.o out/obj/imgui_impl_win32.o \
        -ld3d11 -ld3dcompiler -ldxgi -ldwmapi -limm32 -lgdi32 -lole32 -luuid -ldxguid
    ls -l out/BetterINQ.dll
fi

if [ "$WHAT" = "test" ] || [ "$WHAT" = "all" ]; then
    echo "==> tests"
    g++ $COMMON -o out/hook_test.exe tests/hook_test.cpp src/hook.cpp
    g++ $COMMON -o out/config_test.exe tests/config_test.cpp src/config.cpp src/input.cpp
    g++ $COMMON -Isrc -o out/cursor_test.exe tests/cursor_test.cpp src/cursorfilter.cpp src/hook.cpp src/log.cpp
    g++ $COMMON -Isrc -o out/kbm_test.exe tests/kbm_test.cpp src/kbmfilter.cpp src/hook.cpp src/log.cpp -ldxguid
    imgui_objs
    g++ $COMMON -municode -I$IM -Isrc -o out/overlay_test.exe tests/overlay_test.cpp \
        src/overlay.cpp src/padfilter.cpp src/ui.cpp src/gamedata.cpp src/config.cpp src/input.cpp src/log.cpp src/hook.cpp \
        out/obj/imgui.o out/obj/imgui_draw.o out/obj/imgui_tables.o out/obj/imgui_widgets.o \
        out/obj/imgui_impl_dx11.o out/obj/imgui_impl_dx12.o out/obj/imgui_impl_win32.o \
        -ld3d11 -ld3dcompiler -ldxgi -ldwmapi -limm32 -lgdi32 -lole32 -lwindowscodecs -luuid -lxinput
    g++ $COMMON -municode -I$IM -Isrc -o out/overlay_dx12_test.exe tests/overlay_dx12_test.cpp \
        src/overlay.cpp src/ui.cpp src/gamedata.cpp src/config.cpp src/input.cpp src/log.cpp \
        out/obj/imgui.o out/obj/imgui_draw.o out/obj/imgui_tables.o out/obj/imgui_widgets.o \
        out/obj/imgui_impl_dx11.o out/obj/imgui_impl_dx12.o out/obj/imgui_impl_win32.o \
        -ld3d11 -ld3d12 -ld3dcompiler -ldxgi -ldwmapi -limm32 -lgdi32 -lole32 -lwindowscodecs -luuid
    g++ $COMMON -I$IM -Isrc -o out/ui_nav_test.exe tests/ui_nav_test.cpp src/ui.cpp src/gamedata.cpp src/config.cpp src/input.cpp \
        out/obj/imgui.o out/obj/imgui_draw.o out/obj/imgui_tables.o out/obj/imgui_widgets.o out/obj/imgui_impl_dx11.o \
        -ld3d11 -ld3dcompiler
    ./out/hook_test.exe
    ./out/config_test.exe
    ./out/cursor_test.exe
    ./out/kbm_test.exe
    ./out/overlay_test.exe "$(cygpath -w "$PWD/out")"
    ./out/overlay_dx12_test.exe "$(cygpath -w "$PWD/out")"
    ./out/ui_nav_test.exe
fi
