// PPet - C++ 移植版
// Live2D 动作（motion3.json）解析与播放。
//
// 【为什么自己实现】
//   Cubism Core 层不提供动作播放（csmSetupMotion / csmStartMotion 属于
//   Cubism Framework 层）。本项目不引入整个 Framework（它比 Core 重得多，
//   且与本项目的 OpenGL 渲染管线耦合很深），因此自己实现动作解析与插值。
//
// 【motion3.json 格式】
//   {
//     "Version": 3,
//     "Meta": { "Duration": 3.0, "Fps": 30.0, "CurveCount": 2, "ParameterCount": 2 },
//     "Curves": [
//       {
//         "Target": "Parameter",
//         "Id": "ParamAngleX",
//         "Curve": [ { "Time": 0.0, "Value": 0.0, "In": "linear", "Out": "linear" },
//                    { "Time": 1.5, "Value": 15.0, "In": "linear", "Out": "linear" } ]
//       },
//       { "Target": "Part", "Id": "ArmL", "Curve": [ ... ] }
//     ]
//   }
//
//   每个关键帧的 Time / Value 是绝对时间与绝对值；
//   In / Out 描述**从该关键帧出发**的插值方式：
//     - "linear"：线性
//     - "stepped"：阶跃（保持当前值直到下一个关键帧）
//     - "bezier"：三次贝塞尔，控制点由 Bezier 字段给出
#pragma once

#include <string>
#include <vector>

namespace ppet::live2d {

/// 插值方式。
enum class CurveType {
    Linear,  ///< 线性
    Stepped, ///< 阶跃
    Bezier,  ///< 三次贝塞尔
};

/// 关键帧。
struct MotionKey {
    float time = 0.0f;     ///< 时间（秒）
    float value = 0.0f;    ///< 值
    CurveType inType = CurveType::Linear;  ///< 从上一帧到本帧的插值方式
    CurveType outType = CurveType::Linear; ///< 从本帧到下一帧的插值方式
    /// 贝塞尔控制点（4 个：x1,y1,x2,y2），仅当插值方式为 Bezier 时有效。
    /// 控制点横坐标归一化到 [0,1]（相对本关键帧到下一关键帧的时间段）。
    float bezier[4] = {0.0f, 0.0f, 1.0f, 1.0f};
};

/// 一条参数曲线。
struct MotionCurve {
    std::string target;    ///< "Parameter" 或 "Part"
    std::string id;        ///< 参数名或部件名
    std::vector<MotionKey> keys;
};

/// 一个动作文件。
struct Motion {
    float duration = 0.0f;      ///< 总时长（秒）
    float fps = 30.0f;          ///< 帧率
    /// 淡入/淡出时长（秒）。motion3.json 的 Meta.FadeInTime / FadeOutTime，
    /// 缺省 0 表示不淡入淡出。动作切换时用它们做参数混合，避免姿态跳变。
    float fadeInTime = 0.0f;
    float fadeOutTime = 0.0f;
    std::vector<MotionCurve> curves;

    /// 按时间采样，返回该时刻各曲线的值。
    /// @param time   当前时间（秒）
    /// @param values 输出：与 curves 一一对应，长度不足时补 0
    void Sample(float time, std::vector<float>& values) const;
};

/// 解析 motion3.json。
/// @param path   文件路径
/// @param out    解析结果
/// @param error  失败时写入中文原因
/// @return 成功返回 true。
bool ParseMotionFile(const std::string& path, Motion& out, std::string& error);

}  // namespace ppet::live2d
