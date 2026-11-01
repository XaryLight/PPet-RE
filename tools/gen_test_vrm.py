#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成用于测试 PPet VRM 解析器的最小合法 VRM 1.0 / VRM 0.x 文件与健壮性素材。

产出（默认写入 tools/testdata/）：
  test.vrm            —— 最小合法 VRM 1.0 模型：
                         三角网格（两个 primitive）+ skin/骨骼 + humanoid humanBones
                         + MToon 材质 + expressions（preset + custom）+ springBone
                         + 一段 glTF 动画（translation/rotation/weights）
  test_vrm0.vrm       —— 最小 VRM 0.x 模型（验证旧版扩展的兼容分支）
  broken_truncated.vrm—— 截断文件（健壮性测试用）
  broken_random.vrm   —— 随机字节文件（健壮性测试用）
  broken_badmagic.vrm —— GLB 头标识错误
  broken_badjson.vrm  —— JSON 语法错误
  broken_oob.vrm      —— 畸形 JSON：索引越界 / 计数为负 / 循环节点引用
  broken_count.vrm    —— accessor 的 count 超出上限（恶意文件）

GLB 容器结构（小端序）：
  Header (12 字节) : magic "glTF"(0x46546C67) | version(2) | length
  Chunk 0          : length | type(0x4E4F534A "JSON") | JSON 数据（空格补齐到 4 字节）
  Chunk 1          : length | type(0x004E4942 "BIN\0") | 二进制数据（0 字节补齐到 4 字节）

坐标约定（VRM 1.0）：右手系、Y 轴向上、正面朝 +Z、单位为米。
"""

import argparse
import base64
import json
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# GLB 写入工具
# ---------------------------------------------------------------------------

GLB_MAGIC = 0x46546C67  # "glTF"
CHUNK_JSON = 0x4E4F534A  # "JSON"
CHUNK_BIN = 0x004E4942  # "BIN\0"


class BinaryBuilder:
    """按 4 字节对齐地拼装 BIN chunk，并记录每个数据块的 bufferView。"""

    def __init__(self):
        self.data = bytearray()
        self.buffer_views = []

    def add(self, payload, target=None):
        """追加一段数据，返回 bufferView 下标。"""
        # bufferView 的 byteOffset 必须 4 字节对齐（glTF 2.0 规范要求）。
        while len(self.data) % 4 != 0:
            self.data.append(0)
        offset = len(self.data)
        self.data += payload
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(payload)}
        if target is not None:
            view["target"] = target
        self.buffer_views.append(view)
        return len(self.buffer_views) - 1


def pack_f32(values):
    return struct.pack("<%df" % len(values), *values)


def pack_u32(values):
    return struct.pack("<%dI" % len(values), *values)


def pack_u16(values):
    return struct.pack("<%dH" % len(values), *values)


def write_glb(path, gltf_json, binary):
    """把 JSON 与 BIN 数据打包成 GLB 文件。"""
    json_bytes = json.dumps(gltf_json, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    # JSON chunk 用空格（0x20）补齐；BIN chunk 用 0x00 补齐。
    while len(json_bytes) % 4 != 0:
        json_bytes += b" "
    bin_bytes = bytes(binary)
    while len(bin_bytes) % 4 != 0:
        bin_bytes += b"\x00"

    total = 12 + 8 + len(json_bytes)
    if len(bin_bytes) > 0:
        total += 8 + len(bin_bytes)

    out = bytearray()
    out += struct.pack("<III", GLB_MAGIC, 2, total)
    out += struct.pack("<II", len(json_bytes), CHUNK_JSON)
    out += json_bytes
    if len(bin_bytes) > 0:
        out += struct.pack("<II", len(bin_bytes), CHUNK_BIN)
        out += bin_bytes

    with open(path, "wb") as f:
        f.write(out)
    return len(out)


# ---------------------------------------------------------------------------
# 小工具：生成一张 2x2 的 PNG（作为 MToon 的贴图）
# ---------------------------------------------------------------------------


def write_glb_raw_json(path, json_text, binary):
    """把一段原始 JSON 文本打包成 GLB。

    json.dumps 无法表达超出 double 范围的数值字面量，
    这类回归用例必须直接写原始字节。
    """
    json_bytes = json_text.encode("utf-8")
    while len(json_bytes) % 4 != 0:
        json_bytes += b" "
    bin_bytes = bytes(binary)
    while len(bin_bytes) % 4 != 0:
        bin_bytes += b"\x00"

    total = 12 + 8 + len(json_bytes)
    if len(bin_bytes) > 0:
        total += 8 + len(bin_bytes)

    out = bytearray()
    out += struct.pack("<III", GLB_MAGIC, 2, total)
    out += struct.pack("<II", len(json_bytes), CHUNK_JSON)
    out += json_bytes
    if len(bin_bytes) > 0:
        out += struct.pack("<II", len(bin_bytes), CHUNK_BIN)
        out += bin_bytes

    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def make_png(width, height, rgba_rows):
    """用 zlib 手工拼一张 PNG（避免依赖 Pillow）。"""
    raw = bytearray()
    for row in rgba_rows:
        raw.append(0)  # filter type 0 (None)
        raw += row

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8bit RGBA
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------------------
# VRM 1.0 测试模型
# ---------------------------------------------------------------------------


def build_vrm10():
    """构造最小合法 VRM 1.0 模型，返回 (gltf_json, binary_bytes)。"""
    binb = BinaryBuilder()

    # --- 几何：一个「身体」四边形 + 一个「头」三角形 ------------------------
    # 身体：绕 Y 轴的方形，法线朝 +Z。
    body_positions = [
        (-0.20, 0.00, 0.00),
        (0.20, 0.00, 0.00),
        (0.20, 0.80, 0.00),
        (-0.20, 0.80, 0.00),
    ]
    body_normals = [(0.0, 0.0, 1.0)] * 4
    body_uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    body_indices = [0, 1, 2, 0, 2, 3]

    # 头：一个小三角形，挂在头骨节点下。
    head_positions = [
        (-0.08, 0.00, 0.00),
        (0.08, 0.00, 0.00),
        (0.00, 0.12, 0.00),
    ]
    head_normals = [(0.0, 0.0, 1.0)] * 3
    head_uvs = [(0.0, 0.0), (1.0, 0.0), (0.5, 1.0)]
    head_indices = [0, 1, 2]

    # 骨骼权重：身体顶点全部绑到 hips(0)，头部顶点绑到 head(2)。
    body_joints = [(0, 0, 0, 0)] * 4
    body_weights = [(1.0, 0.0, 0.0, 0.0)] * 4
    head_joints = [(2, 0, 0, 0)] * 3
    head_weights = [(1.0, 0.0, 0.0, 0.0)] * 3

    # morph 目标：把身体顶点往上抬 0.1。
    body_morph = [(0.0, 0.1, 0.0)] * 4
    head_morph = [(0.0, 0.02, 0.0)] * 3

    # --- 把数据写进 BIN chunk ---------------------------------------------
    accessors = []
    buffer_views = binb.buffer_views

    def add_accessor(view_index, component_type, type_name, count, target=None):
        acc = {"bufferView": view_index, "componentType": component_type, "count": count,
               "type": type_name}
        if target is not None:
            acc["target"] = target
        accessors.append(acc)
        return len(accessors) - 1

    def add_vec3(values):
        flat = []
        for v in values:
            flat.extend(v)
        view = binb.add(pack_f32(flat))
        return add_accessor(view, 5126, "VEC3", len(values))

    def add_vec2(values):
        flat = []
        for v in values:
            flat.extend(v)
        view = binb.add(pack_f32(flat))
        return add_accessor(view, 5126, "VEC2", len(values))

    def add_indices(values):
        view = binb.add(pack_u16(values))
        return add_accessor(view, 5123, "SCALAR", len(values))

    def add_joints(values):
        flat = []
        for v in values:
            flat.extend(v)
        view = binb.add(pack_u16(flat))
        return add_accessor(view, 5123, "VEC4", len(values))

    def add_weights(values):
        flat = []
        for v in values:
            flat.extend(v)
        view = binb.add(pack_f32(flat))
        return add_accessor(view, 5126, "VEC4", len(values))

    acc = {}
    acc["body_pos"] = add_vec3(body_positions)
    acc["body_nrm"] = add_vec3(body_normals)
    acc["body_uv"] = add_vec2(body_uvs)
    acc["body_joint"] = add_joints(body_joints)
    acc["body_weight"] = add_weights(body_weights)
    acc["body_idx"] = add_indices(body_indices)
    acc["body_morph"] = add_vec3(body_morph)

    acc["head_pos"] = add_vec3(head_positions)
    acc["head_nrm"] = add_vec3(head_normals)
    acc["head_uv"] = add_vec2(head_uvs)
    acc["head_joint"] = add_joints(head_joints)
    acc["head_weight"] = add_weights(head_weights)
    acc["head_idx"] = add_indices(head_indices)
    acc["head_morph"] = add_vec3(head_morph)

    # 逆绑定矩阵：骨骼在绑定姿态下的世界矩阵的逆。
    # 骨骼节点顺序：0 hips, 1 spine, 2 head, 3 leftEye, 4 rightEye, 5 hair。
    # 绑定姿态下：hips 在原点，spine 在 (0, 0.4, 0)，head 在 (0, 0.8, 0)，
    # leftEye 在 (0.05, 0.86, 0.05)，rightEye 在 (-0.05, 0.86, 0.05)。
    joint_bind_positions = [
        (0.0, 0.0, 0.0),
        (0.0, 0.4, 0.0),
        (0.0, 0.8, 0.0),
        (0.05, 0.86, 0.05),
        (-0.05, 0.86, 0.05),
        (0.0, 0.9, -0.05),
    ]
    ibm = bytearray()
    for (px, py, pz) in joint_bind_positions:
        # 单位旋转 + 平移的逆 = 单位旋转 - 平移。
        ibm += struct.pack("<16f", 1, 0, 0, 0,
                            0, 1, 0, 0,
                            0, 0, 1, 0,
                            -px, -py, -pz, 1)
    acc["ibm"] = add_accessor(binb.add(bytes(ibm)), 5126, "MAT4", len(joint_bind_positions))

    # 动画：0.0 秒与 1.0 秒两个关键帧，让 hips 绕 Y 轴转 90 度。
    acc["anim_time"] = add_accessor(binb.add(pack_f32([0.0, 1.0])), 5126, "SCALAR", 2)
    # rotation 是四元数 xyzw：绕 Y 轴 0 度 -> 绕 Y 轴 90 度。
    acc["anim_rot"] = add_accessor(
        binb.add(pack_f32([0.0, 0.0, 0.0, 1.0, 0.0, 0.70710678, 0.0, 0.70710678])), 5126, "VEC4", 2)
    # weights 通道：morph 权重。
    acc["anim_weights"] = add_accessor(binb.add(pack_f32([0.0, 1.0])), 5126, "SCALAR", 2)

    # --- 贴图 -------------------------------------------------------------
    png = make_png(2, 2, [
        bytes([255, 0, 0, 255, 0, 255, 0, 255]),
        bytes([0, 0, 255, 255, 255, 255, 0, 255]),
    ])
    image_view = binb.add(png)

    # --- 组装 glTF --------------------------------------------------------
    gltf = {
        "asset": {"version": "2.0", "generator": "PPet gen_test_vrm.py"},
        "scene": 0,
        "scenes": [{"name": "Scene", "nodes": [0]}],
        "nodes": [
            # 0: 场景根节点（带一个缩放，用来验证「骨骼世界矩阵包含场景根变换」）。
            {
                "name": "Root",
                "scale": [1.0, 1.0, 1.0],
                "children": [1, 6, 7],
            },
            # 1: hips
            {"name": "hips", "translation": [0.0, 0.0, 0.0], "children": [2]},
            # 2: spine
            {"name": "spine", "translation": [0.0, 0.4, 0.0], "children": [3]},
            # 3: head
            {"name": "head", "translation": [0.0, 0.4, 0.0], "children": [4, 5]},
            # 4: leftEye
            {"name": "leftEye", "translation": [0.05, 0.06, 0.05]},
            # 5: rightEye
            {"name": "rightEye", "translation": [-0.05, 0.06, 0.05]},
            # 6: 身体网格节点
            {"name": "Body", "mesh": 0, "skin": 0},
            # 7: 头部网格节点（带 morph 目标）
            {"name": "Head", "mesh": 1, "skin": 0, "translation": [0.0, 0.8, 0.0]},
            # 8: 头发弹簧骨根节点
            {"name": "hairRoot", "translation": [0.0, 0.9, -0.05]},
            # 9: 头发弹簧骨末端节点
            {"name": "hairTip", "translation": [0.0, 0.1, 0.0]},
        ],
        # 修正根节点的 children：加上弹簧骨节点。
        "meshes": [
            {
                "name": "BodyMesh",
                "primitives": [{
                    "attributes": {
                        "POSITION": acc["body_pos"],
                        "NORMAL": acc["body_nrm"],
                        "TEXCOORD_0": acc["body_uv"],
                        "JOINTS_0": acc["body_joint"],
                        "WEIGHTS_0": acc["body_weight"],
                    },
                    "indices": acc["body_idx"],
                    "material": 0,
                    "mode": 4,
                }],
            },
            {
                "name": "HeadMesh",
                "primitives": [{
                    "attributes": {
                        "POSITION": acc["head_pos"],
                        "NORMAL": acc["head_nrm"],
                        "TEXCOORD_0": acc["head_uv"],
                        "JOINTS_0": acc["head_joint"],
                        "WEIGHTS_0": acc["head_weight"],
                    },
                    "indices": acc["head_idx"],
                    "material": 1,
                    "mode": 4,
                    "targets": [{"POSITION": acc["head_morph"]}],
                }],
                "extras": {"targetNames": ["HeadUp"]},
            },
        ],
        "skins": [{
            "name": "Skin",
            "joints": [1, 2, 3, 4, 5, 8],
            "inverseBindMatrices": acc["ibm"],
            "skeleton": 1,
        }],
        "materials": [
            {
                "name": "BodyMaterial",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.9, 0.7, 0.7, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.9,
                },
                "doubleSided": True,
                "extensions": {
                    "VRMC_materials_mtoon": {
                        "shadeColorFactor": [0.3, 0.3, 0.3, 1.0],
                        "rimColorFactor": [0.5, 0.5, 0.5, 1.0],
                        "rimLightingMixFactor": 0.4,
                        "parametricRimFresnelPowerFactor": 5.0,
                        "shadingShiftFactor": 0.1,
                        "shadingToonyFactor": 0.8,
                        "outlineWidthMode": "worldCoordinates",
                        "outlineWidthFactor": 0.02,
                        "outlineColorFactor": [0.1, 0.1, 0.1, 1.0],
                        "outlineLightingMixFactor": 0.5,
                    }
                },
            },
            {
                "name": "HeadMaterial",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [1.0, 0.9, 0.8, 1.0],
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.5,
                },
                "extensions": {
                    "VRMC_materials_mtoon": {
                        "shadeColorFactor": [0.2, 0.2, 0.2, 1.0],
                        "shadeMultiplyTexture": {"index": 0},
                        "rimColorFactor": [0.8, 0.8, 0.8, 1.0],
                        "rimMultiplyTexture": {"index": 0},
                        "rimLightingMixFactor": 1.0,
                    }
                },
            },
        ],
        "textures": [{"source": 0, "sampler": 0}],
        "images": [{"name": "TestTexture", "mimeType": "image/png", "bufferView": image_view}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "animations": [{
            "name": "Idle",
            "samplers": [
                {"input": acc["anim_time"], "output": acc["anim_rot"], "interpolation": "LINEAR"},
                {"input": acc["anim_time"], "output": acc["anim_weights"], "interpolation": "LINEAR"},
            ],
            "channels": [
                {"sampler": 0, "target": {"node": 1, "path": "rotation"}},
                {"sampler": 1, "target": {"node": 7, "path": "weights"}},
            ],
        }],
        "extensionsUsed": ["VRMC_vrm", "VRMC_materials_mtoon", "VRMC_springBone"],
        "extensions": {
            "VRMC_vrm": {
                "meta": {
                    "name": "PPet 测试模型",
                    "version": "1.0",
                    "authors": ["PPet"],
                    "licenseUrl": "https://example.com/license",
                    "avatarPermission": "onlyAuthor",
                    "commercialUsage": "personalProfit",
                    "creditNotation": "required",
                    "allowExcessivelyViolentUsage": False,
                    "allowExcessivelySexualUsage": False,
                    "creditable": True,
                    "redistribution": "disallow",
                    "modification": "prohibited",
                    "otherLicenseUrl": "",
                },
                "humanoid": {
                    "humanBones": {
                        "hips": {"node": 1},
                        "spine": {"node": 2},
                        "head": {"node": 3},
                        "leftEye": {"node": 4},
                        "rightEye": {"node": 5},
                    }
                },
                "lookAt": {
                    "type": "bone",
                    "rangeMapHorizontal": {"inputMaxValue": 1.0, "outputScale": 15.0,
                                           "outputOffset": 0.0},
                    "rangeMapVertical": {"inputMaxValue": 1.0, "outputScale": 10.0,
                                         "outputOffset": 0.0},
                },
                "expressions": {
                    "preset": {
                        "happy": {
                            "isBinary": False,
                            "morphTargetBinds": [{"node": 7, "index": 0, "weight": 1.0}],
                        },
                        "blink": {
                            "isBinary": True,
                            "morphTargetBinds": [{"node": 7, "index": 0, "weight": 1.0}],
                        },
                    },
                    "custom": {
                        "TestCustom": {
                            "isBinary": False,
                            "morphTargetBinds": [{"node": 7, "index": 0, "weight": 0.5}],
                        },
                    },
                },
            },
            "VRMC_springBone": {
                "colliderGroups": [{"node": 3, "colliders": [0]}],
                "colliders": [{"node": 3, "offset": [0.0, 0.0, 0.0], "radius": 0.05}],
                "springs": [{
                    "name": "hair",
                    "jointNode": 8,
                    "hitRadius": 0.02,
                    "stiffness": 1.0,
                    "dragForce": 0.5,
                    "gravityPower": 0.0,
                    "gravityDir": [0.0, -1.0, 0.0],
                    "colliderGroups": [0],
                }],
            },
        },
    }
    # 根节点的 children 补上弹簧骨节点 8（9 是 hairTip，挂在 8 下面）。
    gltf["nodes"][0]["children"] = [1, 6, 7, 8]
    gltf["nodes"][8]["children"] = [9]

    # 把 accessor 与 bufferView 写进 JSON。
    # 必须在所有 add_* 调用之后写，因为 add_accessor 会往 accessors 里追加。
    gltf["accessors"] = accessors
    gltf["bufferViews"] = buffer_views
    gltf["buffers"] = [{"byteLength": len(binb.data)}]

    return gltf, bytes(binb.data)


# ---------------------------------------------------------------------------
# VRM 0.x 测试模型
# ---------------------------------------------------------------------------


def build_vrm0():
    """构造最小 VRM 0.x 模型（extensions.VRM 老结构）。"""
    binb = BinaryBuilder()

    positions = [(-0.15, 0.0, 0.0), (0.15, 0.0, 0.0), (0.15, 0.6, 0.0), (-0.15, 0.6, 0.0)]
    normals = [(0.0, 0.0, 1.0)] * 4
    indices = [0, 1, 2, 0, 2, 3]
    joints = [(0, 0, 0, 0)] * 4
    weights = [(1.0, 0.0, 0.0, 0.0)] * 4

    # morph 目标：把顶点往上抬，模拟「张嘴 / 变表情」。
    # VRM 0.x 的 blendShapeGroups 就是靠 (mesh, index) 引用这些 morph 目标。
    morph_up = [(0.0, 0.1, 0.0)] * 4

    accessors = []

    def add_accessor(view_index, component_type, type_name, count):
        accessors.append({"bufferView": view_index, "componentType": component_type,
                          "count": count, "type": type_name})
        return len(accessors) - 1

    def add_vec3(values):
        flat = []
        for v in values:
            flat.extend(v)
        return add_accessor(binb.add(pack_f32(flat)), 5126, "VEC3", len(values))

    def add_u16(values):
        return add_accessor(binb.add(pack_u16(values)), 5123, "SCALAR", len(values))

    def add_joints(values):
        flat = []
        for v in values:
            flat.extend(v)
        return add_accessor(binb.add(pack_u16(flat)), 5123, "VEC4", len(values))

    def add_weights(values):
        flat = []
        for v in values:
            flat.extend(v)
        return add_accessor(binb.add(pack_f32(flat)), 5126, "VEC4", len(values))

    acc = {
        "pos": add_vec3(positions),
        "nrm": add_vec3(normals),
        "idx": add_u16(indices),
        "joint": add_joints(joints),
        "weight": add_weights(weights),
        "morph": add_vec3(morph_up),
    }
    ibm = struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
    acc["ibm"] = add_accessor(binb.add(ibm), 5126, "MAT4", 1)

    gltf = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": "Root", "children": [1, 2, 3]},
            {"name": "hips", "children": []},
            {"name": "Body", "mesh": 0, "skin": 0},
            # 弹簧骨链条：hair -> hairTip。
            {"name": "hair", "translation": [0.0, 0.7, 0.0], "children": [4]},
            {"name": "hairTip", "translation": [0.0, 0.1, 0.0]},
        ],
        "meshes": [{
            "name": "BodyMesh",
            "primitives": [{
                "attributes": {
                    "POSITION": acc["pos"],
                    "NORMAL": acc["nrm"],
                    "JOINTS_0": acc["joint"],
                    "WEIGHTS_0": acc["weight"],
                },
                "indices": acc["idx"],
                "material": 0,
                "mode": 4,
                # VRM 0.x 的表情靠 (mesh, index) 引用 morph 目标，所以这里必须真的有 target。
                "targets": [{"POSITION": acc["morph"]}],
            }],
            "extras": {"targetNames": ["Frown"]},
        }],
        "skins": [{"joints": [1], "inverseBindMatrices": acc["ibm"], "skeleton": 1}],
        "materials": [{
            "name": "OldMaterial",
            "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.8, 0.8, 1.0]},
            "extensions": {
                "VRMC_materials_mtoon": {"shadeColorFactor": [0.3, 0.3, 0.3, 1.0]}
            },
        }],
        "extensionsUsed": ["VRM"],
        "extensions": {
            "VRM": {
                "meta": {
                    "name": "PPet VRM0 测试模型",
                    "version": "0.0",
                    "author": "PPet",
                    "licenseUrl": "https://example.com/license",
                },
                # VRM 0.x 常见 humanBones 数组写法。
                "humanoid": {
                    "humanBones": [{"bone": "hips", "node": 1}],
                },
                # blendShapeGroups 是 VRM 0.x 导出器（UniVRM 等）的实际写法：
                # binds[].mesh 是 glTF mesh 下标，binds[].index 是 mesh 内 morph 目标下标，
                # binds[].weight 是 0~100 的百分比。
                "blendShapeMaster": {
                    "blendShapes": [],
                    "blendShapeGroups": [
                        {
                            "name": "Frown",
                            "presetName": "sorrow",
                            "isBinary": False,
                            "binds": [{"mesh": 0, "index": 0, "weight": 100.0}],
                        },
                        {
                            "name": "Joy",
                            "presetName": "joy",
                            "isBinary": False,
                            "binds": [{"mesh": 0, "index": 0, "weight": 75.0}],
                        },
                        {
                            "name": "Blink",
                            "presetName": "blink",
                            "isBinary": True,
                            "binds": [{"mesh": 0, "index": 0, "weight": 100.0}],
                        },
                    ],
                },
                # VRM 0.x 的弹簧骨：secondaryAnimation.boneGroups[]。
                # bones[] 是链条节点数组，colliderGroups[] 是碰撞组下标，
                # 碰撞组内的 colliders[] 是内联对象（offset 是 {x,y,z}）。
                "secondaryAnimation": {
                    "boneGroups": [{
                        "name": "hair",
                        "bones": [3, 4],
                        "colliderGroups": [0],
                        "stiffiness": 1.0,
                        "dragForce": 0.5,
                        "gravityPower": 0.0,
                        "gravityDir": {"x": 0.0, "y": -1.0, "z": 0.0},
                        "hitRadius": 0.02,
                    }],
                    "colliderGroups": [{
                        "node": 1,
                        "colliders": [{"offset": {"x": 0.0, "y": 0.0, "z": 0.0},
                                       "radius": 0.05}],
                    }],
                },
            }
        },
    }
    gltf["accessors"] = accessors
    gltf["bufferViews"] = binb.buffer_views
    gltf["buffers"] = [{"byteLength": len(binb.data)}]
    return gltf, bytes(binb.data)


# ---------------------------------------------------------------------------
# 健壮性素材
# ---------------------------------------------------------------------------


def write_broken(outdir):
    """生成各种畸形文件，用于验证「返回错误而不是崩溃」。"""
    written = []

    # 1) 截断文件：取合法 VRM 的前三分之一。
    gltf, binary = build_vrm10()
    full_path = os.path.join(outdir, "_tmp_full.vrm")
    write_glb(full_path, gltf, binary)
    with open(full_path, "rb") as f:
        full = f.read()
    os.remove(full_path)
    truncated = os.path.join(outdir, "broken_truncated.vrm")
    with open(truncated, "wb") as f:
        f.write(full[: max(16, len(full) // 3)])
    written.append(truncated)

    # 2) 随机字节。
    random_path = os.path.join(outdir, "broken_random.vrm")
    with open(random_path, "wb") as f:
        f.write(bytes((i * 37 + 11) & 0xFF for i in range(512)))
    written.append(random_path)

    # 3) GLB 头标识错误。
    badmagic = os.path.join(outdir, "broken_badmagic.vrm")
    data = bytearray(full)
    data[0:4] = b"VRMF"
    with open(badmagic, "wb") as f:
        f.write(data)
    written.append(badmagic)

    # 4) JSON 语法错误：保留 GLB 头，但把 JSON chunk 换成坏内容。
    badjson = os.path.join(outdir, "broken_badjson.vrm")
    bad_json = b'{"asset":{"version":"2.0",},'
    while len(bad_json) % 4 != 0:
        bad_json += b" "
    body = bytearray()
    body += struct.pack("<II", len(bad_json), CHUNK_JSON)
    body += bad_json
    total = 12 + len(body)
    out = bytearray()
    out += struct.pack("<III", GLB_MAGIC, 2, total)
    out += body
    with open(badjson, "wb") as f:
        f.write(out)
    written.append(badjson)

    # 5) 畸形 JSON：索引越界 / 负数 / 循环节点引用。
    broken_json = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            # 节点 0 与节点 1 互相引用 -> 成环。
            {"name": "a", "children": [1], "mesh": 0, "skin": 0},
            {"name": "b", "children": [0]},
        ],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 999},
                "indices": 1000,
                "material": 999,
                "mode": 4,
                "targets": [{"POSITION": 12345}],
            }]
        }],
        "skins": [{"joints": [999, 1000], "inverseBindMatrices": 888}],
        "materials": [{
            "name": "bad",
            "pbrMetallicRoughness": {"baseColorTexture": {"index": 4242}},
            "extensions": {"VRMC_materials_mtoon": {"shadeMultiplyTexture": {"index": 77}}},
        }],
        "animations": [{
            "samplers": [{"input": 555, "output": 666, "interpolation": "LINEAR"}],
            "channels": [{"sampler": 888, "target": {"node": 999, "path": "rotation"}}],
        }],
        "extensions": {
            "VRMC_vrm": {
                "meta": {"name": "broken"},
                "humanoid": {"humanBones": {"head": {"node": 9999}}},
                "expressions": {
                    "preset": {
                        "happy": {"morphTargetBinds": [{"node": 88888, "index": 44444,
                                                       "weight": 1.0}]}
                    }
                },
            },
            "VRMC_springBone": {
                "colliderGroups": [{"node": 999, "colliders": [999]}],
                "colliders": [{"node": 999, "radius": 1.0}],
                "springs": [{"jointNode": 999, "colliderGroups": [999]}],
            },
        },
    }
    oob = os.path.join(outdir, "broken_oob.vrm")
    write_glb(oob, broken_json, b"")
    written.append(oob)

    # 6) accessor 的 count 超出上限（模拟恶意文件）。
    count_json = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": 4}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 4}],
        "accessors": [{
            "bufferView": 0,
            "componentType": 5126,
            "count": 999999999,
            "type": "VEC3",
        }],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "mode": 4}]}],
    }
    count_path = os.path.join(outdir, "broken_count.vrm")
    write_glb(count_path, count_json, b"\x00\x00\x00\x00")
    written.append(count_path)

    # 7) JSON 数值溢出。
    #
    # nlohmann 在遇到超出 double 表示范围的字面量（例如 -4E371138828673793）时
    # 抛的是 out_of_range 而不是 parse_error，
    # 解析器必须捕获 json::exception 的基类，否则会 std::terminate。
    # 这个文件就是在 fuzz 真实样本时实际崩溃的输入，特意固化成回归用例。
    overflow_json = ('{"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],'
                 '"nodes":[{"name":"Root",'
                 '"translation":[-4E371138828673793,0.0,0.0]}]}')
    overflow_path = os.path.join(outdir, "broken_numoverflow.vrm")
    write_glb_raw_json(overflow_path, overflow_json, b"")
    written.append(overflow_path)

    return written


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description="生成 VRM 测试素材")
    parser.add_argument("--outdir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                         "testdata"))
    args = parser.parse_args()
    outdir = args.outdir
    os.makedirs(outdir, exist_ok=True)

    gltf, binary = build_vrm10()
    path = os.path.join(outdir, "test.vrm")
    size = write_glb(path, gltf, binary)
    print("生成 %s（%d 字节）" % (path, size))

    gltf0, binary0 = build_vrm0()
    path0 = os.path.join(outdir, "test_vrm0.vrm")
    size0 = write_glb(path0, gltf0, binary0)
    print("生成 %s（%d 字节）" % (path0, size0))

    for p in write_broken(outdir):
        print("生成 %s（%d 字节）" % (p, os.path.getsize(p)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
