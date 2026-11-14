// PPet - C++ 移植版
// 智能动作识别（需求 2）：扫描模型动作文件 / 动作组，把「文件名 / 组名的语义」
// 归类成 idle（待机）/ touch（触摸互动）/ expression（表情），并产出
// 「区域 -> 触摸动作」映射，供随机待机与分区触摸使用。
//
// 设计要点：
//   - 纯逻辑、零依赖（只复用 MotionDirector 的分类辅助函数），可单测；
//   - 同时支持「按文件名识别」（许多国内模型把全部动作塞进空名组，组名无语义）
//     与「按动作组识别」（标准 Cubism model3.json 的 Motions 分组）两种来源；
//   - 输出统一为 MotionSetAnalysis，上层（随机待机 / 触摸分发）只消费这份结果。
#pragma once

#include <api/live2d/MotionDirector.hpp>

#include <map>
#include <string>
#include <vector>

namespace ppet::live2d {

/// 对一套动作的识别结果。
struct MotionSetAnalysis {
    bool hasIdle = false;            ///< 是否存在可用待机动作
    int idleCount = 0;               ///< 待机动作数
    int interactionCount = 0;        ///< 触摸/互动动作数
    int expressionCount = 0;         ///< 表情动作数
    int otherCount = 0;              ///< 其它/未知

    /// 区域 -> 该区域关联的触摸动作名（如 "head" -> {"touch_head","tap_head"}）。
    std::map<std::string, std::vector<std::string>> touchByRegion;

    /// 识别出的待机动作名（供随机播放）。
    std::vector<std::string> idleNames;

    /// 识别出的触摸动作名（按区域平铺，供触摸分发）。
    std::vector<std::string> touchNames;

    /// 人类可读摘要（诊断面板 / 日志用）。
    std::string Summary() const;
};

/// 按动作文件名识别（需求 2 主路径：空名组、国内模型包的常见形态）。
///
/// 兼容 guanghui_7 这类「47 个动作文件、组名多为空」的模型：每个文件跑
/// ClassifyMotionFile + HitAreaFromMotionFile，再汇总成统一分析。
/// @param fileNames 动作文件名（可带目录与 .motion3.json 后缀）
MotionSetAnalysis AnalyzeMotionFiles(const std::vector<std::string>& fileNames);

/// 按已分类的动作组识别（标准 model3.json 分组场景）。
/// @param groups MotionDirector::Configure 用的动作组列表
MotionSetAnalysis AnalyzeMotionGroups(const std::vector<MotionGroup>& groups);

}  // namespace ppet::live2d
