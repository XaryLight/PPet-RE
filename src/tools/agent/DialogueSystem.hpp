// PPet - C++ 移植版
// 语言功能（台词 / 气泡 / 口型）：PMX、VRM、Live2D 三种模型共用的纯逻辑层。
//
// 【参考来源】
//   FULASKA/-live2d- 的 src/stage/phrases.js + bubble.js + mouth.js：
//     · phrases：按状态（thinking/working/waiting/success/error/idle）分档的台词库，
//       随机抽且不连续重复；
//     · bubble：气泡优先级、时长、冷却、字数上限，避免刷屏；
//     · mouth：说话能量包络 -> 元音嘴形（aa/ih/ou/ee/oh）与情绪嘴形（笑/怒）。
//
// 【为什么做成纯头文件】
//   与 ExpressionDirector / MotionDirector 同一惯例：不碰 GL / Qt / 文件 IO，
//   可以脱离渲染上下文做单元测试。模型后端只做两件事：
//     1) 用 SpeechController 的「当前台词 + 进度」驱动气泡 UI；
//     2) 用 LipSync 的输出写口型参数 / morph。
//
// 【AI 对话状态接口】
//   AgentState 预留给外部 Agent（DSH / agent-link 之类）推送状态与台词，
//   默认实现是「本地随机台词」，接入后由外部接管「该说什么」。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

namespace ppet::agent {

/// 对话/工作状态。外部 Agent 推送时复用这套枚举，本地随机台词也按它分档。
enum class AgentStateKind {
    Idle,        ///< 待机（无任务）
    Thinking,    ///< 思考中
    Working,     ///< 执行任务中
    Waiting,     ///< 等待输入/工具返回
    Success,     ///< 成功
    Error,       ///< 出错
    Greeting,    ///< 打招呼（被摸头/点击时）
};

inline const char* AgentStateName(AgentStateKind kind) {
    switch (kind) {
        case AgentStateKind::Idle:      return "idle";
        case AgentStateKind::Thinking:  return "thinking";
        case AgentStateKind::Working:   return "working";
        case AgentStateKind::Waiting:   return "waiting";
        case AgentStateKind::Success:   return "success";
        case AgentStateKind::Error:     return "error";
        case AgentStateKind::Greeting:  return "greeting";
    }
    return "idle";
}

/// 一句话台词：文本内容 + 它所属的状态档。
struct Phrase {
    std::string text;
    AgentStateKind kind = AgentStateKind::Idle;
};

/// 台词库：按状态分档，随机抽且不连续重复同一句。
/// 纯逻辑、零依赖、可单测。
class PhraseBook {
public:
    explicit PhraseBook(std::uint32_t seed = 424242u) : rng_(seed) {}

    /// 注册一句台词。
    void Add(const std::string& text, AgentStateKind kind) {
        phrases_.push_back({text, kind});
    }

    /// 批量注册（构造时一次性灌入默认中文台词库用）。
    void AddBatch(const std::vector<Phrase>& phrases) {
        phrases_.insert(phrases_.end(), phrases.cbegin(), phrases.cend());
    }

    /// 按状态随机抽一句。该档为空时退回 Idle 档；都空时返回空串。
    /// @param avoidText 若非空，尽量不连续抽中与它相同的句子（避免「复读」）。
    std::string Pick(AgentStateKind kind, const std::string& avoidText = "") const {
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < phrases_.size(); ++i) {
            if (phrases_[i].kind == kind) candidates.push_back(i);
        }
        if (candidates.empty()) {
            for (std::size_t i = 0; i < phrases_.size(); ++i) {
                if (phrases_[i].kind == AgentStateKind::Idle) candidates.push_back(i);
            }
        }
        if (candidates.empty()) return std::string();

        // 过滤掉与 avoidText 完全相同的一句（除非只剩它自己）。
        std::vector<std::size_t> filtered;
        for (std::size_t i : candidates) {
            if (avoidText.empty() || phrases_[i].text != avoidText) filtered.push_back(i);
        }
        const std::vector<std::size_t>& pool = filtered.empty() ? candidates : filtered;

        std::uniform_int_distribution<std::size_t> dist(0, pool.size() - 1);
        return phrases_[pool[dist(rng_)]].text;
    }

    std::size_t Count() const { return phrases_.size(); }
    std::size_t CountOf(AgentStateKind kind) const {
        std::size_t n = 0;
        for (const Phrase& p : phrases_) if (p.kind == kind) ++n;
        return n;
    }

    const std::vector<Phrase>& All() const { return phrases_; }

private:
    std::vector<Phrase> phrases_;
    mutable std::mt19937 rng_;
};

/// 内置中文默认台词库（覆盖七档状态，每档不少于 3 句，避免很快复读）。
inline PhraseBook DefaultPhraseBook() {
    PhraseBook book;
    // 注意：这里只放「通用」台词；模型专属的台词可以由外部 Add() 注入。
    book.AddBatch({
        {"（发呆中）", AgentStateKind::Idle},
        {"今天也要加油呀～", AgentStateKind::Idle},
        {"在想晚饭吃什么呢", AgentStateKind::Idle},
        {"要不要陪你聊会儿？", AgentStateKind::Idle},
        {"窗外的天气好像不错", AgentStateKind::Idle},
        {"嗯……让我想想", AgentStateKind::Thinking},
        {"正在梳理思路", AgentStateKind::Thinking},
        {"稍等，我在思考", AgentStateKind::Thinking},
        {"这个问题的关键点是……", AgentStateKind::Thinking},
        {"马上就好，别急", AgentStateKind::Working},
        {"正在努力中！", AgentStateKind::Working},
        {"处理一下手头的任务", AgentStateKind::Working},
        {"进度已经过半啦", AgentStateKind::Working},
        {"在等一个结果回来", AgentStateKind::Waiting},
        {"马上就有回复了", AgentStateKind::Waiting},
        {"稍等，网络有点慢", AgentStateKind::Waiting},
        {"搞定！", AgentStateKind::Success},
        {"一切顺利～", AgentStateKind::Success},
        {"任务完成，撒花", AgentStateKind::Success},
        {"哎呀，出错了……", AgentStateKind::Error},
        {"这里好像有点问题", AgentStateKind::Error},
        {"再试一次看看？", AgentStateKind::Error},
        {"嗨，你好呀！", AgentStateKind::Greeting},
        {"嘿嘿，被你发现啦", AgentStateKind::Greeting},
        {"欢迎回来～", AgentStateKind::Greeting},
    });
    return book;
}

/// 气泡/说话状态机：控制「现在该不该说话、说什么、说多久」。
///
/// 设计要点（对齐 FULASKA 的 bubble.js）：
///   · 有优先级：互动/状态切换的台词优先级高于待机碎碎念；
///   · 有冷却：一句话说完前不会立刻被另一句覆盖（除非更高优先级）；
///   · 有字数上限：超长台词自动截断（避免气泡溢出）。
class SpeechController {
public:
    struct Options {
        float minTalkInterval = 12.0f;   ///< 待机碎碎念的最小间隔（秒）
        float maxTalkInterval = 25.0f;   ///< 待机碎碎念的最大间隔（秒）
        float baseDuration = 3.5f;       ///< 一句话基础时长（秒）
        float perCharDuration = 0.06f;   ///< 每个字额外时长
        float maxDuration = 8.0f;        ///< 单句最长显示时长
        std::size_t maxChars = 24;       ///< 气泡字数上限
        float cooldownAfter = 0.8f;      ///< 一句话结束后到可接下一句的最短冷却
    };

    explicit SpeechController(std::uint32_t seed = 998877u, const PhraseBook* book = nullptr)
        : book_(book), rng_(seed) {
        ScheduleIdle();
    }

    void SetPhraseBook(const PhraseBook* book) { book_ = book; }
    void SetOptions(const Options& o) { options_ = o; ScheduleIdle(); }

    /// 当前是否正在显示气泡。
    bool IsSpeaking() const { return remaining_ > 0.0f; }

    /// 当前台词文本（截断到字数上限）。
    const std::string& CurrentText() const { return currentText_; }

    /// 说话进度 [0,1]，供气泡做「打字机」动画或淡入淡出。
    float Progress() const {
        if (duration_ <= 0.0f) return IsSpeaking() ? 1.0f : 0.0f;
        return std::clamp(1.0f - remaining_ / duration_, 0.0f, 1.0f);
    }

    /// 强制说一句（互动/状态切换用）。优先级 >= 当前则打断。
    /// @return 是否成功（台词非空且被接受）。
    bool Say(const std::string& text, int priority = 1) {
        if (text.empty()) return false;
        if (IsSpeaking() && priority < activePriority_) return false;
        currentText_ = Truncate(text);
        duration_ = ClampDuration(static_cast<float>(text.size()));
        remaining_ = duration_;
        activePriority_ = priority;
        idleTimer_ = -1.0f;  // 打断待机碎碎念排程，说完后重新排
        lastText_ = text;
        return true;
    }

    /// 按状态说一句（从台词库抽取）。高于当前优先级才打断。
    bool SayState(AgentStateKind kind, int priority = 1) {
        if (!book_) return false;
        return Say(book_->Pick(kind, lastText_), priority);
    }

    /// 每帧推进；idleTalk=true 时允许触发待机碎碎念。
    void Tick(float dt, bool idleTalk = true) {
        if (dt < 0.0f) dt = 0.0f;
        dt = std::min(dt, 0.1f);

        if (IsSpeaking()) {
            remaining_ -= dt;
            if (remaining_ <= 0.0f) {
                remaining_ = 0.0f;
                activePriority_ = 0;
                // 说完后进入冷却再排下一句待机碎碎念。
                idleTimer_ = -options_.cooldownAfter;
            }
            return;
        }

        if (!idleTalk || !book_) return;
        // idleTimer_ < 0 表示「冷却中」：向 0 收敛。
        if (idleTimer_ < 0.0f) {
            idleTimer_ += dt;
            return;
        }
        idleTimer_ -= dt;
        if (idleTimer_ <= 0.0f) {
            SayState(AgentStateKind::Idle, 0);
            ScheduleIdle();
        }
    }

    const Options& GetOptions() const { return options_; }

private:
    void ScheduleIdle() {
        std::uniform_real_distribution<float> dist(options_.minTalkInterval,
                                                   options_.maxTalkInterval);
        idleTimer_ = dist(rng_);
    }
    std::string Truncate(const std::string& text) const {
        // 按 UTF-8 码点截断（中英文混排时「字数上限」指字符数，不是字节数）。
        std::string out;
        std::size_t chars = 0;
        for (std::size_t i = 0; i < text.size() && chars < options_.maxChars; ) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            const int len = (c < 0x80)                 ? 1
                          : ((c >> 5) == 0x6)          ? 2
                          : ((c >> 4) == 0xE)          ? 3
                          : ((c >> 3) == 0x1E)         ? 4
                                                       : 1;
            const std::size_t take = (i + static_cast<std::size_t>(len) <= text.size())
                                        ? static_cast<std::size_t>(len) : 1;
            out.append(text, i, take);
            i += take;
            ++chars;
        }
        // 截断发生在码点边界，不会把多字节汉字劈成乱码。
        if (out.size() < text.size()) return out + "…";
        return text;
    }
    float ClampDuration(float chars) const {
        const float d = options_.baseDuration + chars * options_.perCharDuration;
        return std::min(options_.maxDuration, d);
    }

    Options options_;
    const PhraseBook* book_ = nullptr;
    std::mt19937 rng_;

    std::string currentText_;
    std::string lastText_;
    float remaining_ = 0.0f;
    float duration_ = 0.0f;
    int activePriority_ = 0;
    float idleTimer_ = 0.0f;
};

/// 口型同步：把「说话进度 + 当前情绪」转换成口型权重。
///
/// 输出两类，适配不同模型后端：
///   1) 开口度 mouthOpen ∈ [0,1] 与嘴形 mouthForm ∈ [-1,1]（Live2D 的
///      ParamMouthOpenY / ParamMouthForm）；
///   2) 五个元音权重 aa/ih/ou/ee/oh（PMX/VRM 的口型 morph）。
///
/// 设计要点（对齐 FULASKA 的 mouth.js）：
///   · 说话能量：用「音节包络」驱动开口度，而不是匀速张嘴，更像真人在说话；
///   · 元音嘴形：每个音节带一个元音，开口时该元音权重拉满；
///   · 情绪嘴形：笑时嘴角上扬（form 偏正）、怒时偏负，叠加在元音之上。
class LipSync {
public:
    struct Vowels {
        float aa = 0.0f;
        float ih = 0.0f;
        float ou = 0.0f;
        float ee = 0.0f;
        float oh = 0.0f;
    };

    /// 设置说话强度（0=闭嘴，1=全力说话）。外部由 SpeechController 的进度驱动。
    void SetIntensity(float intensity) { intensity_ = std::clamp(intensity, 0.0f, 1.0f); }

    /// 设置当前情绪（影响 mouthForm），可选值见 AgentStateKind 的语义映射。
    void SetEmotion(AgentStateKind kind) { emotion_ = kind; }

    /// 设置呼吸微动幅度（静止时嘴也会轻微起伏）。
    void SetBreathAmplitude(float a) { breathAmp_ = std::clamp(a, 0.0f, 0.3f); }

    /// 每帧推进，生成「当前这一帧」的口型。
    /// @param dt        时间步长（秒）
    /// @param phase     连续相位（用于呼吸微动，可由外部累积传入）
    void Tick(float dt, float phase = -1.0f) {
        elapsed_ += dt;
        if (phase >= 0.0f) breathPhase_ = phase;
        else breathPhase_ += dt * kBreathSpeed;

        // 呼吸微动：静止时也轻微张合。
        const float breath = (std::sin(breathPhase_ * 2.0f * 3.14159265f) * 0.5f + 0.5f) *
                            breathAmp_;

        // 说话能量包络：用「音节」的半正弦模拟一个一个字的开口，
        // 频率随强度提高（说得越快音节越密）。
        const float syllableRate = 3.0f + intensity_ * 7.0f;
        const float syllable = intensity_ > 0.01f
                                  ? std::fabs(std::sin(elapsed_ * syllableRate))
                                  : 0.0f;
        mouthOpen_ = std::min(1.0f, breath + syllable * intensity_);

        // 嘴形：元音随音节切换，情绪做整体偏置。
        float form = 0.0f;
        if (emotion_ == AgentStateKind::Success || emotion_ == AgentStateKind::Greeting) {
            form += 0.4f;  // 笑：嘴角上扬
        } else if (emotion_ == AgentStateKind::Error) {
            form -= 0.4f;  // 错：嘴角下压
        }
        mouthForm_ = std::clamp(form, -1.0f, 1.0f);

        // 元音：当前音节对应的元音权重拉满，其余回落。
        const int vowelIndex = static_cast<int>(elapsed_ * syllableRate) % 5;
        vowels_.aa = vowelIndex == 0 ? mouthOpen_ : 0.0f;
        vowels_.ih = vowelIndex == 1 ? mouthOpen_ : 0.0f;
        vowels_.ou = vowelIndex == 2 ? mouthOpen_ : 0.0f;
        vowels_.ee = vowelIndex == 3 ? mouthOpen_ : 0.0f;
        vowels_.oh = vowelIndex == 4 ? mouthOpen_ : 0.0f;
    }

    float MouthOpen() const { return mouthOpen_; }
    float MouthForm() const { return mouthForm_; }
    const Vowels& GetVowels() const { return vowels_; }

private:
    static constexpr float kBreathSpeed = 1.1f;

    float intensity_ = 0.0f;
    AgentStateKind emotion_ = AgentStateKind::Idle;
    float breathAmp_ = 0.06f;

    float elapsed_ = 0.0f;
    float breathPhase_ = 0.0f;
    float mouthOpen_ = 0.0f;
    float mouthForm_ = 0.0f;
    Vowels vowels_{};
};

}  // namespace ppet::agent
