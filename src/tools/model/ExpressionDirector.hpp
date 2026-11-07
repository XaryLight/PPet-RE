// PPet - C++ 移植版
// PMX / VRM 表情管理：把「模型里有哪些表情通道」与「现在该显示什么表情」解耦。
//
// 【参考来源】
//   Tanorika/VRM-VPet 的 ExpressionManager.ts / ExpressionTimeline.ts / MotionManager.ts：
//     · 预设（自然/开心/放松/生气/难过/惊讶）+ 别名解析（happy 也能命中 joy）
//     · 切换预设走 smoothstep 渐变，而不是瞬间跳变
//     · 自动眨眼是独立叠层，用 max 叠加，不写入用户当前选择的表情
//     · 动作播放时的表情是「临时叠层」，结束后按 0.2s 淡出回落到用户表情
//     · 表情关键帧时间轴：录制 / 覆盖 / 删除 / 采样 / 导出
//
// 【为什么做成纯头文件】
//   本项目其它纯逻辑（ClampRectInside / AnalyzeFramePixels / MotionDirector）都是
//   零 GL、零 Qt、零文件 IO 的头文件，可以脱离渲染上下文做单元测试。表情调度
//   本质上是「一堆 float 的时间函数」，没有理由依赖模型后端。
//   后端只做两件事：把通道名喂给 Bind()，把 Weights() 写进 morph / expression 权重。
//
// 【通道与别名】
//   VRM 1.0 的 preset 名是 aa/ih/ou/ee/oh/blink/happy/...；VRM 0.x 与 PMX 用
//   日文/中文 morph 名（あ / 笑い / 眨眼）。Resolve() 先按小写精确匹配，再查别名表，
//   让「预设」这种逻辑名可以落到任意模型的真实通道上。解析不到就返回 -1，
//   预设自动标记为不可用，绝不写越界下标。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace ppet {
namespace expression_detail {

inline float ClampUnit(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float SmoothStep(float t) {
    t = ClampUnit(t);
    return t * t * (3.0f - 2.0f * t);
}

inline std::string AsciiLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

struct AliasEntry {
    const char* logical;
    const char* candidates;  // 以 '|' 分隔，全部按小写比较
};

/// 逻辑通道名 -> 模型里可能出现的真实名字。
/// 覆盖 VRM 1.0 / VRM 0.x / PMX（日文与中文 morph 名）三种命名习惯。
inline constexpr AliasEntry kAliases[] = {
    {"happy", "happy|joy|fun|smile|笑|笑い|开心|喜悦|喜悦"},
    {"sad", "sad|sorrow|cry|悲|泣|难过|悲伤"},
    {"relaxed", "relaxed|fun|calm|安|放松|安心"},
    {"angry", "angry|anger|mad|怒|生气|愤怒"},
    {"surprised", "surprised|surprise|shock|惊|惊讶|びっくり"},
    {"blink", "blink|blinkboth|眨眼|瞬き|まばたき|闭眼"},
    {"blinkLeft", "blinkleft|blink_l|blinkl|winkleft|眨左|左眨眼|ウィンク左"},
    {"blinkRight", "blinkright|blink_r|blinkr|winkright|眨右|右眨眼|ウィンク右"},
    {"aa", "aa|a|ah|あ|ア|口型a|moutha|mouth_a|vowel_a"},
    {"ih", "ih|i|ii|い|イ|口型i|mouthi|mouth_i|vowel_i"},
    {"ou", "ou|u|uu|う|ウ|口型u|mouthu|mouth_u|vowel_u"},
    {"ee", "ee|e|eh|え|エ|口型e|mouthe|mouth_e|vowel_e"},
    {"oh", "oh|o|oo|お|オ|口型o|moutho|mouth_o|vowel_o"},
};

inline const char* const* AliasCandidates(const std::string& logical) {
    for (const AliasEntry& e : kAliases) {
        if (logical == e.logical) {
            static thread_local std::vector<const char*> cache;
            static thread_local std::string cachedKey;
            if (cachedKey != logical) {
                cachedKey = logical;
                cache.clear();
                std::string s(e.candidates);
                std::size_t start = 0;
                while (start <= s.size()) {
                    const std::size_t pos = s.find('|', start);
                    const std::string part = s.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
                    // 每个候选串常驻静态存储，这里用 string 池保存，避免返回悬垂指针。
                    static thread_local std::vector<std::string> pool;
                    pool.push_back(part);
                    cache.push_back(pool.back().c_str());
                    if (pos == std::string::npos) break;
                    start = pos + 1;
                }
            }
            return cache.data();
        }
    }
    return nullptr;
}

/// 把一个 '|' 分隔的候选串拆成向量（供 Resolve 做别名匹配）。
inline std::vector<std::string> SplitAliases(const char* candidates) {
    std::vector<std::string> out;
    std::string s(candidates);
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t pos = s.find('|', start);
        out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}

}  // namespace expression_detail

/// 一个表情预设：逻辑通道名 -> 目标权重。
struct ExpressionPreset {
    std::string id;
    std::string name;
    std::vector<std::pair<std::string, float>> weights;
    float transitionDuration = 0.3f;
    bool builtin = true;
};

/// 内置预设（与 VRM-VPet 的 DEFAULT_PRESETS 对齐，补上中文别名）。
inline std::vector<ExpressionPreset> DefaultExpressionPresets() {
    return {
        {"neutral",   "自然", {}, 0.3f, true},
        {"happy",     "开心", {{"happy", 0.85f}}, 0.3f, true},
        {"relaxed",   "放松", {{"relaxed", 0.80f}}, 0.35f, true},
        {"angry",     "生气", {{"angry", 0.75f}}, 0.25f, true},
        {"sad",       "难过", {{"sad", 0.80f}}, 0.35f, true},
        {"surprised", "惊讶", {{"surprised", 0.85f}}, 0.2f, true},
    };
}

/// 时间轴插值方式。
enum class ClipEasing { Linear, EaseInOut };

/// 一帧表情关键帧。
struct ExpressionFrame {
    float time = 0.0f;
    std::vector<std::pair<std::string, float>> weights;
};

/// 可持久化的表情片段（素材库里的一条记录）。
struct ExpressionClip {
    std::string id;
    std::string name;
    float duration = 2.0f;
    bool loop = false;
    ClipEasing easing = ClipEasing::Linear;
    std::vector<ExpressionFrame> frames;
};

/// 表情时间轴：录制 / 覆盖 / 删除 / 采样 / 导出。
/// 与 VRM-VPet 的 ExpressionTimeline 一致：编辑只重建表情轨道，
/// 导入的骨骼轨道原样保留（这里是 ExpressionClip::frames 之外的字段用 extra 保存）。
class ExpressionTimeline {
public:
    void Load(const ExpressionClip& clip) {
        frames_ = clip.frames;
        SortFrames();
    }

    void Clear() { frames_.clear(); }

    /// 在 time 处记录一帧；同一时刻（误差 0.5ms）覆盖而不是新增。
    /// @return 是否成功（时间必须落在 [0, duration]，且至少有一个权重）。
    bool Record(float time, float duration, const std::vector<std::pair<std::string, float>>& weights) {
        if (!(time >= 0.0f) || time > duration || weights.empty() || !(duration > 0.0f)) return false;
        const float rounded = std::round(time * 1000.0f) / 1000.0f;
        ExpressionFrame frame;
        frame.time = rounded;
        frame.weights = weights;
        for (auto& kv : frame.weights) kv.second = expression_detail::ClampUnit(kv.second);
        for (ExpressionFrame& existing : frames_) {
            if (std::fabs(existing.time - rounded) < 0.0005f) {
                existing = std::move(frame);
                SortFrames();
                return true;
            }
        }
        frames_.push_back(std::move(frame));
        SortFrames();
        return true;
    }

    void Remove(float time) {
        frames_.erase(std::remove_if(frames_.begin(), frames_.end(),
                                     [time](const ExpressionFrame& f) {
                                         return std::fabs(f.time - time) < 0.0005f;
                                     }),
                      frames_.end());
    }

    const std::vector<ExpressionFrame>& Frames() const { return frames_; }
    std::size_t FrameCount() const { return frames_.size(); }

    /// 采样任意时刻的权重；无关键帧返回空。
    std::vector<std::pair<std::string, float>> Sample(float time, ClipEasing easing) const {
        if (frames_.empty()) return {};
        std::size_t index = 0;
        std::size_t next = 0;
        float alpha = 0.0f;
        if (time <= frames_.front().time) {
            index = next = 0;
        } else if (time >= frames_.back().time) {
            index = next = frames_.size() - 1;
        } else {
            for (std::size_t i = 1; i < frames_.size(); ++i) {
                if (frames_[i].time > time) {
                    index = i - 1;
                    next = i;
                    const float span = frames_[i].time - frames_[index].time;
                    alpha = span > 0.0f ? (time - frames_[index].time) / span : 0.0f;
                    break;
                }
            }
        }
        if (easing == ClipEasing::EaseInOut) alpha = expression_detail::SmoothStep(alpha);

        // 所有出现过的通道取并集，缺失的一侧按 0 处理。
        std::vector<std::string> names;
        for (const ExpressionFrame& f : frames_) {
            for (const auto& kv : f.weights) {
                if (std::find(names.begin(), names.end(), kv.first) == names.end()) {
                    names.push_back(kv.first);
                }
            }
        }
        auto valueOf = [](const ExpressionFrame& f, const std::string& name) {
            for (const auto& kv : f.weights) {
                if (kv.first == name) return kv.second;
            }
            return 0.0f;
        };
        std::vector<std::pair<std::string, float>> out;
        out.reserve(names.size());
        for (const std::string& name : names) {
            const float a = valueOf(frames_[index], name);
            const float b = valueOf(frames_[next], name);
            out.emplace_back(name, a + (b - a) * alpha);
        }
        return out;
    }

    float LastTime() const { return frames_.empty() ? 0.0f : frames_.back().time; }

    ExpressionClip ToClip(const std::string& id, const std::string& name, float duration,
                          bool loop, ClipEasing easing) const {
        ExpressionClip clip;
        clip.id = id;
        clip.name = name;
        clip.duration = duration > 0.0f ? duration : std::max(1.0f, LastTime());
        clip.loop = loop;
        clip.easing = easing;
        clip.frames = frames_;
        return clip;
    }

private:
    void SortFrames() {
        std::stable_sort(frames_.begin(), frames_.end(),
                         [](const ExpressionFrame& a, const ExpressionFrame& b) {
                             return a.time < b.time;
                         });
    }
    std::vector<ExpressionFrame> frames_;
};

/// 表情调度器。
class ExpressionDirector {
public:
    explicit ExpressionDirector(std::uint32_t seed = 20240101u) : rng_(seed) {}

    /// 绑定模型真实通道。binaryFlags 与 channels 等长，非零表示该通道是二元表情
    /// （VRM 1.0 的 isBinary，例如 blink）：权重必须量化成 0 / 1。
    void Bind(const std::vector<std::string>& channels,
              const std::vector<char>* binaryFlags = nullptr) {
        channels_ = channels;
        lowered_.clear();
        lowered_.reserve(channels_.size());
        for (const std::string& c : channels_) lowered_.push_back(expression_detail::AsciiLower(c));
        binary_.assign(channels_.size(), 0);
        if (binaryFlags) {
            for (std::size_t i = 0; i < channels_.size() && i < binaryFlags->size(); ++i) {
                binary_[i] = (*binaryFlags)[static_cast<std::size_t>(i)] != 0 ? 1 : 0;
            }
        }
        presets_ = DefaultExpressionPresets();
        base_.assign(channels_.size(), 0.0f);
        final_.assign(channels_.size(), 0.0f);
        motion_.assign(channels_.size(), 0.0f);
        transitionFrom_.assign(channels_.size(), 0.0f);
        transitionTo_.assign(channels_.size(), 0.0f);
        activePreset_ = "neutral";
        transition_.Reset();
        motionInfluence_ = 0.0f;
        releaseInfluence_ = -1.0f;
        blinkTime_ = -1.0f;
        blinkWait_ = RandomBlinkWait();
    }

    const std::vector<std::string>& Channels() const { return channels_; }
    std::size_t ChannelCount() const { return channels_.size(); }

    /// 逻辑名 -> 真实通道下标；找不到返回 -1。
    int Resolve(const std::string& name) const {
        if (name.empty()) return -1;
        const std::string key = expression_detail::AsciiLower(name);
        for (std::size_t i = 0; i < lowered_.size(); ++i) {
            if (lowered_[i] == key) return static_cast<int>(i);
        }
        // 别名表：若 key 是某个逻辑名（happy/sad…）本身，或其候选之一
        // （joy / 笑い / winkleft …），就把该条目的全部候选都纳入比较。
        std::vector<std::string> candidates;
        candidates.push_back(key);
        for (const expression_detail::AliasEntry& e : expression_detail::kAliases) {
            std::vector<std::string> parts = expression_detail::SplitAliases(e.candidates);
            bool hit = (key == e.logical);
            if (!hit) {
                for (const std::string& p : parts) {
                    if (p == key) { hit = true; break; }
                }
            }
            if (hit) candidates.insert(candidates.end(), parts.begin(), parts.end());
        }
        for (const std::string& candidate : candidates) {
            for (std::size_t i = 0; i < lowered_.size(); ++i) {
                if (lowered_[i] == candidate) return static_cast<int>(i);
            }
        }
        return -1;
    }

    /// 预设是否可用（所有逻辑通道都能解析出来）。
    bool IsPresetAvailable(const ExpressionPreset& preset) const {
        if (channels_.empty()) return false;
        if (preset.weights.empty()) return true;  // neutral 总是可用
        for (const auto& kv : preset.weights) {
            if (Resolve(kv.first) < 0) return false;
        }
        return true;
    }

    std::vector<ExpressionPreset> Presets() const { return presets_; }

    void AddCustomPresets(const std::vector<ExpressionPreset>& custom) {
        for (ExpressionPreset p : custom) {
            if (p.id.empty()) continue;
            // 自定义预设可覆盖同名内置预设。
            auto it = std::find_if(presets_.begin(), presets_.end(),
                                   [&p](const ExpressionPreset& q) { return q.id == p.id; });
            p.builtin = false;
            if (it != presets_.end()) *it = std::move(p);
            else presets_.push_back(std::move(p));
        }
    }

    /// 切换预设。duration < 0 时沿用预设自带的过渡时长；0 表示立即。
    bool ApplyPreset(const std::string& id, float duration = -1.0f) {
        const ExpressionPreset* preset = nullptr;
        for (const ExpressionPreset& p : presets_) {
            if (p.id == id) { preset = &p; break; }
        }
        if (!preset || !IsPresetAvailable(*preset)) return false;
        const float seconds = duration >= 0.0f ? duration : preset->transitionDuration;
        if (!(seconds >= 0.0f)) return false;

        transitionFrom_ = base_;
        transitionTo_.assign(channels_.size(), 0.0f);
        for (const auto& kv : preset->weights) {
            const int index = Resolve(kv.first);
            if (index >= 0) transitionTo_[static_cast<std::size_t>(index)] = expression_detail::ClampUnit(kv.second);
        }
        activePreset_ = id;
        transition_.elapsed = 0.0f;
        transition_.duration = seconds;
        transition_.active = seconds > 0.0f;
        if (!transition_.active) base_ = transitionTo_;
        Recompute(0.0f);
        return true;
    }

    const std::string& ActivePreset() const { return activePreset_; }

    /// 直接设置单个通道权重（取消预设与过渡）。
    bool SetWeight(const std::string& name, float weight) {
        const int index = Resolve(name);
        if (index < 0) return false;
        transition_.Reset();
        activePreset_.clear();
        base_[static_cast<std::size_t>(index)] = expression_detail::ClampUnit(weight);
        Recompute(0.0f);
        return true;
    }

    /// 用一组权重覆盖全部通道（编辑器预览 / 时间轴播放）。
    void PreviewWeights(const std::vector<std::pair<std::string, float>>& weights) {
        transition_.Reset();
        activePreset_.clear();
        std::fill(base_.begin(), base_.end(), 0.0f);
        for (const auto& kv : weights) {
            const int index = Resolve(kv.first);
            if (index >= 0) base_[static_cast<std::size_t>(index)] = expression_detail::ClampUnit(kv.second);
        }
        Recompute(0.0f);
    }

    void ResetAll() { ApplyPreset("neutral"); }

    /// 导出当前基础权重（编辑器录制关键帧用）。
    std::vector<std::pair<std::string, float>> CaptureWeights() const {
        std::vector<std::pair<std::string, float>> out;
        out.reserve(channels_.size());
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            if (base_[i] > 0.0f) out.emplace_back(channels_[i], base_[i]);
        }
        return out;
    }

    float BaseWeight(const std::string& name) const {
        const int index = Resolve(name);
        return index < 0 ? 0.0f : base_[static_cast<std::size_t>(index)];
    }

    /// 最终权重（含过渡、眨眼、动作叠层、二元量化），写进模型时用它。
    const std::vector<float>& Weights() const { return final_; }

    void SetAutoBlink(bool enabled) {
        autoBlink_ = enabled;
        blinkTime_ = -1.0f;
        blinkWait_ = RandomBlinkWait();
        Recompute(0.0f);
    }
    bool AutoBlink() const { return autoBlink_; }

    /// 动作表情临时叠层（对齐 VRM-VPet 的 setMotionWeights）。
    void SetMotionWeights(const std::vector<std::pair<std::string, float>>& weights, float influence) {
        std::fill(motion_.begin(), motion_.end(), 0.0f);
        for (const auto& kv : weights) {
            const int index = Resolve(kv.first);
            if (index >= 0) motion_[static_cast<std::size_t>(index)] = expression_detail::ClampUnit(kv.second);
        }
        motionInfluence_ = expression_detail::ClampUnit(influence);
        releaseInfluence_ = -1.0f;
        Recompute(0.0f);
    }

    /// 结束临时叠层：0.2 秒淡出回落到用户当前表情。
    void ClearMotionWeights() {
        if (motionInfluence_ <= 0.0f) return;
        releaseInfluence_ = motionInfluence_ / 0.2f;
    }

    // --- 待机随机表情 -----------------------------------------------------
    //
    // 桌宠待机时每隔一段时间自己换一个表情（对齐 VRM-VPet 的「随机表情」）。
    // 与「手动切换表情」的区别：手动切换会一直保持到下一次手动操作，
    // 而随机表情是「换一个表情 → 停留几秒 → 回到中性」的循环。
    //
    // 用户手动选过表情后会进入「保护期」，保护期内不随机切换，
    // 避免用户刚选好表情就被系统改掉。

    /// 开关待机随机表情。
    void SetRandomIdle(bool enabled) {
        randomIdle_ = enabled;
        randomTimer_ = RandomIdleWait();
        randomHold_ = -1.0f;
        manualHold_ = 0.0f;
    }
    bool RandomIdle() const { return randomIdle_; }

    /// 设置随机间隔（秒）。min 必须大于 0。
    void SetRandomIdleInterval(float min, float max) {
        randomIdleMin_ = std::max(0.5f, min);
        randomIdleMax_ = std::max(randomIdleMin_, max);
    }

    /// 立刻随机抽一个表情并切换过去（返回是否抽到了）。
    bool TriggerRandomIdle() {
        // 收集可用候选：排除中性表情与当前表情。
        std::vector<const ExpressionPreset*> candidates;
        for (const ExpressionPreset& p : presets_) {
            if (p.id == "neutral" || p.id == activePreset_) continue;
            if (!IsPresetAvailable(p)) continue;
            candidates.push_back(&p);
        }
        if (candidates.empty()) return false;
        std::uniform_int_distribution<std::size_t> dist(0, candidates.size() - 1);
        const ExpressionPreset* chosen = candidates[dist(rng_)];
        if (!ApplyPreset(chosen->id)) return false;
        // 停留 2~5 秒后回到中性。
        std::uniform_real_distribution<float> hold(2.0f, 5.0f);
        randomHold_ = hold(rng_);
        randomTimer_ = RandomIdleWait();
        return true;
    }

    /// 用户手动切换表情后调用：在一段时间内不触发随机表情。
    void NotifyManualChange() { manualHold_ = 6.0f; }

    void Tick(float dt) {
        if (!(dt >= 0.0f)) dt = 0.0f;
        // 切后台回来时限制单帧步长，避免眨眼与过渡一次跳完。
        dt = std::min(dt, 0.1f);

        if (transition_.active) {
            transition_.elapsed += dt;
            const float progress = transition_.duration > 0.0f
                                       ? transition_.elapsed / transition_.duration : 1.0f;
            const float alpha = expression_detail::SmoothStep(progress);
            for (std::size_t i = 0; i < channels_.size(); ++i) {
                base_[i] = transitionFrom_[i] + (transitionTo_[i] - transitionFrom_[i]) * alpha;
            }
            if (progress >= 1.0f) transition_.Reset();
        }

        if (releaseInfluence_ >= 0.0f) {
            motionInfluence_ = std::max(0.0f, motionInfluence_ - dt * releaseInfluence_);
            if (motionInfluence_ <= 0.0f) {
                motionInfluence_ = 0.0f;
                releaseInfluence_ = -1.0f;
                std::fill(motion_.begin(), motion_.end(), 0.0f);
            }
        }

        float blink = 0.0f;
        if (autoBlink_) {
            if (blinkTime_ < 0.0f) {
                blinkWait_ -= dt;
                if (blinkWait_ <= 0.0f) blinkTime_ = 0.0f;
            } else {
                blinkTime_ += dt;
                if (blinkTime_ >= kBlinkDuration) {
                    blinkTime_ = -1.0f;
                    blinkWait_ = RandomBlinkWait();
                } else {
                    blink = std::sin(3.14159265f * blinkTime_ / kBlinkDuration);
                }
            }
        }

        // --- 待机随机表情 ---
        if (randomIdle_) {
            if (manualHold_ > 0.0f) {
                manualHold_ -= dt;
            } else if (randomHold_ >= 0.0f) {
                // 正在「保持某个随机表情」，到时间后回到中性。
                randomHold_ -= dt;
                if (randomHold_ <= 0.0f) {
                    randomHold_ = -1.0f;
                    ApplyPreset("neutral");
                }
            } else {
                randomTimer_ -= dt;
                if (randomTimer_ <= 0.0f) {
                    TriggerRandomIdle();
                }
            }
        }

        Recompute(blink);
    }

    /// 诊断面板用的一行状态。
    std::string StatusText() const {
        std::string out;
        if (activePreset_.empty()) out = "自定义表情";
        else {
            for (const ExpressionPreset& p : presets_) {
                if (p.id == activePreset_) { out = "表情预设：" + p.name; break; }
            }
            if (out.empty()) out = "表情预设：" + activePreset_;
        }
        if (motionInfluence_ > 0.0f) out += " / 动作表情叠层 " + std::to_string(static_cast<int>(motionInfluence_ * 100)) + "%";
        if (!autoBlink_) out += " / 自动眨眼关";
        return out;
    }

private:
    static constexpr float kBlinkDuration = 0.22f;

    struct Transition {
        bool active = false;
        float elapsed = 0.0f;
        float duration = 0.0f;
        void Reset() { active = false; elapsed = 0.0f; duration = 0.0f; }
    };

    float RandomBlinkWait() {
        std::uniform_real_distribution<float> dist(2.0f, 5.0f);
        return dist(rng_);
    }

    /// 下一次随机表情的等待时间。
    float RandomIdleWait() {
        std::uniform_real_distribution<float> dist(randomIdleMin_, randomIdleMax_);
        return dist(rng_);
    }

    void Recompute(float blink) {
        final_.resize(channels_.size());
        const int both = Resolve("blink");
        const int left = Resolve("blinkLeft");
        const int right = Resolve("blinkRight");
        for (std::size_t i = 0; i < channels_.size(); ++i) {
            float value = base_[i];
            value += (motion_[i] - value) * motionInfluence_;
            const int idx = static_cast<int>(i);
            if (blink > 0.0f && (idx == both || (both < 0 && (idx == left || idx == right)))) {
                value = std::max(value, blink);
            }
            // 二元表情：半睁半闭看起来很假，量化成 0 / 1。
            if (binary_[i] != 0) value = value >= 0.5f ? 1.0f : 0.0f;
            final_[i] = expression_detail::ClampUnit(value);
        }
    }

    std::vector<std::string> channels_;
    std::vector<std::string> lowered_;
    std::vector<char> binary_;
    std::vector<ExpressionPreset> presets_ = DefaultExpressionPresets();

    std::vector<float> base_;
    std::vector<float> final_;
    std::vector<float> motion_;
    std::vector<float> transitionFrom_;
    std::vector<float> transitionTo_;
    Transition transition_;
    std::string activePreset_ = "neutral";
    float motionInfluence_ = 0.0f;
    float releaseInfluence_ = -1.0f;

    bool autoBlink_ = true;
    float blinkWait_ = 3.0f;
    float blinkTime_ = -1.0f;

    // --- 待机随机表情 ---
    bool randomIdle_ = false;
    float randomIdleMin_ = 8.0f;   ///< 随机间隔下限（秒）
    float randomIdleMax_ = 20.0f;  ///< 随机间隔上限（秒）
    float randomTimer_ = 10.0f;    ///< 距离下一次随机的剩余时间
    float randomHold_ = -1.0f;     ///< 正在保持某个随机表情时的剩余时间（<0 表示不在保持）
    float manualHold_ = 0.0f;       ///< 用户手动切换后的保护期

    std::mt19937 rng_;
};

}  // namespace ppet
