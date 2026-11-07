#!/usr/bin/env python3
"""生成 PMX 模型包（.zip）测试数据。

PMX 文件本身只记录贴图路径、不内嵌图形数据，所以项目把 .zip 作为 PMX 的
一等公民导入方式。本脚本构造四种典型的社区打包形态，用于验证：

    pmx_full.zip      model/test.pmx + model/test_texture.png        —— 标准完整包
    pmx_no_tex.zip    只有 model/test.pmx                              —— 缺贴图（应被诊断出来）
    pmx_case.zip      model/test.pmx + model/TEST_TEXTURE.PNG        —— 贴图大小写不一致
    pmx_subdir.zip    model/test.pmx + model/tex/test_texture.png    —— 贴图在子目录

后两种在 Windows 上能显示、在 Linux 上原本会变成白模，用于验证
PmxModel::ResolveTexturePath 的大小写/目录容错。

用法：gen_test_pmx_zip.py <输出目录>
"""
import subprocess
import sys
import zipfile
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: gen_test_pmx_zip.py <输出目录>", file=sys.stderr)
        return 2

    outdir = Path(sys.argv[1])
    outdir.mkdir(parents=True, exist_ok=True)
    srcdir = outdir / "_source"
    srcdir.mkdir(exist_ok=True)

    # 复用同一套最小合法 PMX 与贴图，避免两份生成逻辑漂移。
    generator = Path(__file__).with_name("gen_test_pmx.py")
    subprocess.run([sys.executable, str(generator), "-o", str(srcdir)], check=True,
                   stdout=subprocess.DEVNULL)

    pmx = (srcdir / "test.pmx").read_bytes()
    png = (srcdir / "test_texture.png").read_bytes()

    def write(name: str, entries: dict) -> None:
        path = outdir / name
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
            for entry_name, data in entries.items():
                zf.writestr(entry_name, data)
        print(f"已生成 {path}（{len(entries)} 个条目）")

    write("pmx_full.zip", {
        "model/test.pmx": pmx,
        "model/test_texture.png": png,
    })
    write("pmx_no_tex.zip", {
        "model/test.pmx": pmx,
    })
    write("pmx_case.zip", {
        "model/test.pmx": pmx,
        "model/TEST_TEXTURE.PNG": png,
    })
    write("pmx_subdir.zip", {
        "model/test.pmx": pmx,
        "model/tex/test_texture.png": png,
    })
    return 0


if __name__ == "__main__":
    sys.exit(main())
