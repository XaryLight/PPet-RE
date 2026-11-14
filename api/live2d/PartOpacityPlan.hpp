// PPet - C++ 移植版
// Live2D 部件不透明度（PartOpacity）曲线应用：纯逻辑，零依赖，可脱离 Cubism SDK 单测。
//
// 【为什么需要它】
//   motion3.json 的曲线除了作用于「参数」(Target="Parameter")，还可作用于
//   「部件不透明度」(Target="PartOpacity"，旧版写作 "Part")。很多 Live2D 模型
//   把几套互斥的身体 / 换装 / 姿势做进同一个 moc3，靠这类曲线把不用的那套
//   部件不透明度置 0 来切换（例如 aimierbeierding_3 的 PartPoseStand /
//   PartPoseDefault 两套完整身体）。
//
//   渲染层早期版本只处理 Parameter 曲线，把 PartOpacity 曲线整体丢弃
//   (`if (curve.target != "Parameter") continue;`)，于是所有互斥部件都保持
//   默认不透明度 1.0 一起画出来 —— 用户看到的「渲染出多个身体」，且因为
//   Parameter 曲线仍生效，这些身体都会响应动作；视线追踪只写 ParamAngleX/Y
//   等标准参数，仅绑定了这些参数的身体会随鼠标偏转，于是「其中一个或多个
//   不随鼠标偏转」。
//
// 【本模块的职责】
//   把「一帧里多个动作层（含淡入淡出权重）对部件不透明度的采样」合并成
//   每个部件的最终不透明度。GL / SDK 层只负责把结果写进
//   csmGetPartOpacities() 缓冲区；Cubism Core 在 csmUpdateModel() 时会自动
//   把部件不透明度传播到其下所有 drawable 的 opacity，渲染计划里
//   `opacity <= 0` 的过滤会自动剔除被隐藏的部件。
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace ppet::live2d {

/// motion3.json 的一条曲线 Target 是否作用于「部件不透明度」。
/// 官方两种写法：Cubism 4 的 "Part"、Cubism 5 的 "PartOpacity"。
inline bool IsPartOpacityTarget(const std::string& target) {
    return target == "PartOpacity" || target == "Part";
}

/// 一条部件不透明度曲线在某一时刻的采样值。
struct PartOpacitySample {
    std::string partId;
    float value = 1.0f;
};

/// 一层生效动作（对应 MotionQueue 的一个 blend），按入队顺序排列，后者覆盖前者。
struct PartOpacityLayer {
    float weight = 1.0f;
    std::vector<PartOpacitySample> samples;
};

using PartOpacityMap = std::map<std::string, float>;

/// 从所有动作的「首帧采样」推导静止姿势（默认部件不透明度）。
///
/// 对每个部件取出现次数最多的值（互斥姿势里被大多数动作选中的那一套）。
/// 这样即使动作队列为空（刚加载 / 动作间隙 / 全部淡出），也不会把几套
/// 互斥身体一起显示出来。
///
/// 平票时取**较大**的值（更倾向「显示」而非「隐藏」），避免因单个样本
/// 抖动导致整块部件消失。
inline PartOpacityMap DeriveDefaultPartOpacity(
    const std::vector<std::vector<PartOpacitySample>>& firstFrames) {
    std::map<std::string, std::vector<float>> votes;
    for (const auto& frame : firstFrames) {
        for (const auto& s : frame) {
            if (!s.partId.empty()) votes[s.partId].push_back(s.value);
        }
    }

    PartOpacityMap result;
    for (auto& [id, values] : votes) {
        std::sort(values.begin(), values.end());
        float best = values.front();
        int bestCount = 0;
        for (std::size_t i = 0; i < values.size();) {
            std::size_t j = i;
            while (j < values.size() && values[j] == values[i]) ++j;
            const int count = static_cast<int>(j - i);
            if (count > bestCount || (count == bestCount && values[i] > best)) {
                bestCount = count;
                best = values[i];
            }
            i = j;
        }
        result[id] = best;
    }
    return result;
}

/// 计算本帧每个部件的最终不透明度。
///
/// 从默认姿势（baseline）出发，按动作入队顺序逐层混合，混合规则与参数曲线
/// 保持一致（官方 Framework 的 DoUpdateParameters）：
///     v = v + (target - v) * weight
/// 未出现在 baseline 的部件以 1.0（完全显示）为基线。
///
/// @param baseline 静止姿势（DeriveDefaultPartOpacity 的结果）
/// @param layers   本帧生效的动作层（含各自权重）
/// @return 部件 id -> 最终不透明度，调用方只需把这些项写进 part opacity 缓冲
inline PartOpacityMap BuildPartOpacityPlan(const PartOpacityMap& baseline,
                                           const std::vector<PartOpacityLayer>& layers) {
    PartOpacityMap result = baseline;
    for (const auto& layer : layers) {
        if (layer.weight <= 0.0f) continue;
        const float w = std::min(1.0f, layer.weight);
        for (const auto& s : layer.samples) {
            if (s.partId.empty()) continue;
            const auto it = result.find(s.partId);
            const float base = (it != result.end()) ? it->second : 1.0f;
            result[s.partId] = base + (s.value - base) * w;
        }
    }
    return result;
}

}  // namespace ppet::live2d
