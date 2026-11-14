// PPet - C++ 移植版
// Live2D 部件不透明度（PartOpacity）合并逻辑单元测试（Q1）：
//   - Target 识别："PartOpacity" / 旧版 "Part" 才算部件曲线
//   - 默认姿势推导：从所有动作首帧里选多数派（互斥身体只留一套）
//   - 每帧计划：静止 / 单层切换 / 淡入淡出混合 / 多层叠加 / 权重钳制
//
// 回归目标：aimierbeierding_3 这类「两套完整身体做进同一个 moc3」的模型，
// 不能因为部件曲线被丢弃而同时把两套都画出来（用户看到的「多个身体」）。
//
// 纯逻辑、零依赖（不碰 Cubism SDK / 窗口 / GL），三种 CMake 配置均可编译运行。
#include <api/live2d/PartOpacityPlan.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
    if (!cond) {
        std::printf("[FAIL] %s\n", msg);
        ++g_failures;
    }
}

bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

using ppet::live2d::BuildPartOpacityPlan;
using ppet::live2d::DeriveDefaultPartOpacity;
using ppet::live2d::IsPartOpacityTarget;
using ppet::live2d::PartOpacityLayer;
using ppet::live2d::PartOpacityMap;
using ppet::live2d::PartOpacitySample;

/// 构造 aimierbeierding_3 的真实姿势：Stand / Default 两套互斥身体。
std::vector<PartOpacitySample> Pose(float stand, float def) {
    return {{"PartPoseStand", stand}, {"PartPoseDefault", def}};
}

float Get(const PartOpacityMap& m, const std::string& id) {
    const auto it = m.find(id);
    return (it == m.end()) ? -1.0f : it->second;
}

/// 造一层「把姿势切到 Stand=stand, Default=def」的动作层。
PartOpacityLayer Layer(float weight, float stand, float def) {
    PartOpacityLayer layer;
    layer.weight = weight;
    layer.samples = Pose(stand, def);
    return layer;
}

}  // namespace

int main() {
    // --- Target 识别 -------------------------------------------------------
    Check(IsPartOpacityTarget("PartOpacity"), "Target=PartOpacity 识别");
    Check(IsPartOpacityTarget("Part"), "Target=Part（旧版写法）识别");
    Check(!IsPartOpacityTarget("Parameter"), "Target=Parameter 不误判为部件曲线");
    Check(!IsPartOpacityTarget("Model"), "Target=Model 不误判为部件曲线");
    Check(!IsPartOpacityTarget(""), "空 Target 不误判");
    Check(!IsPartOpacityTarget("partopacity"), "Target 大小写敏感");

    // --- 默认姿势推导 -----------------------------------------------------
    Check(DeriveDefaultPartOpacity({}).empty(), "空输入 -> 空默认姿势");

    {
        // aimierbeierding_3 实测：14 个动作里 12 个 Stand=0/Default=1，
        // home / login 两个动作相反。多数派应选中「默认站姿」这一套。
        std::vector<std::vector<PartOpacitySample>> frames;
        for (int i = 0; i < 12; ++i) frames.push_back(Pose(0.0f, 1.0f));
        for (int i = 0; i < 2; ++i) frames.push_back(Pose(1.0f, 0.0f));
        const PartOpacityMap def = DeriveDefaultPartOpacity(frames);
        Check(Near(Get(def, "PartPoseStand"), 0.0f), "多数派：Stand 默认隐藏");
        Check(Near(Get(def, "PartPoseDefault"), 1.0f), "多数派：Default 默认显示");
        Check(def.size() == 2, "默认姿势恰好含这两个部件组");
    }

    {
        // 平票取较大值（防御性兜底：真实模型不会平票，但也不能因抖动整块消失）。
        const std::vector<std::vector<PartOpacitySample>> frames = {
            Pose(0.0f, 1.0f), Pose(1.0f, 0.0f),
        };
        const PartOpacityMap def = DeriveDefaultPartOpacity(frames);
        Check(Near(Get(def, "PartPoseStand"), 1.0f), "平票取较大值：Stand=1");
        Check(Near(Get(def, "PartPoseDefault"), 1.0f), "平票取较大值：Default=1");
    }

    {
        // 空 partId 不参与投票，也不写进结果。
        const std::vector<std::vector<PartOpacitySample>> frames = {
            {{"", 0.0f}, {"PartPoseStand", 0.0f}},
        };
        const PartOpacityMap def = DeriveDefaultPartOpacity(frames);
        Check(def.size() == 1 && Near(Get(def, "PartPoseStand"), 0.0f),
              "空 partId 被忽略");
    }

    // --- 每帧合并计划 -----------------------------------------------------
    const PartOpacityMap baseline = {{"PartPoseStand", 0.0f},
                                     {"PartPoseDefault", 1.0f}};

    {
        // Q1 核心回归：动作队列为空（刚加载 / 动作间隙）也必须只显示一套。
        const PartOpacityMap plan = BuildPartOpacityPlan(baseline, {});
        Check(Near(Get(plan, "PartPoseStand"), 0.0f), "静止：Stand 隐藏");
        Check(Near(Get(plan, "PartPoseDefault"), 1.0f), "静止：Default 显示");
        Check(Get(plan, "PartPoseStand") + Get(plan, "PartPoseDefault") <= 1.0f + 1e-4f,
              "静止：两套身体不会同时显示");
    }

    {
        // 播放 home 动作（Stand=1, Default=0）时切到另一套。
        const std::vector<PartOpacityLayer> layers = {Layer(1.0f, 1.0f, 0.0f)};
        const PartOpacityMap plan = BuildPartOpacityPlan(baseline, layers);
        Check(Near(Get(plan, "PartPoseStand"), 1.0f), "home 动作：Stand 显示");
        Check(Near(Get(plan, "PartPoseDefault"), 0.0f), "home 动作：Default 隐藏");
    }

    {
        // 淡入 25%：v = base + (target - base) * w
        const std::vector<PartOpacityLayer> layers = {Layer(0.25f, 1.0f, 0.0f)};
        const PartOpacityMap plan = BuildPartOpacityPlan(baseline, layers);
        Check(Near(Get(plan, "PartPoseStand"), 0.25f), "淡入 25%：Stand=0.25");
        Check(Near(Get(plan, "PartPoseDefault"), 0.75f), "淡入 25%：Default=0.75");
    }

    {
        // 多层按入队顺序叠加（后者覆盖前者）。
        const std::vector<PartOpacityLayer> layers = {
            Layer(1.0f, 1.0f, 0.0f),
            Layer(0.5f, 0.0f, 1.0f),
        };
        const PartOpacityMap plan = BuildPartOpacityPlan(baseline, layers);
        Check(Near(Get(plan, "PartPoseStand"), 0.5f), "多层：Stand = 1 + (0-1)*0.5");
        Check(Near(Get(plan, "PartPoseDefault"), 0.5f), "多层：Default = 0 + (1-0)*0.5");
    }

    {
        const std::vector<PartOpacityLayer> layers = {Layer(0.0f, 1.0f, 0.0f),
                                                      Layer(-2.0f, 1.0f, 0.0f)};
        const PartOpacityMap plan = BuildPartOpacityPlan(baseline, layers);
        Check(Near(Get(plan, "PartPoseStand"), 0.0f), "weight<=0 的层被忽略");
    }

    {
        const std::vector<PartOpacityLayer> layers = {Layer(4.0f, 1.0f, 0.0f)};
        const PartOpacityMap plan = BuildPartOpacityPlan(baseline, layers);
        Check(Near(Get(plan, "PartPoseStand"), 1.0f), "weight>1 钳到 1，避免越界");
    }

    {
        // 只出现在个别动作、不在默认姿势里的部件：以 1.0（显示）为基线。
        PartOpacityLayer layer;
        layer.weight = 0.5f;
        layer.samples = {{"PartArmL", 0.0f}};
        const PartOpacityMap plan = BuildPartOpacityPlan({}, {layer});
        Check(Near(Get(plan, "PartArmL"), 0.5f), "新部件基线 1.0：1 + (0-1)*0.5");
    }

    {
        PartOpacityLayer layer;
        layer.weight = 1.0f;
        layer.samples = {{"", 0.0f}};
        const PartOpacityMap plan = BuildPartOpacityPlan(baseline, {layer});
        Check(plan.size() == baseline.size(), "空 partId 不引入新部件");
    }

    {
        // Q1 结论回归：交叉淡入的任意时刻，两套身体透明度之和恒为 1，
        // 绝不会出现「两套都完全显示」的重复身体。
        for (int step = 0; step <= 20; ++step) {
            const float w = step / 20.0f;
            const PartOpacityMap plan =
                BuildPartOpacityPlan(baseline, {Layer(w, 1.0f, 0.0f)});
            const float sum = Get(plan, "PartPoseStand") + Get(plan, "PartPoseDefault");
            Check(sum <= 1.0f + 1e-3f, "淡入全程两套身体透明度之和 <= 1");
        }
    }

    if (g_failures == 0) {
        std::printf("part_opacity_plan_unit: PASS (all assertions)\n");
        return 0;
    }
    std::printf("part_opacity_plan_unit: %d FAILED\n", g_failures);
    return 1;
}
