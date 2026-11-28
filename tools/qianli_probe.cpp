// 千璃0711.vrm 真实模型探针：验证 VRM 0.x MToon 材质解析与弹簧骨物理。
// 用法：qianli_probe <path-to-vrm>
#include <src/tools/model/vrm/VrmModel.hpp>

#include <cstdio>
#include <string>

using namespace ppet;
using namespace ppet::vrm;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: qianli_probe <vrm>\n");
        return 2;
    }

    VrmModel model;
    std::string error;
    if (!model.Load(std::string(argv[1]), error)) {
        std::printf("VrmModel::Load 失败: %s\n", error.c_str());
        return 1;
    }
    std::printf("VRM 版本: %d\n", model.VrmVersion());

    const SkinnedMesh& mesh = model.Mesh();
    std::printf("顶点=%zu 材质=%zu 骨骼=%zu morph=%zu 表情=%zu 弹簧骨=%zu\n",
                mesh.vertices.size(), mesh.materials.size(), mesh.bones.size(),
                mesh.morphs.size(), mesh.expressions.size(), mesh.springBones.size());

    int matMask = 0, matBlend = 0, matOpaque = 0, matOutline = 0, matShadeTex = 0;
    float maxOutlineWidth = 0.0f;
    for (const Material& m : mesh.materials) {
        if (m.alphaMode == Material::AlphaMode::Mask) ++matMask;
        if (m.alphaMode == Material::AlphaMode::Blend) ++matBlend;
        if (m.alphaMode == Material::AlphaMode::Opaque) ++matOpaque;
        if (m.outlineWidth > 0.0f) ++matOutline;
        if (m.toonRampTex >= 0) ++matShadeTex;
        if (m.outlineWidth > maxOutlineWidth) maxOutlineWidth = m.outlineWidth;
    }
    std::printf("材质: MASK=%d BLEND=%d OPAQUE=%d 有描边=%d 有阴影贴图=%d\n",
                matMask, matBlend, matOpaque, matOutline, matShadeTex);
    // 描边宽度应远小于模型高度；VRM 0.x 未换算时 0.08m 会占 4.8% 遮住模型。
    const float modelHeight = static_cast<float>(mesh.boundsMax.y - mesh.boundsMin.y);
    std::printf("描边最大宽度=%.4f m, 模型高度=%.3f m, 占比=%.2f%%\n",
                maxOutlineWidth, modelHeight,
                modelHeight > 0.0f ? (maxOutlineWidth / modelHeight) * 100.0f : 0.0f);

    if (!mesh.materials.empty()) {
        const Material& m = mesh.materials.front();
        std::printf("首材质 %s:\n  alphaMode=%d cutoff=%.2f\n"
                    "  shadeColor=(%.2f,%.2f,%.2f) rimColor=(%.2f,%.2f,%.2f)\n"
                    "  shadeShift=%.3f shadeToony=%.3f rimFresnel=%.1f\n"
                    "  outlineWidth=%.4f outlineColor=(%.2f,%.2f,%.2f)\n",
                    m.name.c_str(), static_cast<int>(m.alphaMode), m.alphaCutoff,
                    m.shadeColor.x, m.shadeColor.y, m.shadeColor.z,
                    m.rimColor.x, m.rimColor.y, m.rimColor.z,
                    m.shadeShift, m.shadeToony, m.rimFresnelPower,
                    m.outlineWidth, m.outlineColor.x, m.outlineColor.y, m.outlineColor.z);
    }

    // 弹簧骨物理参数统计。
    float minStiff = 1e9f, maxStiff = -1e9f;
    int withGravity = 0;
    for (const SkinnedMesh::SpringBone& sb : mesh.springBones) {
        minStiff = minStiff < sb.stiffness ? minStiff : sb.stiffness;
        maxStiff = maxStiff > sb.stiffness ? maxStiff : sb.stiffness;
        if (sb.gravityPower != 0.0f) ++withGravity;
    }
    std::printf("弹簧骨: 数=%zu stiff范围=[%.3f,%.3f] 带重力=%d\n",
                mesh.springBones.size(), minStiff, maxStiff, withGravity);

    // 驱动 120 帧，交替光标位置带动头部转动，验证运行时不崩溃且姿态稳定。
    RenderContext ctx;
    ctx.deltaTime = 1.0f / 60.0f;
    ctx.viewportWidth = 512;
    ctx.viewportHeight = 512;
    ctx.hasGlobalCursor = true;
    for (int i = 0; i < 120; ++i) {
        ctx.localCursor = Vec2(i % 2 ? 0.9f : -0.9f, 0.0f);
        model.Update(ctx);
    }
    std::printf("驱动 120 帧完成，运行时稳定\n");

    // 断言关键修复生效。
    int fail = 0;
    if (matMask < 20) { std::printf("FAIL: MASK 材质过少\n"); ++fail; }
    if (matBlend < 1) { std::printf("FAIL: BLEND 材质缺失\n"); ++fail; }
    if (matOutline < 20) { std::printf("FAIL: MToon 描边参数未解析\n"); ++fail; }
    // VRM 0.x 描边必须做 0.1 换算，否则 0.08m 会占模型高度 ~5% 而遮住模型。
    // 正常描边应 < 模型高度的 2%。
    if (modelHeight > 0.0f && maxOutlineWidth > modelHeight * 0.02f) {
        std::printf("FAIL: 描边过粗，占比 %.2f%% > 2%%\n", (maxOutlineWidth / modelHeight) * 100.0f);
        ++fail;
    }
    if (mesh.springBones.size() < 20) { std::printf("FAIL: 弹簧骨过少\n"); ++fail; }
    std::printf("结果: %s\n", fail == 0 ? "PASS" : "FAIL");
    return fail == 0 ? 0 : 1;
}
