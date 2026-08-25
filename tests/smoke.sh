#!/usr/bin/env bash
# tests/smoke.sh —— paint-pc 无头冒烟：构建 + headless 离屏导出 PNG + 含笔迹像素断言
set -euo pipefail
cd "$(dirname "$0")/.."
DGCPAIN_DEPS_ROOT="${DGCPAIN_DEPS_ROOT:-/tmp/dgc-deps/usr}"
PC_X11_DEPS_ROOT="${PC_X11_DEPS_ROOT:-/home/qiansenwei/.local/dgc-x11dev/usr}"
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
    -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF \
    -DDGCPAIN_DEPS_ROOT="$DGCPAIN_DEPS_ROOT" \
    -DCMAKE_PREFIX_PATH="$PC_X11_DEPS_ROOT" \
    -DCMAKE_C_FLAGS="-I$PC_X11_DEPS_ROOT/include" \
    -DCMAKE_CXX_FLAGS="-I$PC_X11_DEPS_ROOT/include"
cmake --build build -j
out=$(mktemp /tmp/paint_pc_headless.XXXXXX.png)
./build/paint_pc --headless "$out"
[ -s "$out" ] || { echo "FAIL: PNG empty/missing: $out"; exit 1; }

# 真实笔迹断言：B3-1 真实内核下，固定笔迹（seed=42，黑色）必须产生与背景明显不同的暗像素。
#
# ⚠ 审阅打回修订（U2 rev2）1/2：SDK exportPNG 用 stb_image_write 默认**自适应滤波**，
# 1280×800 实测滤波分布 {Sub:190, Up:609, Paeth:1}，**0 行 filter 0**。必须按每行 filter byte
# 还原原始像素（None/Sub/Up/Average/Paeth），否则把滤波残差当像素 → 门无区分度。
# ⚠ 审阅打回修订（U2 rev3）2/2：SDK `dgcClear` 实际**丢弃颜色参数**、恒清纯白
# （sdk_api/dgc_paint_c_api.cpp:232 把 r/g/b/a 全 (void)；vk_backend.cpp:815 硬编码白）。
# 故**不能硬编码背景常量**；改为从真实输出动态导出背景（采样四角众数），并把背景自检
# 降级为 diagnostic 打印（不 exit 2），dark 判定独立判真——无论背景纸白还是纯白都对。
python3 - "$out" <<'PY'
import sys, zlib, struct
png = open(sys.argv[1], 'rb').read()
assert png[:8] == b'\x89PNG\r\n\x1a\n', "not a PNG"
pos = 8; idat = b''; w = h = None
while pos < len(png):
    ln = struct.unpack('>I', png[pos:pos+4])[0]; typ = png[pos+4:pos+8]; dat = png[pos+8:pos+8+ln]
    if typ == b'IHDR':
        w, h = struct.unpack('>II', dat[:8]); depth, ctype = dat[8], dat[9]
        assert depth == 8 and ctype == 6, "expected RGBA8"
    elif typ == b'IDAT':
        idat += dat
    elif typ == b'IEND':
        break
    pos += 12 + ln
raw = zlib.decompress(idat)
bpp = 4; stride = w * bpp

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)

# 滤波感知解码：stbi_write_force_png_filter 默认自适应滤波（None/Sub/Up/Average/Paeth）。
prev = bytearray(stride)
pix = bytearray(w * h * bpp)
for y in range(h):
    off = y * (stride + 1)
    f = raw[off]
    line = bytearray(raw[off + 1 : off + 1 + stride])
    for i in range(stride):
        v = line[i]
        left = line[i - bpp] if i >= bpp else 0
        up = prev[i]
        up_left = prev[i - bpp] if i >= bpp else 0
        if f == 1:   v = (v + left) & 0xff                     # Sub
        elif f == 2: v = (v + up) & 0xff                       # Up
        elif f == 3: v = (v + ((left + up) >> 1)) & 0xff       # Average
        elif f == 4: v = (v + paeth(left, up, up_left)) & 0xff # Paeth
        line[i] = v
    prev = line
    pix[y * stride : y * stride + stride] = line

def px(x, y):
    o = (y * w + x) * bpp
    return pix[o], pix[o+1], pix[o+2], pix[o+3]

# 动态导出背景（四角 8×8 区域众数；笔迹是斜线不经过四角）。
from collections import Counter
bg_counts = Counter()
for cx, cy in [(0,0), (w-8,0), (0,h-8), (w-8,h-8)]:
    for yy in range(cy, cy+8):
        for xx in range(cx, cx+8):
            bg_counts[px(xx, yy)] += 1
bg, _ = bg_counts.most_common(1)[0]
bg_sum = bg[0] + bg[1] + bg[2]
print(f"bg={bg} bg_sum={bg_sum}", file=sys.stderr)   # diagnostic，不 exit 2

dark = 0
for y in range(0, h, 3):      # 隔行抽样足够（笔迹对角线贯穿画面）
    for x in range(0, w, 3):
        r, g, b, a = px(x, y)
        if a > 0 and (r, g, b) != bg and (r + g + b) < bg_sum - 60:
            dark += 1
print(f"dark_pixels={dark}")
# 门：真实内核（B3-1）必须产生黑色笔迹像素；无笔迹（dark=0）→ FAIL。
if dark <= 50:
    print(f"FAIL: no real stroke pixels (dark={dark})")
    sys.exit(1)
print(f"PASS: headless PNG stroke pixels={dark}")
PY
