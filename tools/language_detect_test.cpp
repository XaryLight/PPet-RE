// PPet - 语言检测单测（报告 4.1.5）。
// 只测纯逻辑：--lang 值解析与 locale 标签推断；系统探测只做弱断言。
#include <src/tools/core/LanguageDetect.hpp>

#include <cstdio>

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    using namespace ppet;
    std::printf("== 语言检测单测 ==\n");

    // --lang 值解析
    Check(ParseLanguageOption("zh") == Language::Chinese, "zh -> Chinese");
    Check(ParseLanguageOption("zh-CN") == Language::Chinese, "zh-CN -> Chinese");
    Check(ParseLanguageOption("zh_CN") == Language::Chinese, "zh_CN -> Chinese");
    Check(ParseLanguageOption("CHINESE") == Language::Chinese, "CHINESE(大写) -> Chinese");
    Check(ParseLanguageOption("en") == Language::English, "en -> English");
    Check(ParseLanguageOption("en-US") == Language::English, "en-US -> English");
    Check(ParseLanguageOption("english") == Language::English, "english -> English");
    Check(!ParseLanguageOption("fr").has_value(), "fr -> 无法识别");
    Check(!ParseLanguageOption("").has_value(), "空串 -> 无法识别");

    // locale 标签推断
    Check(LanguageFromLocaleTag("zh_CN.UTF-8") == Language::Chinese,
          "zh_CN.UTF-8 -> Chinese");
    Check(LanguageFromLocaleTag("en_US.UTF-8") == Language::English,
          "en_US.UTF-8 -> English");
    Check(LanguageFromLocaleTag("zh-Hans-CN") == Language::Chinese,
          "zh-Hans-CN -> Chinese");
    Check(LanguageFromLocaleTag("en-GB") == Language::English, "en-GB -> English");
    Check(LanguageFromLocaleTag("C") == std::nullopt, "C locale -> 无法识别");
    Check(LanguageFromLocaleTag("POSIX") == std::nullopt, "POSIX locale -> 无法识别");
    Check(LanguageFromLocaleTag("") == std::nullopt, "空 locale -> 无法识别");

    // 系统探测：只断言返回合法枚举值（CI 语言环境不定）。
    const Language sys = DetectSystemLanguage();
    Check(sys == Language::Chinese || sys == Language::English,
          "系统语言探测返回合法枚举");

    std::printf("\n结果：%d 项失败\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
