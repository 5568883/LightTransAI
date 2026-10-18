# 轻译 AI —— 应用图标生成脚本
#
# 设计：品牌蓝圆角方块 + 白色「译」字。
# 与托盘图标、悬浮按钮使用同一套品牌色（#2E6FF2），保证视觉一致。
#
# 用法：python make_icon.py
# 产物：app.ico（多尺寸，供 exe 资源与窗口图标使用）

from PIL import Image, ImageDraw, ImageFont

BRAND = (0x2E, 0x6F, 0xF2, 255)      # 品牌蓝
WHITE = (255, 255, 255, 255)

# 需要打包进 .ico 的尺寸。
# 16/20/24 用于任务栏与资源管理器的小图标，256 用于大图标视图。
SIZES = [256, 128, 64, 48, 32, 24, 20, 16]

# 优先用粗体微软雅黑；取不到就退回黑体，保证任何机器上都能生成。
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
]


def load_font(px):
    for path in FONT_CANDIDATES:
        try:
            return ImageFont.truetype(path, px)
        except OSError:
            continue
    return ImageFont.load_default()


def render(size):
    # 4 倍超采样后缩放，让圆角与文字边缘平滑（Pillow 无抗锯齿的绘制路径）
    ss = 4
    S = size * ss
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # 圆角方块。圆角半径取边长的 22%，接近 Windows 11 应用图标的观感。
    radius = int(S * 0.22)
    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=radius, fill=BRAND)

    # 白色「译」字，居中。
    # 字号取边长的 62%，是「单字充满圆角方块」的常用比例；
    # 再按实际墨迹包围盒微调位置，避免不同字体的基线差异导致偏心。
    font = load_font(int(S * 0.62))
    text = "译"
    bbox = d.textbbox((0, 0), text, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (S - tw) / 2 - bbox[0]
    y = (S - th) / 2 - bbox[1]
    d.text((x, y), text, font=font, fill=WHITE)

    return img.resize((size, size), Image.LANCZOS)


def main():
    # 以最大尺寸为基准生成各尺寸，再保存为单文件多帧 .ico
    frames = [render(s) for s in SIZES]
    frames[0].save(
        "app.ico",
        format="ICO",
        sizes=[(s, s) for s in SIZES],
        append_images=frames[1:],
    )
    print("app.ico 已生成，尺寸:", SIZES)

    # 顺带导出一张 PNG 便于预览
    render(256).save("app_icon_preview.png")
    print("app_icon_preview.png 已生成（256x256 预览）")


if __name__ == "__main__":
    main()
