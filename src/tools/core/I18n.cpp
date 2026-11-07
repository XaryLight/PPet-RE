// PPet - C++ 移植版
// 国际化文案实现。
//
// 【文案本体不在这个文件里】
//   中英文文案都放在 Compiler/i18n/{zh-CN,en-US}.json，
//   由 Compiler/i18n_gen（文案翻译器）在构建时翻译成 core/I18nStrings.inl：
//   一个 constexpr 表 + 一个 X 宏。改文案只需要改 JSON，不用再手抄两份。
//
//   生成器同时会校验「文案键 ↔ struct Strings 成员」一一对应，
//   少一条、多一条、拼错一个键都会在构建时立刻报错。
#include <src/tools/core/I18n.hpp>

#include <src/tools/core/I18nStrings.inl>

namespace ppet {

Strings GetStrings(Language lang) {
    Strings s;
    const bool chinese = (lang == Language::Chinese);

// 表与赋值共用同一份 X 宏数据，因此不会出现「表里有、界面却没赋值」的不一致。
#define PPET_I18N_ASSIGN(key, zh, en) s.key = chinese ? zh : en;
    PPET_I18N_ENTRIES(PPET_I18N_ASSIGN)
#undef PPET_I18N_ASSIGN

    return s;
}

}  // namespace ppet
