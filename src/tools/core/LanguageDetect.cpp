// PPet - 语言检测实现（见 LanguageDetect.hpp）。
#include <src/tools/core/LanguageDetect.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFLocale.h>
#endif

namespace ppet {
namespace {

/// 转小写，供大小写不敏感比较。
std::string Lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

/// 取 locale 标签的主语言部分（到首个 '-' 或 '_' 为止）。
std::string_view PrimaryTag(std::string_view tag) {
    const std::size_t pos = tag.find_first_of("-_@.");
    return pos == std::string_view::npos ? tag : tag.substr(0, pos);
}

std::optional<Language> FromPrimary(std::string_view primary) {
    const std::string p = Lower(primary);
    if (p == "zh" || p == "cmn" || p == "chinese" || p == "hans" || p == "hant") {
        return Language::Chinese;
    }
    if (p == "en" || p == "english") {
        return Language::English;
    }
    return std::nullopt;
}

const char* GetEnv(const char* name) {
#if defined(_WIN32)
    // _dupenv_s 比 getenv 在 MSVC 下更安全（不返回可能被后续调用覆盖的指针）。
    char* buf = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buf, &size, name) == 0 && buf != nullptr) {
        const std::string value(buf);
        std::free(buf);
        // 用静态缓冲返回（单线程启动期调用，无并发）。
        static thread_local std::string slot;
        slot = value;
        return slot.c_str();
    }
    return nullptr;
#else
    return std::getenv(name);
#endif
}

}  // namespace

std::optional<Language> ParseLanguageOption(std::string_view value) {
    const std::string v = Lower(value);
    if (v == "zh" || v == "zh-cn" || v == "zh_cn" || v == "chinese" ||
        v == "zh-hans" || v == "zh-hant") {
        return Language::Chinese;
    }
    if (v == "en" || v == "en-us" || v == "en_us" || v == "english" || v == "en-gb") {
        return Language::English;
    }
    return std::nullopt;
}

std::optional<Language> LanguageFromLocaleTag(std::string_view tag) {
    if (tag.empty()) return std::nullopt;
    return FromPrimary(PrimaryTag(tag));
}

Language DetectSystemLanguage() {
    // 1) 标准环境变量（Linux/macOS 常见）。
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (const char* val = GetEnv(var)) {
            if (auto lang = LanguageFromLocaleTag(val)) return *lang;
        }
    }

#if defined(_WIN32)
    // 2) Windows 用户首选 UI 语言。
    if (const ULONG count = GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, nullptr, nullptr, 0)) {
        (void)count;
        std::wstring buffer(64, L'\0');
        ULONG actual = 0;
        ULONG numLanguages = 0;
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &numLanguages, buffer.data(),
                                        &actual) &&
            actual > 0) {
            // 宽字符窄化：语言标签都是 ASCII，直接取低字节。
            std::string tag;
            for (wchar_t wc : buffer) {
                if (wc == L'\0') break;
                if (wc < 128) tag.push_back(static_cast<char>(wc));
            }
            if (auto lang = LanguageFromLocaleTag(tag)) return *lang;
        }
    }
#elif defined(__APPLE__)
    // 3) macOS 首选语言列表。
    CFArrayRef langs = CFLocaleCopyPreferredLanguages();
    if (langs && CFArrayGetCount(langs) > 0) {
        CFStringRef first = static_cast<CFStringRef>(CFArrayGetValueAtIndex(langs, 0));
        char buf[32] = {0};
        if (CFStringGetCString(first, buf, sizeof(buf), kCFStringEncodingUTF8)) {
            if (auto lang = LanguageFromLocaleTag(buf)) {
                CFRelease(langs);
                return *lang;
            }
        }
    }
    if (langs) CFRelease(langs);
#endif

    return Language::Chinese;
}

}  // namespace ppet
