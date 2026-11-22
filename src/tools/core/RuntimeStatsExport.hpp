// PPet - C++ 移植版
// 运行时指标导出（报告 4.2.3）：把调试面板的 RuntimeStats / ModelDebugInfo
// 序列化成 JSON 或 CSV，便于长期性能分析与外部工具消费。
//
// 设计为纯 C++、零 Qt 依赖：
//   - 先产出扁平的 key -> value 指标表（BuildMetrics）；
//   - 再由 ToJson / ToCsv 格式化，两者都可脱离图形环境单测。
// JSON 手写转义（UTF-8 字节透传，仅转义 JSON 控制字符）；
// CSV 用 RFC 4180 双引号规则（含逗号/引号/换行的值加引号并双写引号）。
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <src/tools/core/Math.hpp>
#include <src/tools/model/Model.hpp>
#include <src/ui/DebugPanel.hpp>

namespace ppet {

using Metrics = std::vector<std::pair<std::string, std::string>>;

/// 采集一组扁平指标：模型身份/规模/渲染状态 + 运行时帧统计/光标/帧证据。
/// 数值统一用足够精度的十进制字符串，布尔用 "true"/"false"。
Metrics BuildMetrics(const ModelDebugInfo& model, const RuntimeStats& runtime);

/// 转义成 JSON 字符串内容（不含外层引号）。
std::string JsonEscape(const std::string& value);

/// 转义成 CSV 字段内容（按需加外层引号）。
std::string CsvEscape(const std::string& value);

/// 输出带缩进的 JSON 对象，键顺序与 metrics 一致。
std::string MetricsToJson(const Metrics& metrics);

/// 输出两行 CSV：表头 + 数据行。
std::string MetricsToCsv(const Metrics& metrics);

/// 把 JSON 转义结果还原，供单测验证往返一致性（生产代码不调用）。
std::string JsonUnescape(const std::string& escaped);

}  // namespace ppet
