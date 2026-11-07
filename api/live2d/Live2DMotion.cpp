// PPet - C++ 移植版
// Live2D 动作（motion3.json）解析与播放实现。
#include <src/tools/core/FileIO.hpp>
#include <api/live2d/Live2DMotion.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

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
/// 控制点为 (0,from) -> (x1,y1) -> (x2,y2) -> (1,to)。
/// 横坐标用于反解参数 t（牛顿迭代），纵坐标给出插值结果。
///
/// @param p  控制点 [x1, y1, x2, y2]（x 归一化、y 绝对）
/// @param t  时间比例 [0,1]
float BezierValue(const float p[4], float t, float fromValue, float toValue) {
    const float mt = 1.0f - t;
    return mt * mt * mt * fromValue + 3.0f * mt * mt * t * p[1] +
           3.0f * mt * t * t * p[3] + t * t * t * toValue;
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
            return BezierValue(from.bezier, param, from.value, to.value);
        }
        case CurveType::Linear:
        default:
            break;
    }
    return from.value + (to.value - from.value) * t;
}

/// 把绝对时间的贝塞尔控制点换算成「相对本段」的归一化坐标。
///
/// 两种动作格式里控制点的时间都是绝对秒数，而插值时用的是
/// 相对本关键帧到下一关键帧的 [0,1] 比例，因此要在这里换算。
///
/// @param key      起始关键帧（其 bezier 里存的是绝对控制点）
/// @param nextTime 下一个关键帧的时间
void NormalizeBezier(MotionKey& key, float nextTime) {
    const float span = nextTime - key.time;
    if (span <= 1e-6f) return;
    // 控制点的时间可能在有效区间之外（例如末帧的贝塞尔会延伸到一个「未来」时间），
    // 这里不做钳制，保持与官方一致。
    key.bezier[0] = (key.bezier[0] - key.time) / span;
    key.bezier[2] = (key.bezier[2] - key.time) / span;
}

/// 解析 Cubism 5 的扁平「Segments」数组。
///
/// 官方规范（Live2D/CubismSpecs · FileFormats/motion3.json.md，原文摘录）：
///   · A point is a sequence of 2 numbers, where the first number is its
///     timing in seconds (t), and the second number its value at t.
///   · A segment identifier is single number:
///       0 = linear, 1 = cubic bézier, 2 = stepped, 3 = inverse-stepped.
///   · Each curve starts with the first point followed by the segment
///     identifier. Therefore, the first segment identifier is the third number.
///   · A segment identifier is followed by *1 point* in case of linear, stepped,
///     and inverse stepped segments, that represents the end of the segment,
///     or *3 points* in case of bézier segments, that represent P1, P2, P3.
///   · 贝塞尔控制点的时间按比例固定：
///       P1.t = 1/3 (P3.t - P0.t)，P2.t = 2/3 (P3.t - P0.t)
///
/// 【重要】官方样例（CubismSdkForNative-5-r.5 的 Haru 模型 haru_g_m08）：
///   "Segments": [ 0, 0, 1, 0.167, 0, 0.333, 0, 0.5, 0, 1, ... ]
///   = 点(0s, 0) + 段标识1 + 三个点 (0.167s,0) (0.333s,0) (0.5s,0)。
///   早期实现按「4 个数（x1,y1,x2,y2）」解析，会把控制点数量数错，
///   导致整条曲线的关键帧时间轴错位（动画「看起来不动」或节奏乱掉）。
///
/// @param segments 扁平数组
/// @param out      解析出的关键帧
/// @return 解析出至少一个关键帧返回 true。
bool ParseSegments(const nlohmann::json& segments, std::vector<MotionKey>& out) {
    out.clear();
    if (!segments.is_array() || segments.size() < 3) return false;

    std::vector<float> numbers;
    numbers.reserve(segments.size());
    for (const auto& item : segments) {
        if (!item.is_number()) return false;
        numbers.push_back(item.get<float>());
    }

    // 第一个点。
    MotionKey first;
    first.time = numbers[0];
    first.value = numbers[1];
    out.push_back(first);
    std::size_t i = 2;

    while (i < numbers.size()) {
        // 段标识。
        const int segId = static_cast<int>(numbers[i]);
        ++i;
        if (segId < 0 || segId > 3) return false;

        if (segId == 1) {
            // 贝塞尔段：段标识后跟 **3 个点**（P1、P2、P3），
            // 其中 P3 就是这一段的终点。
            if (i + 6 > numbers.size()) break;
            const float p1t = numbers[i + 0];
            const float p1v = numbers[i + 1];
            const float p2t = numbers[i + 2];
            const float p2v = numbers[i + 3];
            const float p3t = numbers[i + 4];
            const float p3v = numbers[i + 5];
            i += 6;

            MotionKey& prev = out.back();
            prev.outType = CurveType::Bezier;
            // 控制点：x1/x2 用**实际时间**参与插值（随后按段长归一化），
            // y1/y2 是绝对值。
            prev.bezier[0] = p1t;
            prev.bezier[1] = p1v;
            prev.bezier[2] = p2t;
            prev.bezier[3] = p2v;
            // P3 是本段终点。
            MotionKey key;
            key.time = p3t;
            key.value = p3v;
            key.inType = CurveType::Bezier;
            out.push_back(key);
            continue;
        }

        // 线性 / 阶跃 / 反向阶跃：段标识后跟 **1 个点**（本段终点）。
        if (i + 1 >= numbers.size()) break;
        MotionKey key;
        key.time = numbers[i];
        key.value = numbers[i + 1];
        i += 2;

        MotionKey& prev = out.back();
        prev.outType = (segId >= 2) ? CurveType::Stepped : CurveType::Linear;
        key.inType = prev.outType;
        out.push_back(key);
    }

    if (out.size() < 2) return false;
    // 贝塞尔控制点的时间是绝对秒数，换算成相对本段的归一化坐标。
    for (std::size_t k = 0; k + 1 < out.size(); ++k) {
        if (out[k].outType == CurveType::Bezier) {
            NormalizeBezier(out[k], out[k + 1].time);
        }
    }
    return true;
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
    if (!std::filesystem::is_regular_file(ToFsPath(path), ec)) {
        error = "动作文件不存在：" + path;
        return false;
    }

    nlohmann::json doc;
    try {
        // 走跨平台读取接口：Windows 上窄字符路径打不开含中文的模型目录。
        std::string text;
        if (!ReadFileText(path, text)) {
            error = "无法打开动作文件：" + path;
            return false;
        }
        doc = nlohmann::json::parse(text);
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
        // 淡入/淡出：Cubism 3 规范里是 Meta.FadeInTime / FadeOutTime（秒）。
        // 缺省 0：老模型没有这两个字段时保持「无过渡」的旧行为。
        if (meta.contains("FadeInTime") && meta.at("FadeInTime").is_number()) {
            motion.fadeInTime = std::max(0.0f, meta.at("FadeInTime").get<float>());
        }
        if (meta.contains("FadeOutTime") && meta.at("FadeOutTime").is_number()) {
            motion.fadeOutTime = std::max(0.0f, meta.at("FadeOutTime").get<float>());
        }
        // Meta.Loop：待机动画标记为循环（Cubism 5 起新增的字段）。
        if (meta.contains("Loop") && meta.at("Loop").is_boolean()) {
            motion.loop = meta.at("Loop").get<bool>();
        }
    }

    // --- Curves ---
    for (const auto& item : doc.at("Curves")) {
        if (!item.is_object()) continue;

        MotionCurve curve;
        curve.target = JsonString(item, "Target");
        curve.id = JsonString(item, "Id");
        if (curve.id.empty()) continue;

        // 两种曲线写法：
        //   · Cubism 5：扁平数组 "Segments"
        //   · Cubism 3/4：关键帧数组 "Curve"
        const auto segmentsIt = item.find("Segments");
        if (segmentsIt != item.end() && segmentsIt->is_array()) {
            if (ParseSegments(*segmentsIt, curve.keys)) {
                motion.curves.push_back(std::move(curve));
            }
            continue;
        }

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

            // 贝塞尔控制点：Bezier 字段形如 [x1, y1, x2, y2]，
            // 这里的 x 是**绝对时间**（与 Cubism 3/4 的原始格式一致）。
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
        // 贝塞尔控制点换算成相对本段的归一化坐标。
        for (std::size_t k = 0; k + 1 < curve.keys.size(); ++k) {
            if (curve.keys[k].outType == CurveType::Bezier) {
                NormalizeBezier(curve.keys[k], curve.keys[k + 1].time);
            }
        }
        motion.curves.push_back(std::move(curve));
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
