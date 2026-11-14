// PPet - C++ 移植版
// 渲染计划（CubismRenderPlan）单元测试：把官方 OpenGL 渲染器的决策链
// 从 GL 代码里抽出来单测，防止「渲染偏白」「遮挡错乱」再次发生。
//
// 覆盖：
//   1. 官方三套 glBlendFuncSeparate（尤其 Add/Mult 的 Alpha = ZERO/ONE）
//   2. premultiplied GetModelColorWithOpacity：RGB 与 A 一起乘 opacity
//   3. 按 renderOrder 稳定排序、相同 order 保持下标顺序
//   4. 跳过不可见 / 全透明 / 无贴图部件
//   5. 遮罩标记透传
//   6. 【回归】兼作遮罩源的可见部件必须照常绘制（脸/腿透明块 bug）
#include <api/live2d/CubismRenderPlan.hpp>

#include <cstdio>

namespace {

int failures = 0;
int checks = 0;

void Check(bool cond, const char* what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  [FAIL] %s\n", what);
    }
}
bool Near(float a, float b, float eps = 1e-5f) {
    return (a - b) <= eps && (b - a) <= eps;
}

using namespace ppet::live2d;

DrawableDesc Make(int index, int order) {
    DrawableDesc d;
    d.index = index;
    d.renderOrder = order;
    return d;
}

void TestBlendFuncTable() {
    std::printf("-- 官方混合模式表 --\n");
    const BlendFunc normal = BlendFuncFor(DrawableBlend::Normal);
    Check(normal.srcRgb == BlendFactor::One &&
              normal.dstRgb == BlendFactor::OneMinusSrcAlpha &&
              normal.srcA == BlendFactor::One &&
              normal.dstA == BlendFactor::OneMinusSrcAlpha,
          "Normal: RGB ONE/1-SRC_A, A ONE/1-SRC_A");

    const BlendFunc add = BlendFuncFor(DrawableBlend::Additive);
    Check(add.srcRgb == BlendFactor::One && add.dstRgb == BlendFactor::One &&
              add.srcA == BlendFactor::Zero && add.dstA == BlendFactor::One,
          "Add: RGB ONE/ONE, A ZERO/ONE");

    const BlendFunc mult = BlendFuncFor(DrawableBlend::Multiplicative);
    Check(mult.srcRgb == BlendFactor::DstColor &&
              mult.dstRgb == BlendFactor::OneMinusSrcAlpha &&
              mult.srcA == BlendFactor::Zero && mult.dstA == BlendFactor::One,
          "Mult: RGB DST_COLOR/1-SRC_A, A ZERO/ONE");
}

void TestModelColorWithOpacity() {
    std::printf("-- premultiplied 颜色 --\n");
    const ModelColor full = ModelColorWithOpacity(1.0f);
    Check(Near(full.r, 1.0f) && Near(full.g, 1.0f) && Near(full.b, 1.0f) &&
              Near(full.a, 1.0f),
          "opacity=1 -> (1,1,1,1)");

    const ModelColor half = ModelColorWithOpacity(0.5f);
    Check(Near(half.r, 0.5f) && Near(half.g, 0.5f) && Near(half.b, 0.5f) &&
              Near(half.a, 0.5f),
          "opacity=0.5 -> RGB 也乘 0.5（偏白 bug 的关键）");

    const ModelColor zero = ModelColorWithOpacity(0.0f);
    Check(Near(zero.r, 0.0f) && Near(zero.a, 0.0f), "opacity=0 -> 全 0");
    Check(Near(ModelColorWithOpacity(2.0f).a, 1.0f), "opacity 上钳到 1");
    Check(Near(ModelColorWithOpacity(-1.0f).a, 0.0f), "opacity 下钳到 0");

    // 模型整体带颜色时，RGB 按 a 缩放。
    const ModelColor tinted = ModelColorWithOpacity(0.5f, 1.0f, 0.8f, 0.6f);
    Check(Near(tinted.r, 0.5f) && Near(tinted.g, 0.4f) && Near(tinted.b, 0.3f),
          "带模型色时 RGB 分别乘 a");
}

void TestOrdering() {
    std::printf("-- renderOrder 稳定排序 --\n");
    std::vector<DrawableDesc> descs = {Make(0, 30), Make(1, 10), Make(2, 20),
                                       Make(3, 10), Make(4, 0)};
    const auto plan = BuildRenderPlan(descs);
    Check(plan.size() == 5, "5 个部件全部入计划");
    Check(plan[0].drawableIndex == 4, "order 0 最先");
    Check(plan[1].drawableIndex == 1 && plan[2].drawableIndex == 3,
          "相同 order 保持原下标顺序（稳定）");
    Check(plan[3].drawableIndex == 2, "order 20 在中间");
    Check(plan[4].drawableIndex == 0, "order 30 最后");
}

void TestFiltering() {
    std::printf("-- 跳过无效部件 --\n");
    std::vector<DrawableDesc> descs;
    descs.push_back(Make(0, 0));  // 正常
    DrawableDesc invisible = Make(1, 1);
    invisible.visible = false;
    descs.push_back(invisible);
    DrawableDesc transparent = Make(2, 2);
    transparent.opacity = 0.0f;
    descs.push_back(transparent);
    // 兼作遮罩源、但自身可见且有贴图：必须保留（不能再被 isMaskDrawable 剔除）。
    DrawableDesc maskPart = Make(3, 3);
    maskPart.isMaskDrawable = true;
    descs.push_back(maskPart);
    DrawableDesc noTexture = Make(4, 4);
    noTexture.hasOwnTexture = false;
    descs.push_back(noTexture);
    DrawableDesc negativeIndex = Make(-1, 5);
    descs.push_back(negativeIndex);

    const auto plan = BuildRenderPlan(descs);
    // 正常部件(0) + 可见遮罩源(3) 都要画；无贴图(4)、不可见(1)、
    // 全透明(2)、负下标 被剔除。
    Check(plan.size() == 2, "正常部件 + 可见遮罩源都要保留");
    Check(plan[0].drawableIndex == 0 && plan[1].drawableIndex == 3,
          "可见遮罩源按 renderOrder 正常入计划");
}

void TestMaskSourceRegression() {
    std::printf("-- 遮罩源可见性回归（脸/腿透明块）--\n");
    std::vector<DrawableDesc> descs;
    DrawableDesc face = Make(0, 0);
    face.isMaskDrawable = true;   // 被眼睛当遮罩
    face.useMask = false;
    DrawableDesc eye = Make(1, 1);
    eye.useMask = true;           // 眼睛被脸裁剪
    eye.maskCount = 1;
    DrawableDesc pureMask = Make(2, 2);
    pureMask.isMaskDrawable = true;
    pureMask.hasOwnTexture = false;  // 纯几何遮罩：画出来是白块，必须跳过
    descs.push_back(face);
    descs.push_back(eye);
    descs.push_back(pureMask);

    const auto plan = BuildRenderPlan(descs);
    Check(plan.size() == 2, "遮罩源(脸)与被裁剪部件(眼)都绘制，纯几何遮罩跳过");
    Check(plan[0].drawableIndex == 0, "遮罩源按 renderOrder 先画");
    Check(plan[1].drawableIndex == 1 && plan[1].maskCount == 1,
          "被裁剪部件的遮罩数量透传到 GL 层");
}

void TestOpacityAndBlendInPlan() {
    std::printf("-- 计划里的颜色与混合 --\n");
    std::vector<DrawableDesc> descs;
    DrawableDesc normal = Make(0, 0);
    normal.opacity = 0.25f;
    descs.push_back(normal);
    DrawableDesc add = Make(1, 1);
    add.blend = DrawableBlend::Additive;
    add.opacity = 0.5f;
    descs.push_back(add);
    DrawableDesc masked = Make(2, 2);
    masked.useMask = true;
    masked.invertMask = true;
    descs.push_back(masked);

    // 模型整体 0.5 不透明度：最终 opacity = 0.25 * 0.5 = 0.125
    const auto plan = BuildRenderPlan(descs, 0.5f);
    Check(plan.size() == 3, "3 个部件");
    Check(Near(plan[0].baseColor.a, 0.125f) && Near(plan[0].baseColor.r, 0.125f),
          "opacity 与 modelOpacity 相乘，RGB 同步缩放");
    Check(plan[1].blend == DrawableBlend::Additive &&
              plan[1].blendFunc.srcA == BlendFactor::Zero,
          "加法部件 Alpha 用 ZERO/ONE");
    Check(plan[2].useMask && plan[2].invertMask, "遮罩标记透传");
}

}  // namespace

int main() {
    std::printf("=== CubismRenderPlan 单元测试 ===\n");
    TestBlendFuncTable();
    TestModelColorWithOpacity();
    TestOrdering();
    TestFiltering();
    TestOpacityAndBlendInPlan();
    TestMaskSourceRegression();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
