# Better-IN-Q

《怪物猎人：世界 冰原》（PC 版 15.23.00）的快速重开任务插件 —— IN-Q 的游戏内置版。

由 [Stracker's Loader](https://www.nexusmods.com/monsterhunterworld/mods/1982) 加载，开游戏就在，不用另外开软件；
游戏里按快捷键呼出面板，手柄全程可操作。

## 致谢

**快速重开的核心逻辑来自 IN-Q 原作者 年糕狗三世**（B 站视频 BV1yz2PBNEWn），原作者允许修改后重新分发。
移植依据的是 QCanon 中文化与修改的 CheatEngine 版（[QCAY1145/AutoCE-INQ-For-MHW](https://github.com/QCAY1145/AutoCE-INQ-For-MHW)）。

Better-IN-Q 只在界面、bug 修复和使用体验上做了优化，没有参与核心逻辑的开发。请大家支持原作者。

## 功能

- 一键重开当前任务（默认手柄 LB+RB+LT+RT），可选直接重开 / 重置后回据点再接 / 只接任务不出发
- 重开时自动套用「全套」：装备套装、道具套装、猫饭、任务
- 游戏内面板（默认 视图键+菜单键，或键盘 Insert）：换任务（支持拼音 / 首字母搜索）、换套装、换猫饭、设置
- 面板开着时游戏收不到手柄和键鼠输入（窗口消息、DirectInput、按键查询都挡），鼠标可以自由点面板；输入法暂时关掉，直接打拼音
- 相比原版 IN-Q：做成插件，不再有「开游戏检测不到」；修了道具套装排过序后套错的问题；修了黑龙开局脱衣装

## 安装（玩家）

1. 先装好 Stracker's Loader（游戏根目录要有 `dinput8.dll` 和 `loader.dll`）。
2. 下载 Release 里的 `Better-IN-Q vX.Y.Z.zip`，用狩技 MOD 盒子安装，或者直接解压到游戏根目录。
3. 进游戏，回到据点时聊天框出现「Better-IN-Q x.y.z 已就绪」就装好了。

设置会自动存到 `nativePC\plugins\BetterINQ\BetterINQ.ini`，日志在同目录的 `BetterINQ.log`。
不要和 IN-Q.exe 同时开。
游戏的 DirectX 12 开不开都能用：面板支持 DX11 和 DX12。

## 构建

需要 [w64devkit](https://github.com/skeeto/w64devkit)（MinGW-w64，静态链接），`build.sh` 里默认路径是 `D:\w64devkit`。

```sh
sh build.sh all        # 编译 out/BetterINQ.dll 并跑全部测试
sh install.sh          # 装进游戏目录（游戏开着时会拒绝）
py -3 tools/make_zip.py   # 打发布包
```

`src/quests.inc` 是 `tools/gen_quests.py` 从上面那份 INQ.CT 生成的（含拼音，用 Windows 自带的 ICU）。
重新生成时把 CT 放在仓库根目录，命名为 `reference_INQ_QCanon.CT`。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/main.cpp` | 插件入口、主循环、快捷键 |
| `src/inq.*` | 重开的状态机和游戏挂钩（移植自 INQ.CT） |
| `src/hook.*` | 往游戏代码里插跳转、按值换函数指针 |
| `src/overlay.*` | 挂 DXGI Present，用 Dear ImGui 画面板（D3D11 / D3D12） |
| `src/ui.*` | 面板界面 |
| `src/padfilter.*` / `src/kbmfilter.*` / `src/cursorfilter.*` | 面板开着时挡住游戏的手柄、键盘鼠标输入，不让游戏拽鼠标 |
| `src/config.*` / `src/input.*` / `src/gamedata.*` | 配置文件、快捷键、任务表 |
| `tests/` | 单元测试（`build.sh all` 会跑） |
| `third_party/imgui/` | [Dear ImGui](https://github.com/ocornut/imgui)（MIT 许可，见其中的 LICENSE.txt） |

## 许可证

[MIT](LICENSE)。`third_party/imgui/` 是 Dear ImGui，按它自己的 MIT 许可（版权归其作者）。
