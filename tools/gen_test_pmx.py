#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成用于测试 PPet PMX 解析器的最小合法 PMX 2.0 文件与贴图。

产出（默认写入 tools/testdata/）：
  test.pmx         —— 最小 PMX 2.0 模型：包含骨骼链、材质、贴图索引、UV morph
  test_utf16.pmx   —— 同一模型但文本用 UTF-16LE 编码，用于验证编码分支
  test_texture.png —— 4x4 的测试贴图
  broken_truncated.pmx —— 截断文件（健壮性测试用）
  broken_random.pmx    —— 随机字节文件（健壮性测试用）

PMX 2.0 二进制格式（小端序）各段顺序：
  Header -> ModelInfo(4 个文本) -> Vertices -> Faces -> Textures -> Materials
  -> Bones -> Morphs -> DisplayFrames -> RigidBodies -> Joints

格式依据：felixjones 的 PMX 2.1 规范 + mmd-parser v1.0.0 的读取顺序。
"""

import argparse
import os
import random
import struct
import sys

# ---------------------------------------------------------------------------
# 基础写入工具
# ---------------------------------------------------------------------------

# PMX 全局标志：0 = UTF-16LE，1 = UTF-8
ENCODING_UTF16 = 0
ENCODING_UTF8 = 1


class PmxWriter:
    """按 PMX 2.0 格式顺序写入各段。"""

    def __init__(self, encoding=ENCODING_UTF16):
        self.encoding = encoding
        self.buf = bytearray()

    # -- 基础类型 ---------------------------------------------------------
    def u8(self, v):
        self.buf += struct.pack("<B", v)

    def i8(self, v):
        self.buf += struct.pack("<b", v)

    def u16(self, v):
        self.buf += struct.pack("<H", v)

    def i16(self, v):
        self.buf += struct.pack("<h", v)

    def u32(self, v):
        self.buf += struct.pack("<I", v)

    def i32(self, v):
        self.buf += struct.pack("<i", v)

    def f32(self, v):
        self.buf += struct.pack("<f", v)

    def vec2(self, x, y):
        self.buf += struct.pack("<ff", x, y)

    def vec3(self, x, y, z):
        self.buf += struct.pack("<fff", x, y, z)

    def vec4(self, x, y, z, w):
        self.buf += struct.pack("<ffff", x, y, z, w)

    # -- 文本 -------------------------------------------------------------
    def text(self, s):
        """text = int32 字节长度 + 原始字节（长度是字节数，不是字符数）。"""
        if self.encoding == ENCODING_UTF16:
            data = s.encode("utf-16-le")
        else:
            data = s.encode("utf-8")
        self.i32(len(data))
        self.buf += data

    # -- 索引 -------------------------------------------------------------
    def index(self, v, size):
        """有符号索引，-1 表示空（nil）。"""
        if size == 1:
            self.i8(v)
        elif size == 2:
            self.i16(v)
        else:
            self.i32(v)

    def uindex(self, v, size):
        """无符号索引（顶点索引）。"""
        if size == 1:
            self.u8(v)
        elif size == 2:
            self.u16(v)
        else:
            self.u32(v)

    # -- 段 ---------------------------------------------------------------
    def header(self, version=2.0, additional_uv=0, vertex_index=2, texture_index=1,
               material_index=1, bone_index=1, morph_index=1, rigid_index=1):
        self.buf += b"PMX "
        self.f32(version)
        self.u8(8)  # globals 数量
        self.u8(self.encoding)
        self.u8(additional_uv)
        self.u8(vertex_index)
        self.u8(texture_index)
        self.u8(material_index)
        self.u8(bone_index)
        self.u8(morph_index)
        self.u8(rigid_index)

    def model_info(self, name, name_en, comment, comment_en):
        self.text(name)
        self.text(name_en)
        self.text(comment)
        self.text(comment_en)

    def vertices(self, verts):
        """verts: list of dict，键为 pos/normal/uv/weight_type/bones/weights/edge。"""
        self.i32(len(verts))
        for v in verts:
            self.vec3(*v["pos"])
            self.vec3(*v["normal"])
            self.vec2(*v["uv"])
            for _ in range(v.get("additional_uv", 0)):
                self.vec4(0.0, 0.0, 0.0, 1.0)
            self.u8(v["weight_type"])
            wt = v["weight_type"]
            bones = v["bones"]
            weights = v["weights"]
            if wt == 0:      # BDEF1
                self.index(bones[0], v["bone_index_size"])
            elif wt == 1:    # BDEF2
                self.index(bones[0], v["bone_index_size"])
                self.index(bones[1], v["bone_index_size"])
                self.f32(weights[0])
            elif wt == 2:    # BDEF4
                for b in bones[:4]:
                    self.index(b, v["bone_index_size"])
                for w in weights[:4]:
                    self.f32(w)
            elif wt == 3:    # SDEF
                self.index(bones[0], v["bone_index_size"])
                self.index(bones[1], v["bone_index_size"])
                self.f32(weights[0])
                self.vec3(*v.get("sdef_c", (0.0, 0.0, 0.0)))
                self.vec3(*v.get("sdef_r0", (1.0, 0.0, 0.0)))
                self.vec3(*v.get("sdef_r1", (0.0, 1.0, 0.0)))
            else:            # QDEF
                for b in bones[:4]:
                    self.index(b, v["bone_index_size"])
                for w in weights[:4]:
                    self.f32(w)
            self.f32(v.get("edge", 1.0))

    def faces(self, tris):
        """tris: list of (i0, i1, i2)。写入的索引数 = 三角形数 * 3。"""
        self.i32(len(tris) * 3)
        for tri in tris:
            for idx in tri:
                self.uindex(idx, 2)

    def textures(self, paths):
        self.i32(len(paths))
        for p in paths:
            self.text(p)

    def materials(self, mats):
        """mats: list of dict，字段与 PMX 材质段一一对应。"""
        self.i32(len(mats))
        for m in mats:
            self.text(m["name"])
            self.text(m.get("name_en", ""))
            self.vec4(*m.get("diffuse", (1.0, 1.0, 1.0, 1.0)))
            self.vec3(*m.get("specular", (0.0, 0.0, 0.0)))
            self.f32(m.get("shininess", 5.0))
            self.vec3(*m.get("ambient", (0.0, 0.0, 0.0)))
            self.u8(m.get("flags", 0))
            self.vec4(*m.get("edge_color", (0.0, 0.0, 0.0, 1.0)))
            self.f32(m.get("edge_size", 0.0))
            self.index(m.get("texture", -1), 1)
            self.index(m.get("sphere", -1), 1)
            self.u8(m.get("sphere_mode", 0))
            toon_flag = m.get("toon_flag", 1)
            self.u8(toon_flag)
            if toon_flag == 0:
                self.index(m.get("toon", -1), 1)
            else:
                self.u8(m.get("toon", 0))
            self.text(m.get("comment", ""))
            self.i32(m.get("face_count", 0))

    def bones(self, bones):
        """bones: list of dict。

        字段：name/name_en/pos/parent/layer/flags/tail(索引或偏移)/grant/
        fixed_axis/local_axis/external_key/ik
        """
        self.i32(len(bones))
        for b in bones:
            self.text(b["name"])
            self.text(b.get("name_en", ""))
            self.vec3(*b["pos"])
            self.index(b.get("parent", -1), 1)
            self.u32(b.get("layer", 0))
            flags = b.get("flags", 0)
            self.u16(flags)
            # bit0：尾端是骨骼索引（1）还是 vec3 偏移（0）
            if flags & 0x0001:
                self.index(b.get("tail", -1), 1)
            else:
                self.vec3(*b.get("tail_offset", (0.0, 0.0, 0.0)))
            # bit8/bit9：继承旋转 / 继承平移
            if flags & 0x0300:
                self.index(b.get("grant_parent", -1), 1)
                self.f32(b.get("grant_ratio", 0.0))
            # bit10：固定轴
            if flags & 0x0400:
                self.vec3(*b.get("fixed_axis", (0.0, 0.0, 1.0)))
            # bit11：局部坐标系（X 轴与 Z 轴）
            if flags & 0x0800:
                self.vec3(*b.get("local_x", (1.0, 0.0, 0.0)))
                self.vec3(*b.get("local_z", (0.0, 0.0, 1.0)))
            # bit13：外部父级变形 key（uint32）
            if flags & 0x2000:
                self.u32(b.get("external_key", 0))
            # bit5：IK
            if flags & 0x0020:
                self.index(b.get("ik_target", -1), 1)
                self.u32(b.get("ik_iteration", 0))
                self.f32(b.get("ik_limit_angle", 0.0))
                links = b.get("ik_links", [])
                self.i32(len(links))
                for link in links:
                    self.index(link["bone"], 1)
                    has_limit = 1 if "limit" in link else 0
                    self.u8(has_limit)
                    if has_limit:
                        self.vec3(*link["limit"][0])
                        self.vec3(*link["limit"][1])

    def morphs(self, morphs):
        """morphs: list of dict，type 为 PMX 权重/形变类型编号。

        type: 0=组 1=顶点 2=骨骼 3=UV 4..7=附加UV 8=材质
        """
        self.i32(len(morphs))
        for m in morphs:
            self.text(m["name"])
            self.text(m.get("name_en", ""))
            self.u8(m.get("panel", 0))
            mtype = m["type"]
            self.u8(mtype)
            elements = m.get("elements", [])
            self.i32(len(elements))
            for e in elements:
                if mtype == 0:      # 组 morph
                    self.index(e["morph"], 1)
                    self.f32(e["influence"])
                elif mtype == 1:    # 顶点形变
                    self.uindex(e["vertex"], 2)
                    self.vec3(*e["offset"])
                elif mtype == 2:    # 骨骼形变
                    self.index(e["bone"], 1)
                    self.vec3(*e["offset"])
                    self.vec4(*e.get("rotation", (0.0, 0.0, 0.0, 1.0)))
                elif mtype in (3, 4, 5, 6, 7):  # UV / 附加 UV 形变
                    self.uindex(e["vertex"], 2)
                    self.vec4(*e["offset"])
                elif mtype == 8:    # 材质形变
                    self.index(e.get("material", -1), 1)
                    self.u8(e.get("operation", 0))
                    self.vec4(*e.get("diffuse", (1.0, 1.0, 1.0, 1.0)))
                    self.vec3(*e.get("specular", (0.0, 0.0, 0.0)))
                    self.f32(e.get("specular_strength", 5.0))
                    self.vec3(*e.get("ambient", (0.0, 0.0, 0.0)))
                    self.vec4(*e.get("edge_color", (0.0, 0.0, 0.0, 1.0)))
                    self.f32(e.get("edge_size", 0.0))
                    self.vec4(*e.get("texture_tint", (1.0, 1.0, 1.0, 1.0)))
                    self.vec4(*e.get("sphere_tint", (1.0, 1.0, 1.0, 1.0)))
                    self.vec4(*e.get("toon_tint", (1.0, 1.0, 1.0, 1.0)))
                else:
                    raise ValueError("unsupported morph type %d" % mtype)

    def display_frames(self, frames):
        """frames: list of dict，items 为 (target, index) 列表，target: 0=骨骼 1=morph。"""
        self.i32(len(frames))
        for f in frames:
            self.text(f["name"])
            self.text(f.get("name_en", ""))
            self.u8(f.get("special", 0))
            items = f.get("items", [])
            self.i32(len(items))
            for target, index in items:
                self.u8(target)
                if target == 0:
                    self.index(index, 1)
                else:
                    self.index(index, 1)

    def rigid_bodies(self, bodies):
        self.i32(len(bodies))
        for b in bodies:
            self.text(b["name"])
            self.text(b.get("name_en", ""))
            self.index(b.get("bone", -1), 1)
            self.u8(b.get("group", 0))
            self.u16(b.get("mask", 0))
            self.u8(b.get("shape", 0))
            self.vec3(*b.get("size", (1.0, 1.0, 1.0)))
            self.vec3(*b.get("pos", (0.0, 0.0, 0.0)))
            self.vec3(*b.get("rot", (0.0, 0.0, 0.0)))
            self.f32(b.get("mass", 1.0))
            self.f32(b.get("move_attenuation", 0.5))
            self.f32(b.get("rot_damping", 0.5))
            self.f32(b.get("restitution", 0.0))
            self.f32(b.get("friction", 0.5))
            self.u8(b.get("mode", 0))

    def joints(self, joints):
        self.i32(len(joints))
        for j in joints:
            self.text(j["name"])
            self.text(j.get("name_en", ""))
            self.u8(j.get("type", 0))
            self.index(j.get("body_a", -1), 1)
            self.index(j.get("body_b", -1), 1)
            self.vec3(*j.get("pos", (0.0, 0.0, 0.0)))
            self.vec3(*j.get("rot", (0.0, 0.0, 0.0)))
            self.vec3(*j.get("limit_min", (0.0, 0.0, 0.0)))
            self.vec3(*j.get("limit_max", (0.0, 0.0, 0.0)))
            self.vec3(*j.get("rot_limit_min", (0.0, 0.0, 0.0)))
            self.vec3(*j.get("rot_limit_max", (0.0, 0.0, 0.0)))
            self.vec3(*j.get("spring_pos", (0.0, 0.0, 0.0)))
            self.vec3(*j.get("spring_rot", (0.0, 0.0, 0.0)))

    def bytes(self):
        return bytes(self.buf)


# ---------------------------------------------------------------------------
# 测试模型内容
# ---------------------------------------------------------------------------

# 骨骼 flag 位
BONE_ROTATABLE = 0x0002
BONE_TRANSLATABLE = 0x0004
BONE_VISIBLE = 0x0008
BONE_ENABLED = 0x0010
BONE_IK = 0x0020
BONE_INDEXED_TAIL = 0x0001

# 材质 flag 位
MAT_NO_CULL = 0x01
MAT_HAS_EDGE = 0x10
MAT_VERTEX_COLOR = 0x20


def build_cube():
    """构造一个规范一致的立方体网格。

    返回 (vertices, faces)：
      - 24 个顶点（每个面 4 个顶点，法线为面法线），4 种权重类型轮转覆盖；
      - 12 个三角形。

    绕序约定（依据真实 MMD 模型实测）：在 PMX 原始的左手系数据里，
    用标准叉积公式算 cross(v1-v0, v2-v0)，结果应与顶点法线同向。
    转换到右手系时做两件事——取反 Z（镜像，翻转叉积符号）与交换索引 0/2
    （再翻转一次），两次翻转相互抵消，因此「叉积与法线同向」这一关系在
    转换前后保持不变，转换后即为 OpenGL 需要的 CCW 正面朝外。
    下面按这个判据构造：取面法线 n 与两个切向量 t1、t2 使 cross(t1, t2) == n，
    再按 (c-t1-t2, c+t1-t2, c+t1+t2, c-t1+t2) 排布四个角点。
    """
    # 每个面：(中心, 外法线, 切向量 t1, 切向量 t2)，必须满足 cross(t1, t2) == n。
    # 注意：顶面/底面的 t2 取反方向，否则叉积得到 -n（会导致转换后该面绕序反向）。
    faces_def = [
        # 前面（MMD 中模型朝向 -Z）
        ((0.0, 1.0, -1.0), (0.0, 0.0, -1.0), (1.0, 0.0, 0.0), (0.0, -1.0, 0.0)),
        # 后面
        ((0.0, 1.0, 1.0), (0.0, 0.0, 1.0), (-1.0, 0.0, 0.0), (0.0, -1.0, 0.0)),
        # 右面
        ((1.0, 1.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (0.0, -1.0, 0.0)),
        # 左面
        ((-1.0, 1.0, 0.0), (-1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.0, -1.0, 0.0)),
        # 顶面：cross((1,0,0), (0,0,-1)) == (0,1,0) == n
        ((0.0, 2.0, 0.0), (0.0, 1.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, -1.0)),
        # 底面：cross((1,0,0), (0,0,1)) == (0,-1,0) == n
        ((0.0, 0.0, 0.0), (0.0, -1.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0)),
    ]

    verts = []
    tris = []
    for center, normal, t1, t2 in faces_def:
        base = len(verts)
        for sign1, sign2 in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            pos = tuple(center[i] + sign1 * t1[i] + sign2 * t2[i] for i in range(3))
            verts.append((pos, normal))
        # (0,1,2) 与 (0,2,3)：按上面的构造，叉积与外法线同向。
        tris.append((base + 0, base + 1, base + 2))
        tris.append((base + 0, base + 2, base + 3))
    return verts, tris


def build_model(encoding=ENCODING_UTF16):
    """构造测试模型内容。

    骨骼链刻意让「父骨骼下标大于自身」：
        0 center  <- 3 neck
        3 neck    <- 2 upper body
        2 upper body <- 1 head
        1 head    <- 4 eye_L, 5 eye_R
    这样可以验证解析器 / 转换层的拓扑排序逻辑。
    """
    w = PmxWriter(encoding)
    w.header(version=2.0, additional_uv=1, vertex_index=2, texture_index=1,
             material_index=1, bone_index=1, morph_index=1, rigid_index=1)
    w.model_info(
        "テストモデル",
        "Test Model",
        "PMX 解析器测试用模型",
        "Model used by the PMX parser test suite",
    )

    # --- 顶点与面 ---------------------------------------------------------
    corners, tris = build_cube()
    verts = []
    for i, (pos, nrm) in enumerate(corners):
        # 轮转覆盖 4 种权重类型：BDEF1 / BDEF2 / BDEF4 / SDEF。
        kind = i % 4
        if kind == 0:
            wt, bones, weights = 0, [i % 6], [1.0]
        elif kind == 1:
            wt, bones, weights = 1, [i % 6, (i + 1) % 6], [0.7]
        elif kind == 2:
            wt, bones, weights = 2, [0, 1, 2, 3], [0.4, 0.3, 0.2, 0.1]
        else:
            wt, bones, weights = 3, [0, 1], [0.6]
        verts.append({
            "pos": pos,
            "normal": nrm,
            "uv": ((i % 4) / 4.0, (i // 4) / 4.0),
            "additional_uv": 1,
            "weight_type": wt,
            "bones": bones,
            "weights": weights,
            "edge": 1.0,
            "bone_index_size": 1,
        })
    w.vertices(verts)
    w.faces(tris)

    # --- 贴图 -------------------------------------------------------------
    w.textures(["test_texture.png"])

    # --- 材质：两个材质，第一个 6 个三角形，第二个 6 个 -------------------
    w.materials([
        {
            "name": "マテリアル1",
            "name_en": "Material1",
            "diffuse": (0.8, 0.7, 0.6, 1.0),
            "specular": (0.2, 0.2, 0.2),
            "shininess": 12.0,
            "ambient": (0.1, 0.1, 0.1),
            "flags": MAT_NO_CULL | MAT_HAS_EDGE,
            "edge_color": (0.0, 0.0, 0.0, 1.0),
            "edge_size": 1.5,
            "texture": 0,
            "sphere": -1,
            "sphere_mode": 0,
            "toon_flag": 0,
            "toon": -1,
            "comment": "first material",
            "face_count": 6 * 3,
        },
        {
            "name": "マテリアル2",
            "name_en": "Material2",
            "diffuse": (0.4, 0.5, 0.9, 1.0),
            "specular": (0.0, 0.0, 0.0),
            "shininess": 5.0,
            "ambient": (0.05, 0.05, 0.05),
            "flags": MAT_HAS_EDGE,
            "edge_color": (0.1, 0.1, 0.1, 1.0),
            "edge_size": 1.0,
            "texture": 0,
            "sphere": -1,
            "sphere_mode": 0,
            "toon_flag": 1,
            "toon": 2,
            "comment": "second material",
            "face_count": 6 * 3,
        },
    ])

    # --- 骨骼：父下标刻意大于自身 ---------------------------------------
    base = BONE_ROTATABLE | BONE_TRANSLATABLE | BONE_VISIBLE | BONE_ENABLED | BONE_INDEXED_TAIL
    w.bones([
        {   # 0 center
            "name": "センター", "name_en": "center",
            "pos": (0.0, 0.0, 0.0), "parent": -1, "flags": base,
            "tail": 3,
        },
        {   # 1 head，父是 2（父下标 > 自身）
            "name": "頭", "name_en": "head",
            "pos": (0.0, 1.9, 0.0), "parent": 2, "flags": base,
            "tail": 4,
        },
        {   # 2 upper body，父是 3
            "name": "上半身", "name_en": "upper body",
            "pos": (0.0, 1.0, 0.0), "parent": 3, "flags": base,
            "tail": 1,
        },
        {   # 3 neck，父是 0
            "name": "首", "name_en": "neck",
            "pos": (0.0, 1.7, 0.0), "parent": 0, "flags": base,
            "tail": 1,
        },
        {   # 4 eye_L
            "name": "左目", "name_en": "eye_L",
            "pos": (0.2, 1.8, -0.1), "parent": 1, "flags": base,
            "tail": -1,
        },
        {   # 5 eye_R
            "name": "右目", "name_en": "eye_R",
            "pos": (-0.2, 1.8, -0.1), "parent": 1, "flags": base,
            "tail": -1,
        },
    ])

    # --- morph：顶点形变 + UV 形变 ---------------------------------------
    w.morphs([
        {
            "name": "頂点変形", "name_en": "vertex morph", "panel": 1, "type": 1,
            "elements": [
                {"vertex": 2, "offset": (0.0, 0.5, 0.0)},
                {"vertex": 3, "offset": (0.0, 0.5, 0.0)},
            ],
        },
        {
            "name": "UV変形", "name_en": "uv morph", "panel": 2, "type": 3,
            "elements": [
                {"vertex": 0, "offset": (0.1, 0.0, 0.0, 0.0)},
                {"vertex": 1, "offset": (0.2, 0.0, 0.0, 0.0)},
            ],
        },
    ])

    # --- 显示帧 -----------------------------------------------------------
    w.display_frames([
        {"name": "表情1", "name_en": "Face1", "special": 0, "items": [(1, 0)]},
        {"name": "表情2", "name_en": "Face2", "special": 0, "items": [(1, 1)]},
    ])

    # --- 刚体与关节 -------------------------------------------------------
    w.rigid_bodies([
        {
            "name": "剛体1", "name_en": "RigidBody1",
            "bone": 0, "group": 0, "mask": 0, "shape": 0,
            "size": (1.0, 1.0, 1.0), "pos": (0.0, 1.0, 0.0), "rot": (0.0, 0.0, 0.0),
            "mass": 1.0, "move_attenuation": 0.5, "rot_damping": 0.5,
            "restitution": 0.0, "friction": 0.5, "mode": 0,
        },
    ])
    w.joints([
        {
            "name": "関節1", "name_en": "Joint1",
            "type": 0, "body_a": 0, "body_b": -1,
            "pos": (0.0, 1.0, 0.0), "rot": (0.0, 0.0, 0.0),
            "limit_min": (-30.0, -30.0, -30.0), "limit_max": (30.0, 30.0, 30.0),
            "rot_limit_min": (-30.0, -30.0, -30.0), "rot_limit_max": (30.0, 30.0, 30.0),
            "spring_pos": (0.0, 0.0, 0.0), "spring_rot": (0.0, 0.0, 0.0),
        },
    ])
    return w.bytes()


# ---------------------------------------------------------------------------
# 贴图
# ---------------------------------------------------------------------------

def write_test_png(path):
    """生成 4x4 的 RGB 测试贴图（不依赖第三方库，直接写 PNG）。"""
    import zlib

    width = height = 4
    # 每行像素：红、绿、蓝、白 循环。
    palette = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        for x in range(width):
            r, g, b = palette[(x + y) % 4]
            raw += bytes((r, g, b))

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="生成 PMX 解析器测试素材")
    ap.add_argument("-o", "--outdir", default=None,
                    help="输出目录（默认 tools/testdata）")
    ap.add_argument("--seed", type=int, default=1234, help="随机文件使用的种子")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    outdir = args.outdir or os.path.join(here, "testdata")
    os.makedirs(outdir, exist_ok=True)

    utf16_data = build_model(ENCODING_UTF16)
    utf8_data = build_model(ENCODING_UTF8)

    files = {
        "test.pmx": utf16_data,
        "test_utf16.pmx": utf16_data,
        "test_utf8.pmx": utf8_data,
    }
    for name, data in files.items():
        path = os.path.join(outdir, name)
        with open(path, "wb") as f:
            f.write(data)
        print("wrote %s (%d bytes)" % (path, len(data)))

    png_path = os.path.join(outdir, "test_texture.png")
    write_test_png(png_path)
    print("wrote %s" % png_path)

    # 截断文件：在中段切断，解析器必须报错而不是崩溃。
    trunc_path = os.path.join(outdir, "broken_truncated.pmx")
    with open(trunc_path, "wb") as f:
        f.write(utf16_data[:len(utf16_data) // 3])
    print("wrote %s" % trunc_path)

    # 随机字节文件：magic 都不对，解析器必须报错。
    rand_path = os.path.join(outdir, "broken_random.pmx")
    rng = random.Random(args.seed)
    with open(rand_path, "wb") as f:
        f.write(bytes(rng.randrange(256) for _ in range(512)))
    print("wrote %s" % rand_path)

    # 伪造一个「magic 正确但内容截断」的文件，验证头部通过后仍能安全失败。
    fake_path = os.path.join(outdir, "broken_header_only.pmx")
    with open(fake_path, "wb") as f:
        f.write(b"PMX " + struct.pack("<f", 2.0) + bytes([8, 0, 0, 1, 1, 1, 1, 1]))
    print("wrote %s" % fake_path)

    return 0


if __name__ == "__main__":
    sys.exit(main())
