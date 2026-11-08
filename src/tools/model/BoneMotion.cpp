// PPet - C++ 移植版
// 骨骼动作（Bone Motion）实现。
#include <src/tools/model/BoneMotion.hpp>

#include <algorithm>
#include <cmath>

namespace ppet::model {

namespace {

/// 平滑过渡曲线：比线性淡入淡出更接近官方 Framework 的 EasingSine 观感，
/// 且端点导数为 0，切换动作时不会出现速度突变。
float SmoothStep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

}  // namespace

// ---------------------------------------------------------------------------
// BoneMotionPlayer
// ---------------------------------------------------------------------------

bool BoneMotionPlayer::Play(const BoneMotion& motion, float fadeIn) {
    if (motion.keys.empty()) return false;

    // 把正在播放的动作冻结成「淡出层」：切换瞬间不会把旧姿态直接抹掉，
    // 而是让它在 crossFade 秒内平滑退场，同时新动作淡入。
    if (playing_ && !current_.keys.empty() && Weight() > 0.001f && crossFade_ > 0.0f) {
        outgoing_ = current_;
        outgoingTime_ = time_;
        outgoingWeight_ = Weight();
        outgoingFade_ = 0.0f;
        outgoingFadeDuration_ = std::max(crossFade_, fadeIn);
        hasOutgoing_ = true;
    } else {
        hasOutgoing_ = false;
    }

    current_ = motion;
    time_ = 0.0f;
    playing_ = true;
    fadeInDuration_ = std::max(0.0f, fadeIn);
    fadeIn_ = fadeInDuration_ > 0.0f ? 0.0f : 1.0f;
    fadeOutDuration_ = 0.0f;
    fadeOut_ = 0.0f;
    return true;
}

void BoneMotionPlayer::Stop(float fadeOut) {
    if (fadeOut <= 0.0f) {
        playing_ = false;
        fadeOut_ = 0.0f;
        fadeOutDuration_ = 0.0f;
        hasOutgoing_ = false;
        return;
    }
    if (!playing_) return;
    fadeOutDuration_ = fadeOut;
    fadeOut_ = 0.0f;
}

float BoneMotionPlayer::Progress() const {
    if (!playing_ || current_.duration <= 0.0f) return 0.0f;
    return std::clamp(time_ / current_.duration, 0.0f, 1.0f);
}

float BoneMotionPlayer::Weight() const {
    if (!playing_) return 0.0f;
    return SmoothStep(fadeIn_) * (1.0f - SmoothStep(fadeOut_));
}

void BoneMotionPlayer::Interpolate(const BoneMotion& motion, float time, BonePose& pose) {
    pose.entries.clear();

    // 按骨骼名归并，再按时间排序后插值。
    //
    // 【为什么要重写】内置动作的关键帧是「head / spine / head / spine ...」交替排列的，
    // 旧实现假设同一骨骼的关键帧连续，用 `i += count - 1` 跳帧，结果同一根骨骼会被
    // 输出多条互相矛盾的条目；后端逐条连乘旋转，动作姿态实际是错的。
    // 现在每根骨骼只输出一条，交叉淡入淡出按名字累加时也不会重复。
    std::vector<std::string> names;
    names.reserve(motion.keys.size());
    for (const BoneKey& key : motion.keys) {
        if (key.bone.empty()) continue;
        if (std::find(names.begin(), names.end(), key.bone) == names.end()) {
            names.push_back(key.bone);
        }
    }

    for (const std::string& name : names) {
        std::vector<const BoneKey*> keys;
        keys.reserve(4);
        for (const BoneKey& key : motion.keys) {
            if (key.bone == name) keys.push_back(&key);
        }
        std::sort(keys.begin(), keys.end(),
                  [](const BoneKey* a, const BoneKey* b) { return a->time < b->time; });
        if (keys.empty()) continue;

        const BoneKey* before = nullptr;
        const BoneKey* after = nullptr;
        for (const BoneKey* key : keys) {
            if (key->time <= time) {
                before = key;
            } else if (after == nullptr) {
                after = key;
            }
        }

        const BoneKey& first = *keys.front();
        const BoneKey& last = *keys.back();
        float pitch = 0.0f;
        float yaw = 0.0f;
        float roll = 0.0f;
        float weight = 1.0f;

        if (before != nullptr && after != nullptr) {
            const float span = after->time - before->time;
            const float t =
                span > 1e-6f ? std::clamp((time - before->time) / span, 0.0f, 1.0f) : 1.0f;
            pitch = before->pitch + (after->pitch - before->pitch) * t;
            yaw = before->yaw + (after->yaw - before->yaw) * t;
            roll = before->roll + (after->roll - before->roll) * t;
            weight = before->weight + (after->weight - before->weight) * t;
        } else {
            // 时间落在第一帧之前或最后一帧之后：保持端点姿态，不外推。
            const BoneKey& endpoint = before != nullptr ? last : first;
            pitch = endpoint.pitch;
            yaw = endpoint.yaw;
            roll = endpoint.roll;
            weight = endpoint.weight;
        }

        BonePose::Entry entry;
        entry.bone = name;
        // 权重作用在角度上：weight=0 表示该帧不产生位移。
        entry.pitch = pitch * weight;
        entry.yaw = yaw * weight;
        entry.roll = roll * weight;
        pose.entries.push_back(entry);
    }
}

void BoneMotionPlayer::Sample(const BoneMotion& motion, float time, BonePose& pose) const {
    Interpolate(motion, time, pose);
}

void BoneMotionPlayer::Accumulate(BonePose& dst, const BonePose& layer, float weight) {
    if (weight <= 1e-5f) return;
    for (const BonePose::Entry& entry : layer.entries) {
        auto it = std::find_if(dst.entries.begin(), dst.entries.end(),
                               [&](const BonePose::Entry& d) { return d.bone == entry.bone; });
        if (it == dst.entries.end()) {
            BonePose::Entry out;
            out.bone = entry.bone;
            out.pitch = entry.pitch * weight;
            out.yaw = entry.yaw * weight;
            out.roll = entry.roll * weight;
            dst.entries.push_back(out);
        } else {
            // 交叉淡入淡出时新旧两层会命中同一根骨骼，必须累加而不是各写一条。
            it->pitch += entry.pitch * weight;
            it->yaw += entry.yaw * weight;
            it->roll += entry.roll * weight;
        }
    }
}

void BoneMotionPlayer::Tick(float dt, BonePose& pose) {
    pose.entries.clear();
    const float step = std::max(0.0f, dt);

    // 随机待机调度：即使当前没有动作也在走计时器。
    if (step > 0.0f) {
        randomTimer_ -= step;
        if (randomTimer_ <= 0.0f) {
            randomTimer_ = randomMin_ +
                           (randomMax_ - randomMin_) *
                               (0.5f * (std::sin(static_cast<float>(randomCursor_ * 2.399963f)) + 1.0f));
            if (!playing_) TriggerRandom();
        }
    }

    // --- 淡出层：上一个动作继续沿自己的时间轴走，权重平滑归零 ---
    if (hasOutgoing_) {
        outgoingTime_ += step;
        if (outgoing_.loop && outgoing_.duration > 0.0f) {
            outgoingTime_ = std::fmod(outgoingTime_, outgoing_.duration);
        }
        outgoingFade_ = outgoingFadeDuration_ > 0.0f
                            ? std::min(1.0f, outgoingFade_ + step / outgoingFadeDuration_)
                            : 1.0f;
        const float w = outgoingWeight_ * (1.0f - SmoothStep(outgoingFade_));
        if (w > 1e-4f) {
            BonePose layer;
            Sample(outgoing_, outgoingTime_, layer);
            Accumulate(pose, layer, w);
        } else {
            hasOutgoing_ = false;
        }
    }

    if (!playing_) return;

    time_ += step;

    if (fadeIn_ < 1.0f) {
        fadeIn_ = fadeInDuration_ > 0.0f
                      ? std::min(1.0f, fadeIn_ + step / fadeInDuration_)
                      : 1.0f;
    }

    // 淡出：显式 Stop(fadeOut)，或一次性动作播完后的自动淡出。
    // 后者是「动作之间需要过渡」的关键：旧实现播完瞬间把姿态清零，会看到明显一顿。
    if (fadeOutDuration_ > 0.0f) {
        fadeOut_ = std::min(1.0f, fadeOut_ + step / fadeOutDuration_);
    } else if (!current_.loop && endFade_ > 0.0f && time_ >= current_.duration) {
        fadeOutDuration_ = endFade_;
        fadeOut_ = 0.0f;
    }

    const float w = Weight();
    if (w > 1e-4f) {
        float sampleTime = time_;
        if (current_.loop && current_.duration > 0.0f) {
            sampleTime = std::fmod(time_, current_.duration);
        }
        BonePose layer;
        Sample(current_, sampleTime, layer);
        Accumulate(pose, layer, w);
    }

    // 结束条件：淡出完成，或（关闭自动淡出时）一次性动作时间到。
    if (fadeOutDuration_ > 0.0f && fadeOut_ >= 1.0f) {
        playing_ = false;
    } else if (fadeOutDuration_ <= 0.0f && !current_.loop && time_ >= current_.duration) {
        playing_ = false;
    }
}

bool BoneMotionPlayer::TriggerRandom() {
    if (randomMotions_.empty()) return false;
    // 用轮转 + 伪随机偏移，避免每次都是同一个动作。
    const std::size_t count = randomMotions_.size();
    const std::size_t index = (randomCursor_ + randomMotions_.size() / 2) % count;
    randomCursor_ = (randomCursor_ + 1) % count;

    // 概率判定：用游标做确定性伪随机（不引入随机数引擎，便于复现）。
    const float roll = static_cast<float>((index * 37 + 11) % 100) / 100.0f;
    if (roll > randomChance_) return false;

    return Play(randomMotions_[index]);
}

// ---------------------------------------------------------------------------
// 内置动作库
// ---------------------------------------------------------------------------

namespace bone_motions {

BoneMotion Nod() {
    BoneMotion motion;
    motion.id = "nod";
    motion.name = "点头";
    motion.duration = 1.1f;
    motion.loop = false;
    motion.keys = {
        // 起手：微微抬头
        {"head", 0.00f, -6.0f, 0.0f, 0.0f, 1.0f},
        {"spine", 0.00f, -2.0f, 0.0f, 0.0f, 1.0f},
        // 第一下点头
        {"head", 0.18f, 18.0f, 0.0f, 0.0f, 1.0f},
        {"spine", 0.18f, 5.0f, 0.0f, 0.0f, 1.0f},
        {"head", 0.34f, 2.0f, 0.0f, 0.0f, 1.0f},
        {"spine", 0.34f, 1.0f, 0.0f, 0.0f, 1.0f},
        // 第二下点头
        {"head", 0.56f, 16.0f, 0.0f, 0.0f, 1.0f},
        {"spine", 0.56f, 4.0f, 0.0f, 0.0f, 1.0f},
        {"head", 0.72f, 2.0f, 0.0f, 0.0f, 1.0f},
        {"spine", 0.72f, 1.0f, 0.0f, 0.0f, 1.0f},
        // 回到初始
        {"head", 1.10f, 0.0f, 0.0f, 0.0f, 1.0f},
        {"spine", 1.10f, 0.0f, 0.0f, 0.0f, 1.0f},
    };
    return motion;
}

BoneMotion Shake() {
    BoneMotion motion;
    motion.id = "shake";
    motion.name = "摇头";
    motion.duration = 1.2f;
    motion.loop = false;
    motion.keys = {
        {"head", 0.00f, 0.0f, 0.0f, 0.0f, 1.0f},
        {"spine", 0.00f, 0.0f, 0.0f, 0.0f, 1.0f},
        {"head", 0.16f, 0.0f, -14.0f, 4.0f, 1.0f},
        {"spine", 0.16f, 0.0f, -4.0f, 0.0f, 1.0f},
        {"head", 0.40f, 0.0f, 14.0f, -4.0f, 1.0f},
        {"spine", 0.40f, 0.0f, 4.0f, 0.0f, 1.0f},
        {"head", 0.64f, 0.0f, -10.0f, 3.0f, 1.0f},
        {"spine", 0.64f, 0.0f, -3.0f, 0.0f, 1.0f},
        {"head", 0.86f, 0.0f, 5.0f, 0.0f, 1.0f},
        {"spine", 0.86f, 0.0f, 1.0f, 0.0f, 1.0f},
        {"head", 1.20f, 0.0f, 0.0f, 0.0f, 1.0f},
        {"spine", 1.20f, 0.0f, 0.0f, 0.0f, 1.0f},
    };
    return motion;
}

BoneMotion Wave(const std::string& arm) {
    const bool right = arm != "leftArm";
    const std::string shoulder = right ? "rightArm" : "leftArm";
    const std::string forearm = right ? "rightForearm" : "leftForearm";
    // 抬手方向由几何决定：上臂在局部坐标系里沿 ±X 延伸，绕 Z 转 φ 时
    // +X -> (cosφ, sinφ)。右臂（+X）要抬起来必须 φ>0，左臂（-X）则 φ<0。
    // 旧版取了相反符号，挥手实际是把胳膊往下压，看起来「动作不明显」。
    const float lift = right ? 70.0f : -70.0f;
    const float side = right ? 18.0f : -18.0f;

    BoneMotion motion;
    motion.id = right ? "wave" : "waveLeft";
    motion.name = "挥手";
    motion.duration = 1.6f;
    motion.loop = false;
    motion.keys = {
        {"head", 0.00f, 0.0f, 0.0f, 0.0f, 1.0f},
        {shoulder, 0.00f, 0.0f, 0.0f, 0.0f, 1.0f},
        {forearm, 0.00f, 0.0f, 0.0f, 0.0f, 1.0f},
        // 抬起
        {shoulder, 0.30f, 0.0f, 0.0f, lift, 1.0f},
        {forearm, 0.30f, 0.0f, 0.0f, 0.0f, 1.0f},
        {shoulder, 0.55f, -8.0f, 0.0f, lift + 6.0f, 1.0f},
        // 左右摆
        {shoulder, 0.80f, 8.0f, 0.0f, lift + 6.0f, 1.0f},
        {shoulder, 1.05f, -8.0f, 0.0f, lift + 6.0f, 1.0f},
        {shoulder, 1.30f, 6.0f, 0.0f, lift + 4.0f, 1.0f},
        {forearm, 0.80f, 0.0f, 0.0f, 20.0f, 1.0f},
        {forearm, 1.30f, 0.0f, 0.0f, 15.0f, 1.0f},
        // 放下
        {shoulder, 1.60f, 0.0f, 0.0f, 0.0f, 1.0f},
        {forearm, 1.60f, 0.0f, 0.0f, 0.0f, 1.0f},
        {"head", 1.60f, 0.0f, 0.0f, 0.0f, 1.0f},
    };
    (void)side;
    return motion;
}

std::vector<BoneMotion> Builtin() {
    std::vector<BoneMotion> out;
    out.push_back(Nod());
    out.push_back(Shake());
    out.push_back(Wave("rightArm"));
    out.push_back(Wave("leftArm"));
    return out;
}

bool Find(const std::string& id, BoneMotion& out) {
    for (const BoneMotion& motion : Builtin()) {
        if (motion.id == id) {
            out = motion;
            return true;
        }
    }
    return false;
}

}  // namespace bone_motions

}  // namespace ppet::model
