# -*- coding: utf-8 -*-
# 打发布包（狩技盒子拖进去就能装，也可以直接解压到游戏根目录）。
#   py -3 tools/make_zip.py [输出目录]
#
# zip 根目录是 nativePC\，文件名就是盒子里显示的 mod 名。
# 不带配置文件：插件第一次运行自己生成 BetterINQ.ini，这样它不归盒子管，
# 以后用盒子更新/卸载都不会冲掉玩家改过的设置。
import hashlib, os, sys, zipfile

VER = 'v0.6.7'
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'out')
ZIP = os.path.join(OUT_DIR, 'Better-IN-Q %s.zip' % VER)

PLUG = 'nativePC/plugins/'
DATA = PLUG + 'BetterINQ/'


def text_for_windows(path):
    # 说明给人看：转成 CRLF、加 UTF-8 BOM，老版记事本打开也不乱码
    s = open(path, 'r', encoding='utf-8').read().replace('\r\n', '\n').replace('\n', '\r\n')
    return b'\xef\xbb\xbf' + s.encode('utf-8')


FILES = [
    # (zip 里的路径, 内容)
    (PLUG + 'BetterINQ.dll', open(os.path.join(ROOT, 'out', 'BetterINQ.dll'), 'rb').read()),
    (DATA + '说明.txt', text_for_windows(os.path.join(ROOT, 'dist', '说明.txt'))),
    (DATA + '第三方许可-Dear ImGui.txt', text_for_windows(os.path.join(ROOT, 'third_party', 'imgui', 'LICENSE.txt'))),
]

os.makedirs(OUT_DIR, exist_ok=True)
if os.path.exists(ZIP):
    os.remove(ZIP)
with zipfile.ZipFile(ZIP, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name, data in FILES:
        z.writestr(name, data)

# ---- 回读校验 ----
bad = 0
with zipfile.ZipFile(ZIP) as z:
    names = z.namelist()
    print('%s  %.2f MB' % (os.path.basename(ZIP), os.path.getsize(ZIP) / 1048576.0))
    for n in names:
        print('  %-48s %9d' % (n, z.getinfo(n).file_size))
    for name, data in FILES:
        if name not in names:
            print('!! 少了 %s' % name); bad += 1
        elif hashlib.sha256(z.read(name)).digest() != hashlib.sha256(data).digest():
            print('!! 内容对不上 %s' % name); bad += 1
    if any(n.lower().endswith('.ini') for n in names):
        print('!! 混进了配置文件'); bad += 1
    if z.testzip() is not None:
        print('!! zip 自检失败'); bad += 1
    dll = z.read(PLUG + 'BetterINQ.dll')
    if VER[1:].encode() not in dll or 'Better-IN-Q'.encode('utf-8') not in dll:
        print('!! DLL 里找不到版本号 %s，是不是没重新编译' % VER[1:]); bad += 1
    doc = z.read(DATA + '说明.txt').decode('utf-8-sig')
    if VER not in doc.splitlines()[0]:
        print('!! 说明.txt 标题不是 %s' % VER); bad += 1
    if "Stracker's Loader" not in doc:
        print('!! 说明里没写前置'); bad += 1

print('\n%s' % ('有问题' if bad else '打包完成，内容逐个校验通过'))
sys.exit(1 if bad else 0)
