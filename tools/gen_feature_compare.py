#!/usr/bin/env python3
"""把 ppet_feature_verify 的四状态截图拼成全景对比图与局部放大对比图。

用法：
    python3 tools/gen_feature_compare.py <out_dir> <prefix>

读取 <out_dir>/<prefix>_report.json 与各状态 PNG，生成：
    <prefix>_compare.png            2x2 全景
    <prefix>_detail_expression.png  中性 vs 表情（变化区域放大）
    <prefix>_detail_lipsync.png     中性 vs 口型（变化区域放大）
    <prefix>_detail_action.png      中性 vs 动作（变化区域放大）
    <prefix>_diff_<state>.png       中性 / 状态 / 变化像素红色高亮（三格放大）
    <prefix>_diffmap.png            四状态相对中性的全局差异掩膜（2x2）

【为什么需要差异高亮】
   表情参数只驱动眉毛 / 笑眼这类极小区域，全脸放大后两张图肉眼几乎一样。
   把「差异 > 8 的像素」直接染红，才能一眼确认变化确实落在眼睛/眉毛/嘴部，
   而不是靠像素百分比去猜。
"""
import json
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFont

STATES = ["neutral", "expression", "lipsync", "action"]


def font(size):
    for path in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    ):
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                pass
    return ImageFont.load_default()


def on_white(im):
    bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
    bg.alpha_composite(im.convert("RGBA"))
    return bg.convert("RGB")


def densest_region(neutral, state):
    """找出两张图差异最密集的 128x128 区域（用于把放大框聚焦在嘴/眉毛上）。

    直接用变化像素外接框会把头发、裙摆的间接变化也框进来，
    真正想对比的嘴部只占中间一小块，放大后仍然看不清。
    """
    w, h = neutral.size
    diff = ImageChops.difference(on_white(neutral), on_white(state)).convert("L")
    block = 16
    bw, bh = (w + block - 1) // block, (h + block - 1) // block
    scores = [[0] * bw for _ in range(bh)]
    small = diff
    px = small.load()
    for by in range(bh):
        for bx in range(bw):
            total = 0
            for y in range(by * block, min((by + 1) * block, h)):
                for x in range(bx * block, min((bx + 1) * block, w)):
                    total += px[x, y]
            scores[by][bx] = total
    best = (0, 0, 0)
    for by in range(bh):
        for bx in range(bw):
            # 3x3 块窗口（约 48px）求和，避免单块噪声
            s = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    yy, xx = by + dy, bx + dx
                    if 0 <= yy < bh and 0 <= xx < bw:
                        s += scores[yy][xx]
            if s > best[0]:
                best = (s, bx * block, by * block)
    size = 128
    cx, cy = best[1] + block // 2, best[2] + block // 2
    x0, y0 = max(0, cx - size // 2), max(0, cy - size // 2)
    x1, y1 = min(w, x0 + size), min(h, y0 + size)
    return (max(0, x1 - size), max(0, y1 - size), x1, y1)


def diff_overlay(neutral, state, threshold=8):
    """返回 (差异掩膜 L, 在状态图上叠加红色高亮的 RGB)。

    threshold 与 ppet_feature_verify.cpp 的 Compare() 保持一致（任一通道差 > 8）。
    """
    a = on_white(neutral).convert("RGB")
    b = on_white(state).convert("RGB")
    diff = ImageChops.difference(a, b)
    r, g, bl = diff.split()
    mx = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    mask = mx.point(lambda v: 255 if v > threshold else 0)
    red = Image.new("RGBA", b.size, (230, 40, 50, 0))
    red.putalpha(mask.point(lambda v: int(v * 0.80)))
    overlay = Image.alpha_composite(b.convert("RGBA"), red).convert("RGB")
    return mask, overlay


def crop_for(comparisons, imgs, state, w, h):
    """取该状态用于放大的裁剪框（与 _detail_*.png 保持同一套逻辑）。"""
    if state in ("expression", "lipsync"):
        crop = densest_region(imgs["neutral"], imgs[state])
    else:
        bbox = comparisons.get(state, {}).get("change_bbox")
        if not bbox or len(bbox) != 4 or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            return None
        pad = 28
        crop = (max(0, bbox[0] - pad), max(0, bbox[1] - pad),
                min(w, bbox[2] + pad + 1), min(h, bbox[3] + pad + 1))
    x0, y0, x1, y1 = crop
    if x1 - x0 < 90:
        cx = (x0 + x1) // 2
        x0, x1 = max(0, cx - 60), min(w, cx + 60)
    if y1 - y0 < 90:
        cy = (y0 + y1) // 2
        y0, y1 = max(0, cy - 60), min(h, cy + 60)
    return (x0, y0, x1, y1)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    out_dir, prefix = sys.argv[1], sys.argv[2]
    with open(os.path.join(out_dir, prefix + "_report.json"), encoding="utf-8") as f:
        report = json.load(f)

    imgs = {s: Image.open(os.path.join(out_dir, f"{prefix}_{s}.png")) for s in STATES}
    w, h = imgs["neutral"].size
    label_h, gap = 34, 8

    # --- 2x2 全景 ---
    canvas = Image.new("RGB", (w * 2 + gap, (h + label_h) * 2 + gap), (238, 238, 238))
    draw = ImageDraw.Draw(canvas)
    f = font(18)
    for i, s in enumerate(STATES):
        x = (i % 2) * (w + gap)
        y = (i // 2) * (h + label_h + gap)
        draw.text((x + 10, y + 7), f"{prefix.upper()} / {s}", fill=(20, 20, 20), font=f)
        canvas.paste(on_white(imgs[s]), (x, y + label_h))
    canvas.save(os.path.join(out_dir, prefix + "_compare.png"))

    # --- 局部放大：用报告里的变化区域外接框裁剪 ---
    comparisons = report.get("comparisons", {})
    for state in ("expression", "lipsync", "action"):
        if state in ("expression", "lipsync"):
            crop = densest_region(imgs["neutral"], imgs[state])
        else:
            bbox = comparisons.get(state, {}).get("change_bbox")
            if not bbox or len(bbox) != 4 or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
                continue
            pad = 28
            crop = (max(0, bbox[0] - pad), max(0, bbox[1] - pad),
                    min(w, bbox[2] + pad + 1), min(h, bbox[3] + pad + 1))
        x0, y0, x1, y1 = crop
        # 变化区域太小时（例如只有眉毛），强制给一个最小的可读窗口。
        if x1 - x0 < 90:
            cx = (x0 + x1) // 2
            x0, x1 = max(0, cx - 60), min(w, cx + 60)
        if y1 - y0 < 90:
            cy = (y0 + y1) // 2
            y0, y1 = max(0, cy - 60), min(h, cy + 60)
        crop = (x0, y0, x1, y1)
        scale = max(2, min(6, int(360 / max(1, x1 - x0))))
        cw, ch = (x1 - x0) * scale, (y1 - y0) * scale
        detail = Image.new("RGB", (cw * 2 + gap, ch + label_h + 4), (238, 238, 238))
        d = ImageDraw.Draw(detail)
        d.text((8, 7), f"{prefix.upper()} / neutral (x{scale})", fill=(20, 20, 20), font=f)
        d.text((cw + gap + 8, 7), f"{prefix.upper()} / {state} (x{scale})",
               fill=(20, 20, 20), font=f)
        for idx, s in enumerate(("neutral", state)):
            part = on_white(imgs[s].crop(crop)).resize((cw, ch), Image.LANCZOS)
            detail.paste(part, (idx * (cw + gap), label_h))
        path = os.path.join(out_dir, f"{prefix}_detail_{state}.png")
        detail.save(path)
        changed = comparisons[state].get("changed_percent")
        params = comparisons[state].get("param_changed_count", 0)
        print(f"{path}  bbox={crop} scale=x{scale} "
              f"pixels={changed}% param_changed={params}")

    # --- 差异高亮：中性 / 状态 / 变化像素染红 ---
    for state in ("expression", "lipsync", "action"):
        crop = crop_for(comparisons, imgs, state, w, h)
        if crop is None:
            continue
        x0, y0, x1, y1 = crop
        scale = max(2, min(6, int(300 / max(1, x1 - x0))))
        cw, ch = (x1 - x0) * scale, (y1 - y0) * scale
        _, overlay = diff_overlay(imgs["neutral"], imgs[state])
        cells = [
            ("neutral", on_white(imgs["neutral"].crop(crop))),
            (state, on_white(imgs[state].crop(crop))),
            ("changed pixels", overlay.crop(crop)),
        ]
        canvas = Image.new("RGB", (cw * 3 + gap * 2, ch + label_h + 4), (238, 238, 238))
        d = ImageDraw.Draw(canvas)
        for i, (name, part) in enumerate(cells):
            d.text((i * (cw + gap) + 8, 7), f"{prefix.upper()} / {name} (x{scale})",
                   fill=(20, 20, 20), font=f)
            canvas.paste(part.resize((cw, ch), Image.LANCZOS), (i * (cw + gap), label_h))
        path = os.path.join(out_dir, f"{prefix}_diff_{state}.png")
        canvas.save(path)
        changed = comparisons.get(state, {}).get("changed_percent", 0)
        print(f"{path}  bbox={crop} scale=x{scale} pixels={changed}%")

    # --- 全局差异掩膜 2x2：一眼看出每个状态的变化落在画面哪里 ---
    dmap = Image.new("RGB", (w * 2 + gap, (h + label_h) * 2 + gap), (238, 238, 238))
    dd = ImageDraw.Draw(dmap)
    for i, s in enumerate(STATES):
        x = (i % 2) * (w + gap)
        y = (i // 2) * (h + label_h + gap)
        if s == "neutral":
            part = on_white(imgs[s])
            name = "neutral (baseline)"
        else:
            mask, _ = diff_overlay(imgs["neutral"], imgs[s])
            part = Image.new("RGB", (w, h), (250, 250, 250))
            part.paste((225, 45, 55), (0, 0), mask)
            name = "diff vs neutral"
        dd.text((x + 10, y + 7), f"{prefix.upper()} / {s} / {name}",
                fill=(20, 20, 20), font=f)
        dmap.paste(part, (x, y + label_h))
    dmap_path = os.path.join(out_dir, prefix + "_diffmap.png")
    dmap.save(dmap_path)
    print(dmap_path)

    print(os.path.join(out_dir, prefix + "_compare.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
