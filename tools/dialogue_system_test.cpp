// PPet - C++ 移植版
// 语言功能（DialogueSystem.hpp）单元测试。
//
// 覆盖：台词库分档随机（不连续重复）/ 气泡优先级与冷却 / 字数上限截断 /
// 口型音节包络（开口度、元音权重）/ 情绪嘴形偏置。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <src/tools/agent/DialogueSystem.hpp>

namespace {

int failures = 0;
int checks = 0;

void Check(bool cond, const char* what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  [FAIL] %s\n", what);
    }
}

using namespace ppet::agent;

void TestPhraseBook() {
    std::printf("-- 台词库 PhraseBook --\n");
    PhraseBook book(1u);
    book.Add("A", AgentStateKind::Idle);
    book.Add("B", AgentStateKind::Idle);
    book.Add("C", AgentStateKind::Error);
    Check(book.Count() == 3, "Count=3");
    Check(book.CountOf(AgentStateKind::Idle) == 2, "Idle 档 2 句");
    Check(book.CountOf(AgentStateKind::Error) == 1, "Error 档 1 句");

    // 空档退回 Idle 档；Idle 也空则空串。
    PhraseBook empty;
    Check(empty.Pick(AgentStateKind::Thinking) == "", "空库 Pick 空串");
    PhraseBook onlyError;
    onlyError.Add("E", AgentStateKind::Error);
    // Idle 档为空且 Idle 自身也空：无可用台词，返回空串（不跨档乱抽）。
    Check(onlyError.Pick(AgentStateKind::Idle).empty(), "Idle/Error 皆空时返回空串");

    // 不连续重复：连续抽 20 次，应出现两种文本且不会连续相同。
    std::string prev;
    bool noRepeat = true;
    for (int i = 0; i < 20; ++i) {
        const std::string t = book.Pick(AgentStateKind::Idle, prev);
        if (t == prev) noRepeat = false;
        prev = t;
    }
    Check(noRepeat, "避免连续重复同一句");
}

void TestSpeechController() {
    std::printf("-- 气泡状态机 SpeechController --\n");
    PhraseBook book(7u);
    book.AddBatch(DefaultPhraseBook().All());
    SpeechController sc(3u, &book);
    Check(!sc.IsSpeaking(), "初始不说话");
    Check(sc.Say("你好呀", 1), "Say 成功");
    Check(sc.IsSpeaking(), "Say 后正在说话");
    Check(sc.CurrentText() == "你好呀", "当前台词正确");
    Check(sc.Progress() <= 1.0f, "进度 <= 1");

    // 字数上限截断（按 UTF-8 码点，不会劈坏汉字）。
    SpeechController sc2(3u, &book);
    const std::string longText = "这是一句非常非常非常非常非常非常非常非常非常非常非常长长长长长的台词";
    sc2.Say(longText, 1);
    Check(sc2.CurrentText() != longText, "超长台词被截断");
    Check(sc2.CurrentText().size() < longText.size(), "截断后更短（含省略号）");

    // 优先级：低优先级不打断高优先级。
    SpeechController sc3(3u, &book);
    sc3.Say("重要通知", 5);
    Check(!sc3.Say("闲聊", 1), "低优先级不打断高优先级");

    // 推进到说完。
    for (int i = 0; i < 200 && sc.IsSpeaking(); ++i) sc.Tick(0.1f);
    Check(!sc.IsSpeaking(), "推进后说完");

    // 待机碎碎念：长时间不说话会触发 Idle 台词。
    SpeechController sc4(3u, &book);
    for (int i = 0; i < 400 && !sc4.IsSpeaking(); ++i) sc4.Tick(0.1f);
    Check(sc4.IsSpeaking(), "空闲一段时间后会自发碎碎念");
}

void TestLipSync() {
    std::printf("-- 口型同步 LipSync --\n");
    LipSync lip;
    // 闭嘴时开口度应接近 0（仅呼吸微动）。
    lip.SetIntensity(0.0f);
    lip.Tick(0.016f, 0.0f);
    Check(lip.MouthOpen() < 0.1f, "闭嘴时开口度很小");

    // 全力说话：开口度应显著大于 0。
    lip.SetIntensity(1.0f);
    float maxOpen = 0.0f;
    for (int i = 0; i < 60; ++i) {
        lip.Tick(0.016f);
        maxOpen = std::max(maxOpen, lip.MouthOpen());
    }
    Check(maxOpen > 0.5f, "说话时开口度显著上升");

    // 元音：同一时刻只有一个元音权重接近当前开口度，其余近 0。
    lip.SetIntensity(1.0f);
    lip.Tick(0.016f);
    const LipSync::Vowels v = lip.GetVowels();
    std::vector<float> vals = {v.aa, v.ih, v.ou, v.ee, v.oh};
    int nonzero = 0;
    for (float x : vals) if (x > 0.3f) ++nonzero;
    Check(nonzero <= 1, "同一时刻最多一个元音活跃");

    // 情绪嘴形偏置。
    LipSync lip2;
    lip2.SetEmotion(AgentStateKind::Success);
    lip2.SetIntensity(0.0f);
    lip2.Tick(0.016f);
    Check(lip2.MouthForm() > 0.0f, "成功情绪嘴形偏正（笑）");
    LipSync lip3;
    lip3.SetEmotion(AgentStateKind::Error);
    lip3.SetIntensity(0.0f);
    lip3.Tick(0.016f);
    Check(lip3.MouthForm() < 0.0f, "错误情绪嘴形偏负");
}

}  // namespace

int main() {
    std::printf("=== DialogueSystem 单元测试 ===\n");
    TestPhraseBook();
    TestSpeechController();
    TestLipSync();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
