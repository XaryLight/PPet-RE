// PPet - C++ 移植版
// Live2D 动作（motion3.json）解析与播放实现。
#include "model/live2d/Live2DMotion.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace ppet::live2d {
namespace {

/// 从 JSON 里取字符串字段，失败返回空串。
std::string JsonString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return std::string();
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

/// 解析插值方式。未知类型按 linear 处理。
CurveType ParseCurveType(const std::string& text) {
    if (text == "stepped") return CurveType::Stepped;
    if (text == "bezier") return CurveType::Bezier;
    return CurveType::Linear;
}

/// 计算三次贝塞尔曲线在 t 处的值。
///
/// 控制点为 (0,0) -> (x1,y1) -> (x2,y2) -> (1,1)。
/// 横坐标用于反解参数 t（牛顿迭代），纵坐标给出插值结果。
///
/// @param p  控制点 [x1, y1, x2, y2]
/// @param t  时间比例 [0,1]
float BezierValue(const float p[4], float t) {
    // 二次贝塞尔的 y 分量。
    const float mt = 1.0f - t;
    const float y = mt * mt * mt * 0.0f + 3.0f * mt * mt * t * p[1] +
                    3.0f * mt * t * t * p[3] + t * t * t * 1.0f;
    return y;
}

/// 计算贝塞尔曲线在给定 x 处的 t（横坐标反解）。
///
/// @param p  控制点 [x1, y1, x2, y2]
/// @param x  目标横坐标 [0,1]
float BezierTime(const float p[4], float x) {
    // 二分法足够：曲线段数少，且对精度要求不高（像素级）。
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 24; ++i) {
        const float t = (lo + hi) * 0.5f;
        const float mt = 1.0f - t;
        // 三次贝塞尔的 x 分量。
        const float cx = 3.0f * mt * mt * t * p[0] + 3.0f * mt * t * t * p[2] + t * t * t;
        if (cx < x) {
            lo = t;
        } else {
            hi = t;
        }
    }
    return (lo + hi) * 0.5f;
}

/// 在两个关键帧之间插值。
///
/// @param from 起始关键帧（提供 Out 方式与贝塞尔控制点）
/// @param to   结束关键帧
/// @param t    归一化时间 [0,1]
float Interpolate(const MotionKey& from, const MotionKey& to, float t) {
    switch (from.outType) {
        case CurveType::Stepped:
            return from.value;
        case CurveType::Bezier: {
            const float param = BezierTime(from.bezier, t);
            return BezierValue(from.bezier, param);
        }
        case CurveType::Linear:
        default:
            break;
    }
    return from.value + (to.value - from.value) * t;
}

}  // namespace

void Motion::Sample(float time, std::vector<float>& values) const {
    values.assign(curves.size(), 0.0f);
    if (curves.empty()) return;

    for (std::size_t c = 0; c < curves.size(); ++c) {
        const MotionCurve& curve = curves[c];
        if (curve.keys.empty()) continue;

        // 时间早于第一个关键帧：保持首值。
        if (time <= curve.keys.front().time) {
            values[c] = curve.keys.front().value;
            continue;
        }
        // 时间晚于最后一个关键帧：保持末值。
        if (time >= curve.keys.back().time) {
            values[c] = curve.keys.back().value;
            continue;
        }

        // 找到 time 落在哪两个关键帧之间。
        std::size_t index = 0;
        while (index + 1 < curve.keys.size() && curve.keys[index + 1].time <= time) {
            ++index;
        }
        const MotionKey& from = curve.keys[index];
        const MotionKey& to = curve.keys[index + 1];

        const float span = to.time - from.time;
        const float t = (span > 1e-6f) ? (time - from.time) / span : 1.0f;
        values[c] = Interpolate(from, to, std::min(1.0f, std::max(0.0f, t)));
    }
}

bool ParseMotionFile(const std::string& path, Motion& out, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(path), ec)) {
        error = "动作文件不存在：" + path;
        return false;
    }

    nlohmann::json doc;
    try {
        std::ifstream file(path);
        if (!file) {
            error = "无法打开动作文件：" + path;
            return false;
        }
        file >> doc;
    } catch (const nlohmann::json::exception& e) {
        error = std::string("动作文件不是合法 JSON：") + e.what();
        return false;
    } catch (const std::exception& e) {
        error = std::string("读取动作文件失败：") + e.what();
        return false;
    }

    if (!doc.is_object()) {
        error = "动作文件根节点不是对象";
        return false;
    }
    if (!doc.contains("Curves") || !doc.at("Curves").is_array()) {
        error = "动作文件缺少 Curves 字段";
        return false;
    }

    Motion motion;

    // --- Meta ---
    if (doc.contains("Meta") && doc.at("Meta").is_object()) {
        const nlohmann::json& meta = doc.at("Meta");
        if (meta.contains("Duration") && meta.at("Duration").is_number()) {
            motion.duration = meta.at("Duration").get<float>();
        }
        if (meta.contains("Fps") && meta.at("Fps").is_number()) {
            motion.fps = meta.at("Fps").get<float>();
        }
    }

    // --- Curves ---
    for (const auto& item : doc.at("Curves")) {
        if (!item.is_object()) continue;

        MotionCurve curve;
        curve.target = JsonString(item, "Target");
        curve.id = JsonString(item, "Id");
        if (curve.id.empty()) continue;

        const auto curveIt = item.find("Curve");
        if (curveIt == item.end() || !curveIt->is_array()) continue;

        for (const auto& keyItem : *curveIt) {
            if (!keyItem.is_object()) continue;

            MotionKey key;
            if (keyItem.contains("Time") && keyItem.at("Time").is_number()) {
                key.time = keyItem.at("Time").get<float>();
            }
            if (keyItem.contains("Value") && keyItem.at("Value").is_number()) {
                key.value = keyItem.at("Value").get<float>();
            }
            key.inType = ParseCurveType(JsonString(keyItem, "In"));
            key.outType = ParseCurveType(JsonString(keyItem, "Out"));

            // 贝塞尔控制点：Bezier 字段形如 [x1, y1, x2, y2]。
            const auto bezierIt = keyItem.find("Bezier");
            if (bezierIt != keyItem.end() && bezierIt->is_array() &&
                bezierIt->size() >= 4) {
                for (std::size_t i = 0; i < 4; ++i) {
                    const auto& v = (*bezierIt)[i];
                    key.bezier[i] = v.is_number() ? v.get<float>() : (i < 2 ? 0.0f : 1.0f);
                }
            }

            curve.keys.push_back(key);
        }

        if (curve.keys.empty()) continue;
        // 关键帧按时间排序，保证采样逻辑成立。
        std::sort(curve.keys.begin(), curve.keys.end(),
                  [](const MotionKey& a, const MotionKey& b) { return a.time < b.time; });
        motion.curves.push_back(curve);
    }

    if (motion.curves.empty()) {
        error = "动作文件里没有可用的曲线：" + path;
        return false;
    }

    // Meta 里没有 Duration 时，用最后一个关键帧的时间作为时长。
    if (motion.duration <= 0.0f) {
        for (const MotionCurve& curve : motion.curves) {
            if (!curve.keys.empty()) {
                motion.duration = std::max(motion.duration, curve.keys.back().time);
            }
        }
    }

    out = motion;
    return true;
}

}  // namespace ppet::live2d
