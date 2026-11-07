// PPet - C++ 移植版
// Live2D 物理（physics3.json）解析与摆锤求解实现。
#include <api/live2d/Live2DPhysics.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace ppet::live2d {
namespace {

constexpr float kPi = 3.14159265358979323846f;
/// 官方 CubismPhysics 的空气阻力常数。
constexpr float kAirResistance = 5.0f;
/// 官方 CubismPhysics 的最大权重（Weight 以 0~100 记）。
constexpr float kMaximumWeight = 100.0f;
/// 官方 CubismPhysics 的移动阈值：小于该值的横向位移归零，避免抖动。
constexpr float kMovementThreshold = 0.001f;
/// 单帧最大时间步长（秒），防止长时间卡顿后物理炸开。
constexpr float kMaxDeltaTime = 5.0f;

/// 解析 PhysicsType。
PhysicsType ParseType(const std::string& text) {
    if (text == "Y") return PhysicsType::Y;
    if (text == "Angle") return PhysicsType::Angle;
    return PhysicsType::X;
}

/// 从 JSON 对象里取字符串字段。
std::string JsonString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return std::string();
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}

/// 从 JSON 对象里取浮点字段，缺失时返回默认值。
float JsonFloat(const nlohmann::json& obj, const char* key, float fallback) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return fallback;
    return it->get<float>();
}

/// 从 JSON 对象里取布尔字段，缺失时返回默认值。
bool JsonBool(const nlohmann::json& obj, const char* key, bool fallback) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

/// 从 JSON 对象里取子对象（缺失时返回空对象）。
///
/// nlohmann 3.12 没有无参 value() 重载，这里统一用 find + 静态空对象兜底。
const nlohmann::json& FindField(const nlohmann::json& obj, const char* key) {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    if (!obj.is_object()) return kEmpty;
    const auto it = obj.find(key);
    if (it == obj.end()) return kEmpty;
    return *it;
}

/// 把「可能是数组、也可能是单个对象」的字段统一成数组。
///
/// Cubism 3+ 规范里 Input / Output / Vertices 都是数组，但部分工具
/// （以及某些转换脚本）会写成单个对象，这里做兼容。
std::vector<nlohmann::json> AsArray(const nlohmann::json& value) {
    std::vector<nlohmann::json> out;
    if (value.is_array()) {
        out = value.get<std::vector<nlohmann::json>>();
    } else if (value.is_object()) {
        out.push_back(value);
    }
    return out;
}

/// 读取「参数引用」对象（Source / Destination）。
///
/// 标准写法是 { "Target": "Parameter", "Id": "ParamAngleX" }；
/// 部分旧文件直接写 { "Id": "..." }，这里一并兼容。
std::string JsonParameterId(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return std::string();
    const auto it = obj.find(key);
    if (it == obj.end()) return std::string();
    const nlohmann::json& ref = *it;
    if (ref.is_string()) return ref.get<std::string>();
    if (!ref.is_object()) return std::string();
    const std::string id = JsonString(ref, "Id");
    if (!id.empty()) return id;
    return std::string();
}

/// 解析归一化区间。
void ParseNormalization(const nlohmann::json& node, PhysicsSetting& setting) {
    if (!node.is_object()) return;
    const auto pos = node.find("Position");
    if (pos != node.end() && pos->is_object()) {
        setting.positionMin = JsonFloat(*pos, "Minimum", setting.positionMin);
        setting.positionDefault = JsonFloat(*pos, "Default", setting.positionDefault);
        setting.positionMax = JsonFloat(*pos, "Maximum", setting.positionMax);
    }
    const auto ang = node.find("Angle");
    if (ang != node.end() && ang->is_object()) {
        setting.angleMin = JsonFloat(*ang, "Minimum", setting.angleMin);
        setting.angleDefault = JsonFloat(*ang, "Default", setting.angleDefault);
        setting.angleMax = JsonFloat(*ang, "Maximum", setting.angleMax);
    }
}

/// 解析一个物理设置。
bool ParseSetting(const nlohmann::json& node, PhysicsSetting& setting) {
    if (!node.is_object()) return false;
    setting.id = JsonString(node, "Id");

    for (const nlohmann::json& item : AsArray(FindField(node, "Input"))) {
        PhysicsInput input;
        input.parameter = JsonParameterId(item, "Source");
        if (input.parameter.empty()) continue;
        input.weight = JsonFloat(item, "Weight", 100.0f);
        input.type = ParseType(JsonString(item, "Type"));
        input.reflect = JsonBool(item, "Reflect", false);
        setting.inputs.push_back(input);
    }

    for (const nlohmann::json& item : AsArray(FindField(node, "Output"))) {
        PhysicsOutput output;
        output.parameter = JsonParameterId(item, "Destination");
        if (output.parameter.empty()) continue;
        output.vertexIndex = static_cast<int>(JsonFloat(item, "VertexIndex", 0.0f));
        output.scale = JsonFloat(item, "Scale", 1.0f);
        output.weight = JsonFloat(item, "Weight", 100.0f);
        output.type = ParseType(JsonString(item, "Type"));
        output.reflect = JsonBool(item, "Reflect", false);
        setting.outputs.push_back(output);
    }

    for (const nlohmann::json& item : AsArray(FindField(node, "Vertices"))) {
        PhysicsVertex vertex;
        const auto pos = item.find("Position");
        if (pos != item.end() && pos->is_object()) {
            vertex.positionX = JsonFloat(*pos, "X", 0.0f);
            vertex.positionY = JsonFloat(*pos, "Y", 0.0f);
        }
        vertex.mobility = JsonFloat(item, "Mobility", 1.0f);
        vertex.delay = JsonFloat(item, "Delay", 1.0f);
        vertex.acceleration = JsonFloat(item, "Acceleration", 1.0f);
        vertex.radius = JsonFloat(item, "Radius", 0.0f);
        setting.vertices.push_back(vertex);
    }

    const auto norm = node.find("Normalization");
    if (norm != node.end()) ParseNormalization(*norm, setting);

    // 至少要有输出才有意义。
    return !setting.outputs.empty();
}

/// 计算 from 到 to 的有符号夹角（弧度），范围 (-π, π]。
float DirectionToRadian(float fromX, float fromY, float toX, float toY) {
    float r = std::atan2(toY, toX) - std::atan2(fromY, fromX);
    while (r < -kPi) r += 2.0f * kPi;
    while (r > kPi) r -= 2.0f * kPi;
    return r;
}

/// 把参数值从其自身区间归一化到目标区间（中点对应目标中点）。
///
/// 与官方 CubismPhysicsMath.Normalize 一致：正向用上侧区间、负向用下侧区间，
/// 两侧各自线性映射；结果再按「未反向则取负」的官方约定处理。
float Normalize(float value, float min, float max, float normMin, float normMax,
                float normDefault, bool inverted) {
    const float lo = std::min(min, max);
    const float hi = std::max(min, max);
    value = std::clamp(value, lo, hi);

    const float normLo = std::min(normMin, normMax);
    const float normHi = std::max(normMin, normMax);
    const float middle = lo + (hi - lo) * 0.5f;
    const float offset = value - middle;

    float result;
    if (offset > 0.0f) {
        const float p = hi - middle;
        result = (p != 0.0f) ? offset * ((normHi - normDefault) / p) + normDefault : 0.0f;
    } else if (offset < 0.0f) {
        const float p = lo - middle;
        result = (p != 0.0f) ? offset * ((normLo - normDefault) / p) + normDefault : 0.0f;
    } else {
        result = normDefault;
    }
    return inverted ? result : -result;
}

}  // namespace

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

bool ParsePhysicsJson(const std::string& text, PhysicsFile& out, std::string& error) {
    out = PhysicsFile();
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        error = std::string("物理文件不是合法 JSON：") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        error = "物理文件根节点不是对象";
        return false;
    }

    // Meta：帧率与外力。
    const auto meta = doc.find("Meta");
    if (meta != doc.end() && meta->is_object()) {
        out.fps = JsonFloat(*meta, "Fps", 0.0f);
        const auto forces = meta->find("EffectiveForces");
        if (forces != meta->end() && forces->is_object()) {
            const auto gravity = forces->find("Gravity");
            if (gravity != forces->end() && gravity->is_object()) {
                out.gravityX = JsonFloat(*gravity, "X", out.gravityX);
                out.gravityY = JsonFloat(*gravity, "Y", out.gravityY);
            }
            const auto wind = forces->find("Wind");
            if (wind != forces->end() && wind->is_object()) {
                out.windX = JsonFloat(*wind, "X", out.windX);
                out.windY = JsonFloat(*wind, "Y", out.windY);
            }
        }
    }

    const auto settings = doc.find("PhysicsSettings");
    if (settings == doc.end() || !settings->is_array()) {
        error = "物理文件缺少 PhysicsSettings";
        return false;
    }

    for (const auto& node : *settings) {
        PhysicsSetting setting;
        if (!ParseSetting(node, setting)) continue;
        out.settings.push_back(std::move(setting));
    }

    if (out.settings.empty()) {
        error = "物理文件里没有有效的物理设置";
        return false;
    }
    return true;
}

bool LoadPhysicsFile(const std::string& path, PhysicsFile& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "物理文件无法打开：" + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ParsePhysicsJson(ss.str(), out, error);
}

// ---------------------------------------------------------------------------
// 求解器
// ---------------------------------------------------------------------------

bool Live2DPhysics::Load(const PhysicsFile& file) {
    Reset();
    settings_ = file.settings;
    gravityX_ = file.gravityX;
    gravityY_ = file.gravityY;
    windX_ = file.windX;
    windY_ = file.windY;
    fps_ = file.fps > 0.0f ? file.fps : 0.0f;

    strands_.resize(settings_.size());
    for (std::size_t i = 0; i < settings_.size(); ++i) {
        Strand& strand = strands_[i];
        const PhysicsSetting& setting = settings_[i];

        // 粒子链：第 0 个是根部，第 i 个通过 radius 挂在第 i-1 个下面。
        strand.particles.resize(setting.vertices.size());
        for (std::size_t v = 0; v < setting.vertices.size(); ++v) {
            const PhysicsVertex& src = setting.vertices[v];
            Strand::Particle& p = strand.particles[v];
            p.mobility = src.mobility;
            p.delay = src.delay;
            p.acceleration = src.acceleration;
            p.radius = src.radius;
            p.lastGravityY = 1.0f;
        }
        strand.currentOutputs.assign(setting.outputs.size(), 0.0f);
        strand.previousOutputs.assign(setting.outputs.size(), 0.0f);
    }
    return !settings_.empty();
}

void Live2DPhysics::Reset() {
    settings_.clear();
    strands_.clear();
    accumulator_ = 0.0f;
    inputCache_.clear();
    outputCache_.clear();
    paramRanges_.clear();
    hasInputCache_ = false;
}

void Live2DPhysics::SetParameterRange(const std::string& parameter, float min, float max) {
    paramRanges_[parameter] = {min, max};
}

void Live2DPhysics::ComputeInputs(const PhysicsSetting& setting,
                                  const std::map<std::string, float>& params,
                                  float& outTranslationX, float& outTranslationY,
                                  float& outAngle) {
    float translationX = 0.0f;
    float translationY = 0.0f;
    float angle = 0.0f;

    for (const PhysicsInput& input : setting.inputs) {
        const auto it = params.find(input.parameter);
        if (it == params.end()) continue;

        // 参数自身的 min/max 决定归一化的基准。
        float minValue = -1.0f;
        float maxValue = 1.0f;
        const auto rangeIt = paramRanges_.find(input.parameter);
        if (rangeIt != paramRanges_.end()) {
            minValue = rangeIt->second.first;
            maxValue = rangeIt->second.second;
        }

        const float weight = input.weight / kMaximumWeight;
        const float normalized = Normalize(it->second, minValue, maxValue, setting.positionMin,
                                           setting.positionMax, setting.positionDefault,
                                           input.reflect);
        switch (input.type) {
            case PhysicsType::X:
                translationX += normalized * weight;
                break;
            case PhysicsType::Y:
                translationY += normalized * weight;
                break;
            case PhysicsType::Angle:
                angle += Normalize(it->second, minValue, maxValue, setting.angleMin,
                                   setting.angleMax, setting.angleDefault, input.reflect) *
                         weight;
                break;
        }
    }

    // 官方行为：输入的平移会按角度旋转（y 用的是已旋转的 x）。
    const float radian = (-angle) * kPi / 180.0f;
    const float rotatedX = translationX * std::cos(radian) - translationY * std::sin(radian);
    const float rotatedY = translationX * std::sin(radian) + translationY * std::cos(radian);
    outTranslationX = rotatedX;
    outTranslationY = rotatedY;
    outAngle = angle;
}

void Live2DPhysics::UpdateStrand(Strand& strand, const PhysicsSetting& setting,
                                 float translationX, float translationY, float angle, float dt) {
    if (strand.particles.empty()) return;

    // 根部跟随输入平移。
    strand.particles[0].x = translationX;
    strand.particles[0].y = translationY;

    // 重力方向：绕根部旋转（Angle 输入给出旋转角）。
    const float radian = angle * kPi / 180.0f;
    float gravityX = std::sin(radian);
    float gravityY = std::cos(radian);
    const float gLen = std::sqrt(gravityX * gravityX + gravityY * gravityY);
    if (gLen > 1e-6f) {
        gravityX /= gLen;
        gravityY /= gLen;
    } else {
        gravityX = 0.0f;
        gravityY = 1.0f;
    }

    for (std::size_t i = 1; i < strand.particles.size(); ++i) {
        Strand::Particle& p = strand.particles[i];
        const Strand::Particle& parent = strand.particles[i - 1];

        // 力 = 重力 * 收敛速度 + 风。
        const float forceX = gravityX * p.acceleration + windX_;
        const float forceY = gravityY * p.acceleration + windY_;

        p.lastX = p.x;
        p.lastY = p.y;

        // 官方把时间步长按 30fps 基准缩放。
        const float delay = p.delay * dt * 30.0f;

        // 当前连线方向，并按「重力方向的变化 / 空气阻力」旋转（模拟空气阻力）。
        float dirX = p.x - parent.x;
        float dirY = p.y - parent.y;
        const float resist =
            DirectionToRadian(p.lastGravityX, p.lastGravityY, gravityX, gravityY) / kAirResistance;
        const float cosR = std::cos(resist);
        const float sinR = std::sin(resist);
        const float rotX = cosR * dirX - dirY * sinR;
        const float rotY = sinR * dirX + dirY * cosR;
        dirX = rotX;
        dirY = rotY;

        // 沿父粒子方向重新摆放（保持距离约束）。
        p.x = parent.x + dirX;
        p.y = parent.y + dirY;

        // Verlet 积分：速度项 + 力项。
        p.x += p.velocityX * delay + forceX * delay * delay;
        p.y += p.velocityY * delay + forceY * delay * delay;

        // 距离约束：把粒子拉回与父粒子距离为 radius 的位置。
        float newDirX = p.x - parent.x;
        float newDirY = p.y - parent.y;
        const float newLen = std::sqrt(newDirX * newDirX + newDirY * newDirY);
        if (newLen > 1e-6f) {
            newDirX /= newLen;
            newDirY /= newLen;
        } else {
            newDirX = 0.0f;
            newDirY = 1.0f;
        }
        p.x = parent.x + newDirX * p.radius;
        p.y = parent.y + newDirY * p.radius;

        // 横向抖动阈值。
        const float threshold = kMovementThreshold * setting.positionMax;
        if (std::fabs(p.x) < threshold) p.x = 0.0f;

        // 更新速度。
        if (delay != 0.0f) {
            p.velocityX = (p.x - p.lastX) / delay * p.mobility;
            p.velocityY = (p.y - p.lastY) / delay * p.mobility;
        }

        p.lastGravityX = gravityX;
        p.lastGravityY = gravityY;
    }
}

void Live2DPhysics::WriteOutputs(Strand& strand, const PhysicsSetting& setting) {
    for (std::size_t k = 0; k < setting.outputs.size(); ++k) {
        const PhysicsOutput& output = setting.outputs[k];
        const int index = output.vertexIndex;
        if (index < 1 || static_cast<std::size_t>(index) >= strand.particles.size()) continue;

        const Strand::Particle& p = strand.particles[static_cast<std::size_t>(index)];
        const Strand::Particle& parent = strand.particles[static_cast<std::size_t>(index - 1)];
        const float transX = p.x - parent.x;
        const float transY = p.y - parent.y;

        float value = 0.0f;
        switch (output.type) {
            case PhysicsType::X:
                value = transX;
                break;
            case PhysicsType::Y:
                value = transY;
                break;
            case PhysicsType::Angle: {
                // 角度：与「上一段连线」比较（官方在 particleIndex<2 时与重力方向比较）。
                float refX = 0.0f;
                float refY = 1.0f;
                if (index >= 2) {
                    const Strand::Particle& grand =
                        strand.particles[static_cast<std::size_t>(index - 2)];
                    refX = parent.x - grand.x;
                    refY = parent.y - grand.y;
                }
                value = DirectionToRadian(refX, refY, transX, transY);
                break;
            }
        }
        if (output.reflect) value = -value;
        strand.currentOutputs[k] = value;
    }
}

void Live2DPhysics::Stabilize(std::map<std::string, float>& params) {
    accumulator_ = 0.0f;
    hasInputCache_ = false;
    inputCache_.clear();
    outputCache_.clear();

    for (std::size_t i = 0; i < settings_.size(); ++i) {
        const PhysicsSetting& setting = settings_[i];
        Strand& strand = strands_[i];

        // 计算输入（用当前参数值）。
        float translationX = 0.0f;
        float translationY = 0.0f;
        float angle = 0.0f;
        ComputeInputs(setting, params, translationX, translationY, angle);

        // 摆锤静止下垂：每个粒子沿重力方向挂在父粒子下方。
        const float radian = angle * kPi / 180.0f;
        float gravityX = std::sin(radian);
        float gravityY = std::cos(radian);
        const float gLen = std::sqrt(gravityX * gravityX + gravityY * gravityY);
        if (gLen > 1e-6f) {
            gravityX /= gLen;
            gravityY /= gLen;
        } else {
            gravityX = 0.0f;
            gravityY = 1.0f;
        }

        strand.particles[0].x = translationX;
        strand.particles[0].y = translationY;
        strand.particles[0].lastX = translationX;
        strand.particles[0].lastY = translationY;
        for (std::size_t v = 1; v < strand.particles.size(); ++v) {
            Strand::Particle& p = strand.particles[v];
            p.x = strand.particles[v - 1].x + gravityX * p.radius;
            p.y = strand.particles[v - 1].y + gravityY * p.radius;
            p.lastX = p.x;
            p.lastY = p.y;
            p.velocityX = 0.0f;
            p.velocityY = 0.0f;
            p.lastGravityX = gravityX;
            p.lastGravityY = gravityY;
        }

        // 计算输出并写入参数。
        WriteOutputs(strand, setting);
        for (std::size_t k = 0; k < setting.outputs.size(); ++k) {
            const PhysicsOutput& output = setting.outputs[k];
            const float scaled = strand.currentOutputs[k] * output.scale;
            auto it = params.find(output.parameter);
            if (it == params.end()) continue;
            const float weight = output.weight / kMaximumWeight;
            it->second = it->second * (1.0f - weight) + scaled * weight;
            outputCache_[output.parameter] = it->second;
        }
        std::fill(strand.previousOutputs.begin(), strand.previousOutputs.end(), 0.0f);
        std::fill(strand.currentOutputs.begin(), strand.currentOutputs.end(), 0.0f);
    }
}

void Live2DPhysics::Evaluate(float dt, std::map<std::string, float>& params) {
    if (settings_.empty()) return;
    dt = std::clamp(dt, 0.0f, kMaxDeltaTime);

    // 官方在声明了 Fps 时按固定步长推进，并在帧间对输入做插值。
    const float step = fps_ > 0.0f ? (1.0f / fps_) : dt;
    accumulator_ += dt;
    if (accumulator_ > kMaxDeltaTime) accumulator_ = 0.0f;

    // 输入缓存（用于帧间插值）。
    if (!hasInputCache_) {
        inputCache_.clear();
        for (const PhysicsSetting& setting : settings_) {
            for (const PhysicsInput& input : setting.inputs) {
                const auto it = params.find(input.parameter);
                inputCache_[input.parameter] = (it == params.end()) ? 0.0f : it->second;
            }
        }
        hasInputCache_ = true;
    }

    // 推进固定步长。
    while (accumulator_ >= step) {
        const float weight = step / accumulator_;
        for (const PhysicsSetting& setting : settings_) {
            for (const PhysicsInput& input : setting.inputs) {
                const auto it = params.find(input.parameter);
                const float current = (it == params.end()) ? 0.0f : it->second;
                auto cacheIt = inputCache_.find(input.parameter);
                const float cached = (cacheIt == inputCache_.end()) ? current : cacheIt->second;
                const float interpolated = cached * (1.0f - weight) + current * weight;
                inputCache_[input.parameter] = interpolated;
            }
        }

        for (std::size_t i = 0; i < settings_.size(); ++i) {
            const PhysicsSetting& setting = settings_[i];
            Strand& strand = strands_[i];
            strand.previousOutputs = strand.currentOutputs;

            float translationX = 0.0f;
            float translationY = 0.0f;
            float angle = 0.0f;
            ComputeInputs(setting, inputCache_, translationX, translationY, angle);
            UpdateStrand(strand, setting, translationX, translationY, angle, step);
            WriteOutputs(strand, setting);
        }
        accumulator_ -= step;
    }

    // 输出插值：官方在两个固定步之间对输出做线性插值。
    const float alpha = step > 0.0f ? (accumulator_ / step) : 1.0f;
    for (std::size_t i = 0; i < settings_.size(); ++i) {
        const PhysicsSetting& setting = settings_[i];
        Strand& strand = strands_[i];
        for (std::size_t k = 0; k < setting.outputs.size(); ++k) {
            const PhysicsOutput& output = setting.outputs[k];
            const float raw = strand.previousOutputs[k] * (1.0f - alpha) +
                              strand.currentOutputs[k] * alpha;
            const float scaled = raw * output.scale;
            auto it = params.find(output.parameter);
            if (it == params.end()) continue;

            // 与「参数当前值」按 Weight 混合：Weight=100 时完全由物理决定。
            const float weight = output.weight / kMaximumWeight;
            it->second = it->second * (1.0f - weight) + scaled * weight;
            outputCache_[output.parameter] = it->second;
        }
    }
}

}  // namespace ppet::live2d
