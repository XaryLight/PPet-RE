// PPet - C++ 移植版
// 骨骼动作（Bone Motion）实现。
#include <src/tools/model/BoneMotion.hpp>

#include <algorithm>
#include <cmath>

namespace ppet::model {

// ---------------------------------------------------------------------------
// BoneMotionPlayer
// ---------------------------------------------------------------------------

bool BoneMotionPlayer::Play(const BoneMotion& motion, float fadeIn) {
    if (motion.keys.empty()) return false;
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
    if (!playing_) return;
    if (fadeOut <= 0.0f) {
        playing_ = false;
        fadeOut_ = 0.0f;
        return;
    }
    fadeOutDuration_ = fadeOut;
    fadeOut_ = 0.0f;
}

float BoneMotionPlayer::Progress() const {
    if (!playing_ || current_.duration <= 0.0f) return 0.0f;
    return std::clamp(time_ / current_.duration, 0.0f, 1.0f);
}

void BoneMotionPlayer::Interpolate(const BoneMotion& motion, float time, BonePose& pose) {
    pose.entries.clear();

    // 按骨骼分组处理：同一骨骼可能有多条关键帧，必须先归并再插值，
    // 否则同一个骨骼会被输出多条互相覆盖的条目。
    //
    // 关键帧数量很少（每个骨骼 2~5 个），直接线性扫描即可。
    for (std::size_t i = 0; i < motion.keys.size(); ++i) {
        const BoneKey& first = motion.keys[i];
        if (first.bone.empty()) continue;

        // 同一骨骼的关键帧数量（文件顺序即时间顺序）。
        std::size_t count = 0;
        for (std::size_t j = i; j < motion.keys.size(); ++j) {
            if (motion.keys[j].bone != first.bone) continue;
            ++count;
        }
        if (count == 0) continue;

        // 找出该骨骼在 time 两侧的关键帧。
        const BoneKey* before = nullptr;
        const BoneKey* after = nullptr;
        for (std::size_t j = i; j < motion.keys.size(); ++j) {
            const BoneKey& key = motion.keys[j];
            if (key.bone != first.bone) continue;
            if (key.time <= time) {
                before = &key;
            } else if (after == nullptr) {
                after = &key;
            }
        }

        float pitch = 0.0f;
        float yaw = 0.0f;
        float roll = 0.0f;
        float weight = 0.0f;

        if (before == nullptr && after == nullptr) {
            // 时间点在所有关键帧之前：用第一帧的姿态。
            pitch = first.pitch;
            yaw = first.yaw;
            roll = first.roll;
            weight = first.weight;
        } else if (after == nullptr) {
            // 超出最后一帧：保持最后一帧。
            pitch = before->pitch;
            yaw = before->yaw;
            roll = before->roll;
            weight = before->weight;
        } else if (before == nullptr) {
            // 还没到第一帧：用第一帧的姿态（不外推）。
            pitch = after->pitch;
            yaw = after->yaw;
            roll = after->roll;
            weight = after->weight;
        } else {
            // 在两帧之间：按时间比例插值。
            const float span = after->time - before->time;
            const float t =
                span > 1e-6f ? std::clamp((time - before->time) / span, 0.0f, 1.0f) : 1.0f;
            pitch = before->pitch + (after->pitch - before->pitch) * t;
            yaw = before->yaw + (after->yaw - before->yaw) * t;
            roll = before->roll + (after->roll - before->roll) * t;
            weight = before->weight + (after->weight - before->weight) * t;
        }

        BonePose::Entry entry;
        entry.bone = first.bone;
        // 权重作用在角度上：weight=0 表示该帧不产生位移。
        entry.pitch = pitch * weight;
        entry.yaw = yaw * weight;
        entry.roll = roll * weight;
        pose.entries.push_back(entry);

        // 跳过同一骨骼的其它关键帧。
        i += count - 1;
    }
}

void BoneMotionPlayer::Sample(float time, BonePose& pose) const {
    Interpolate(current_, time, pose);
}

void BoneMotionPlayer::Tick(float dt, BonePose& pose) {
    pose.entries.clear();

    // 随机待机调度：即使当前没有动作也在走计时器。
    randomTimer_ -= dt;
    if (randomTimer_ <= 0.0f) {
        randomTimer_ = randomMin_ +
                       (randomMax_ - randomMin_) *
                           (0.5f * (std::sin(static_cast<float>(randomCursor_ * 2.399963f)) + 1.0f));
        // 上面的三角波保证落在 [randomMin, randomMax] 内。
        if (!playing_) {
            TriggerRandom();
        }
    }

    if (!playing_) return;

    time_ += dt;

    // 淡入。
    if (fadeIn_ < 1.0f) {
        if (fadeInDuration_ > 0.0f) {
            fadeIn_ = std::min(1.0f, fadeIn_ + dt / fadeInDuration_);
        } else {
            fadeIn_ = 1.0f;
        }
    }

    // 淡出。
    float weight = fadeIn_;
    if (fadeOutDuration_ > 0.0f) {
        fadeOut_ = std::min(1.0f, fadeOut_ + dt / fadeOutDuration_);
        weight *= (1.0f - fadeOut_);
        if (fadeOut_ >= 1.0f) {
            playing_ = false;
            return;
        }
    }

    // 采样（循环动作取模）。
    float sampleTime = time_;
    if (current_.loop && current_.duration > 0.0f) {
        sampleTime = std::fmod(time_, current_.duration);
    }

    BonePose sampled;
    Sample(sampleTime, sampled);

    // 应用权重。
    for (const BonePose::Entry& entry : sampled.entries) {
        BonePose::Entry out;
        out.bone = entry.bone;
        out.pitch = entry.pitch * weight;
        out.yaw = entry.yaw * weight;
        out.roll = entry.roll * weight;
        pose.entries.push_back(out);
    }

    // 动作播完：一次性动作自动结束，回到待机。
    if (!current_.loop && time_ >= current_.duration) {
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
    // 右臂挥手时向内抬（负 roll），左臂则向外抬。
    const float lift = right ? -70.0f : 70.0f;
    const float side = right ? -18.0f : 18.0f;

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
