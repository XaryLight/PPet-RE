#!/usr/bin/env python3
"""生成 ZIP 模型包测试数据。

用 Python 标准库 zipfile 打包一个最小但结构完整的模型包：
    model_package/
        model.pmx        最小可解析的 PMX 2.0
        texture.png      1x1 贴图
        readme.txt       无关文件（用于验证不影响模型定位）

不需要任何第三方依赖。
"""
import struct
import sys
import zlib
from pathlib import Path


def build_pmx() -> bytes:
    """构造一个最小的 PMX 2.0 文件（头 + 极少数据）。

    结构参考 PMX 2.0 规范：
        头: "PMX " + 版本(1.0) + 全局字节数(1) + [浮点精度(4), 颜色精度(4),
            带权UV精度(4), 顶点索引尺寸(1), 贴图索引尺寸(1), 材质索引尺寸(1),
            骨骼索引尺寸(1), 变形索引尺寸(1), 刚体索引尺寸(1)]
        名称: 编码(1) + 名称(UTF-16LE)
        ...
    这里只填到「顶点」之前就足够让解析器产出可读结果，
    实际测试关注的是 ZIP 解压与模型定位，而非 PMX 语义。
    """
    out = bytearray()
    out += b"PMX "
    out += bytes([0])  # 版本 2.0
    out += bytes([1])  # 全局信息字节数
    out += struct.pack("<f", 1.0)   # 浮点精度
    out += struct.pack("<f", 1.0)   # 颜色精度
    out += struct.pack("<f", 1.0)   # 带权 UV 精度
    out += bytes([1, 1, 1, 1, 1, 1])  # 顶点/贴图/材质/骨骼/变形/刚体索引尺寸
    out += bytes([0])   # 顶点数量 = 0
    out += bytes([0, 0, 0])  # 各索引数量 = 0
    # 名称（UTF-16LE）
    name = "model".encode("utf-16-le")
    out += bytes([0]) + struct.pack("<I", len(name)) + name
    # 注释
    comment = b"test"
    out += bytes([0]) + struct.pack("<I", len(comment)) + comment
    return bytes(out)


def build_png() -> bytes:
    """构造 1x1 的合法 PNG（RGBA）。"""
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    raw = bytes([0]) + bytes([255, 255, 255, 255])  # filter=0 + RGBA
    idat = zlib.compress(raw)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: gen_test_zip.py <输出 zip 路径>", file=sys.stderr)
        return 2

    out_path = Path(sys.argv[1])
    out_path.parent.mkdir(parents=True, exist_ok=True)

    import zipfile
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("model_package/model.pmx", build_pmx())
        zf.writestr("model_package/texture.png", build_png())
        zf.writestr("model_package/readme.txt", "PPet zip import test\n")

    print(f"已生成测试模型包：{out_path} ({out_path.stat().st_size} 字节)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
