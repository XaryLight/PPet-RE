// PPet - C++ 移植版
// Compiler/ 文案翻译器 —— 单元测试。
//
// 覆盖：CSV/JSON 读取、结构体成员解析、键与成员的一致性校验、
//       C++ 字面量转义、.inl 生成。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <Compiler/i18n_gen/i18n_gen.hpp>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) {
        std::printf("  [通过] %s\n", what);
    } else {
        std::printf("  [失败] %s\n", what);
        ++g_failures;
    }
}

/// 在临时目录里写一个文件，返回路径。
std::string WriteTemp(const fs::path& dir, const char* name, const std::string& content) {
    const fs::path p = dir / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return p.string();
}

}  // namespace

int main() {
    using namespace ppet::i18n_gen;

    const fs::path tmp = fs::temp_directory_path() / "ppet_i18n_gen_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::printf("== 文案翻译器测试 ==\n");

    // --- 1. CSV 读取：引号、逗号、"" 转义、注释行、注释列 ---
    {
        const std::string csv =
            "key,text\n"
            "greeting,你好\n"
            "withComma,\"模型文件 (*.json *.pmx)\"\n"
            "withQuote,\"他说\"\"你好\"\"\"\n"
            "_comment,这是一条注释\n";
        const std::string path = WriteTemp(tmp, "t.csv", csv);
        std::map<std::string, std::string> table;
        std::string error;
        const bool ok = LoadTable(path, table, error);
        Check(ok, "CSV 读取成功");
        Check(table.count("greeting") == 1 && table["greeting"] == "你好", "普通字段解析正确");
        Check(table.count("withComma") == 1 && table["withComma"] == "模型文件 (*.json *.pmx)",
              "带逗号的引号字段解析正确");
        Check(table.count("withQuote") == 1 && table["withQuote"] == "他说\"你好\"",
              "双引号转义解析正确");
        Check(table.count("_comment") == 0, "以 _ 开头的注释键被忽略");
    }

    // --- 2. JSON 读取 ---
    {
        const std::string js =
            "{\n"
            "  \"_comment\": \"注释\",\n"
            "  \"quit\": \"退出\"\n"
            "}\n";
        const std::string path = WriteTemp(tmp, "t.json", js);
        std::map<std::string, std::string> table;
        std::string error;
        const bool ok = LoadTable(path, table, error);
        Check(ok, "JSON 读取成功");
        Check(table.size() == 1 && table["quit"] == "退出", "JSON 字段解析正确");
    }

    // --- 3. 结构体成员解析 ---
    {
        const std::string hdr =
            "#pragma once\n"
            "struct Strings {\n"
            "    // 注释行\n"
            "    std::string alwaysOnTop;\n"
            "    std::string quit = \"退出\";\n"
            "};\n";
        const std::string path = WriteTemp(tmp, "T.hpp", hdr);
        std::vector<std::string> members;
        std::string error;
        const bool ok = ParseStructMembers(path, members, error);
        Check(ok, "结构体成员解析成功");
        Check(members.size() == 2, "解析出 2 个成员");
        Check(members.size() == 2 && members[0] == "alwaysOnTop" && members[1] == "quit",
              "成员名与顺序正确");
    }

    // --- 4. 一致性校验 ---
    {
        const std::vector<std::string> members{"a", "b"};
        std::map<std::string, std::string> zh{{"a", "甲"}, {"b", "乙"}};
        std::map<std::string, std::string> en{{"a", "A"}, {"b", "B"}};
        std::string error;
        Check(Validate(members, zh, en, error), "键与成员一一对应时校验通过");

        std::map<std::string, std::string> zhMissing = zh;
        zhMissing.erase("b");
        Check(!Validate(members, zhMissing, en, error), "缺译文时报错");

        std::map<std::string, std::string> zhExtra = zh;
        zhExtra["typo"] = "错";
        Check(!Validate(members, zhExtra, en, error), "多余键时报错");

        std::map<std::string, std::string> zhEmpty = zh;
        zhEmpty["a"] = "";
        Check(!Validate(members, zhEmpty, en, error), "空译文时报错");
    }

    // --- 5. C++ 字面量转义 ---
    {
        Check(EscapeCppLiteral("a\"b") == "a\\\"b", "双引号被转义");
        Check(EscapeCppLiteral("a\\b") == "a\\\\b", "反斜杠被转义");
        Check(EscapeCppLiteral("a\nb") == "a\\nb", "换行被转义");
        Check(EscapeCppLiteral("中文") == "中文", "非 ASCII 字符原样保留");
    }

    // --- 6. .inl 生成 ---
    {
        std::vector<Entry> entries;
        entries.push_back(Entry{"quit", "退出", "Quit"});
        entries.push_back(Entry{"about", "关于\n全部", "About\nAll"});
        const std::string inl = BuildInl(entries, "zh-CN.json", "en-US.json");
        Check(inl.find("X(quit, \"退出\", \"Quit\")") != std::string::npos, "X 宏包含条目");
        Check(inl.find("inline constexpr I18nEntry kI18nTable[]") != std::string::npos,
              "生成 constexpr 表");
        Check(inl.find("\"关于\\n全部\"") != std::string::npos, "换行在生成的字面量里被转义");
        Check(inl.find("PPET_I18N_ENTRIES(PPET_I18N_ROW)") != std::string::npos,
              "constexpr 表由同一个 X 宏展开");

        const std::string path = WriteTemp(tmp, "out.inl", inl);
        std::string error;
        Check(WriteFile(path, inl, error), "生成文件写入成功");
        std::string back;
        Check(ReadFile(path, back, error) && back == inl, "写回内容一致");
    }

    fs::remove_all(tmp);

    if (g_failures == 0) {
        std::printf("全部通过\n");
        return 0;
    }
    std::printf("%d 项失败\n", g_failures);
    return 1;
}
