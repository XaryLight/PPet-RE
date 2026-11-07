// PPet - C++ 移植版
// Compiler/ 文案翻译器 —— 命令行入口。
//
// 用法：
//   ppet_i18n_gen --out <生成文件.inl> --struct <I18n.hpp>
//                 --zh <中文文案.json|csv> --en <英文文案.json|csv>
//                 [--check]
//
//   --check 只比对、不写文件：若生成文件与源文案不一致就报错退出（返回码 1），
//   用于 CI 检查「提交的内嵌表是否已经过期」。
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <Compiler/i18n_gen/i18n_gen.hpp>

namespace {

void PrintUsage() {
    std::cerr
        << "用法:\n"
        << "  ppet_i18n_gen --out <out.inl> --struct <I18n.hpp> \\\n"
        << "                --zh <zh.json|csv> --en <en.json|csv> [--check]\n\n"
        << "选项:\n"
        << "  --out     生成的 C++ 内嵌表路径（必填）\n"
        << "  --struct  struct Strings 所在的头文件，用于校验键与成员一一对应（必填）\n"
        << "  --zh      中文文案表（.json 或 .csv，必填）\n"
        << "  --en      英文文案表（.json 或 .csv，必填）\n"
        << "  --check   只比对不写文件；不一致时返回 1\n";
}

/// 取参数值：--key value 或 --key=value。
bool TakeValue(int argc, char** argv, int& i, const char* name, std::string& out) {
    const std::string arg = argv[i];
    const std::string prefix = std::string("--") + name;
    if (arg == prefix) {
        if (i + 1 >= argc) {
            std::cerr << "错误：" << name << " 缺少参数值\n";
            return false;
        }
        out = argv[++i];
        return true;
    }
    if (arg.rfind(prefix + "=", 0) == 0) {
        out = arg.substr(prefix.size() + 1);
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::string outPath;
    std::string structPath;
    std::string zhPath;
    std::string enPath;
    bool checkOnly = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (TakeValue(argc, argv, i, "out", value)) {
            outPath = value;
        } else if (TakeValue(argc, argv, i, "struct", value)) {
            structPath = value;
        } else if (TakeValue(argc, argv, i, "zh", value)) {
            zhPath = value;
        } else if (TakeValue(argc, argv, i, "en", value)) {
            enPath = value;
        } else if (arg == "--check") {
            checkOnly = true;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage();
            return 0;
        } else {
            std::cerr << "错误：无法识别的参数 " << arg << "\n\n";
            PrintUsage();
            return 2;
        }
    }

    if (outPath.empty() || structPath.empty() || zhPath.empty() || enPath.empty()) {
        std::cerr << "错误：--out / --struct / --zh / --en 都是必填项\n\n";
        PrintUsage();
        return 2;
    }

    using namespace ppet::i18n_gen;

    std::string error;
    std::map<std::string, std::string> zh;
    std::map<std::string, std::string> en;
    if (!LoadTable(zhPath, zh, error) || !LoadTable(enPath, en, error)) {
        std::cerr << "错误：" << error << "\n";
        return 1;
    }

    std::vector<std::string> members;
    if (!ParseStructMembers(structPath, members, error)) {
        std::cerr << "错误：" << error << "\n";
        return 1;
    }

    if (!Validate(members, zh, en, error)) {
        std::cerr << "错误：" << error << "\n";
        return 1;
    }

    // 条目顺序 = struct Strings 的成员顺序，保证生成的表可读、可 diff。
    std::vector<Entry> entries;
    entries.reserve(members.size());
    for (const std::string& m : members) {
        Entry e;
        e.key = m;
        e.zh = zh[m];
        e.en = en[m];
        entries.push_back(e);
    }

    const std::string content = BuildInl(entries, zhPath, enPath);

    if (checkOnly) {
        std::string existing;
        if (!ReadFile(outPath, existing, error) || existing != content) {
            std::cerr << "错误：" << outPath
                      << " 与当前文案源不一致，请重新生成（构建时会自动重建）\n";
            return 1;
        }
        std::cout << "文案内嵌表与源文件一致：" << outPath << "\n";
        return 0;
    }

    if (!WriteFile(outPath, content, error)) {
        std::cerr << "错误：" << error << "\n";
        return 1;
    }
    std::cout << "已生成 " << outPath << "（" << entries.size() << " 条文案）\n";
    return 0;
}
