// PPet - C++ 移植版
// Compiler/ 文案翻译器 —— 实现。
#include <Compiler/i18n_gen/i18n_gen.hpp>

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

#include "nlohmann/json.hpp"

namespace ppet {
namespace i18n_gen {
namespace {

using nlohmann::json;

/// 去掉路径的扩展名（含点），用于判断 JSON / CSV。
std::string ExtensionOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return ext;
}

/// 解析 CSV 文本为「行 → 字段」。
///
/// 支持：
///   - 双引号包裹字段，"" 表示字面量双引号；
///   - 引号内可以出现逗号与换行；
///   - UTF-8 BOM 会被自动跳过（Excel 另存为 CSV 时常见）。
std::vector<std::vector<std::string>> ParseCsv(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool inQuotes = false;
    bool fieldStarted = false;

    std::size_t i = 0;
    // 跳过 UTF-8 BOM
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        i = 3;
    }

    auto endField = [&]() {
        row.push_back(field);
        field.clear();
        fieldStarted = false;
    };
    auto endRow = [&]() {
        endField();
        // 跳过完全空白行
        bool blank = true;
        for (const std::string& f : row) {
            if (!f.empty() &&
                f.find_first_not_of(" \t\r\n") != std::string::npos) {
                blank = false;
                break;
            }
        }
        if (!blank) rows.push_back(row);
        row.clear();
    };

    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field.push_back(c);
            }
            continue;
        }
        switch (c) {
            case '"':
                inQuotes = true;
                fieldStarted = true;
                break;
            case ',':
                endField();
                break;
            case '\r':
                break;  // CRLF 里的 CR，交给 LF 处理
            case '\n':
                endRow();
                break;
            default:
                field.push_back(c);
                fieldStarted = true;
                break;
        }
    }
    if (fieldStarted || !field.empty() || !row.empty()) {
        endRow();
    }
    return rows;
}

/// 去掉首尾空白。
std::string Trim(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

bool ReadFile(const std::string& path, std::string& content, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "无法打开文件：" + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    content = ss.str();
    return true;
}

bool WriteFile(const std::string& path, const std::string& content, std::string& error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "无法写入文件：" + path;
        return false;
    }
    out << content;
    if (!out) {
        error = "写入文件时出错：" + path;
        return false;
    }
    return true;
}

bool LoadTable(const std::string& path, std::map<std::string, std::string>& out,
               std::string& error) {
    std::string text;
    if (!ReadFile(path, text, error)) return false;

    const std::string ext = ExtensionOf(path);

    if (ext == "json") {
        json j;
        try {
            j = json::parse(text);
        } catch (const std::exception& e) {
            error = "JSON 解析失败（" + path + "）：" + e.what();
            return false;
        }
        if (!j.is_object()) {
            error = "JSON 顶层必须是对象（" + path + "）";
            return false;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            // 以 _ 开头的键是注释项，不参与校验。
            if (!it.key().empty() && it.key()[0] == '_') continue;
            if (!it.value().is_string()) {
                error = "JSON 条目 " + it.key() + " 的值必须是字符串（" + path + "）";
                return false;
            }
            out[it.key()] = it.value().get<std::string>();
        }
        return true;
    }

    if (ext == "csv") {
        const auto rows = ParseCsv(text);
        if (rows.empty()) {
            error = "CSV 是空的：" + path;
            return false;
        }
        // 首行必须是 key,text 表头。
        const auto& header = rows.front();
        if (header.size() < 2 || Trim(header[0]) != "key" || Trim(header[1]) != "text") {
            error = "CSV 表头必须是 key,text（" + path + "）";
            return false;
        }
        for (std::size_t r = 1; r < rows.size(); ++r) {
            const auto& row = rows[r];
            if (row.size() < 2) {
                error = "CSV 第 " + std::to_string(r + 1) + " 行缺少 text 列（" + path + "）";
                return false;
            }
            const std::string key = Trim(row[0]);
            if (key.empty()) continue;
            if (!key.empty() && key[0] == '_') continue;  // 注释项
            out[key] = row[1];
        }
        return true;
    }

    error = "不支持的文案文件扩展名（只支持 .json / .csv）：" + path;
    return false;
}

bool ParseStructMembers(const std::string& headerPath, std::vector<std::string>& members,
                        std::string& error) {
    std::string text;
    if (!ReadFile(headerPath, text, error)) return false;

    const std::size_t structPos = text.find("struct Strings");
    if (structPos == std::string::npos) {
        error = "在 " + headerPath + " 里找不到 struct Strings";
        return false;
    }
    const std::size_t bodyStart = text.find('{', structPos);
    const std::size_t bodyEnd = text.find("};", bodyStart);
    if (bodyStart == std::string::npos || bodyEnd == std::string::npos) {
        error = "无法定位 struct Strings 的定义范围：" + headerPath;
        return false;
    }
    const std::string body = text.substr(bodyStart + 1, bodyEnd - bodyStart - 1);

    // 成员形如：std::string name;  或  std::string name = "默认值";
    static const std::regex kMember(R"(std::string\s+([A-Za-z_][A-Za-z0-9_]*)\s*(=|;))");
    for (std::sregex_iterator it(body.begin(), body.end(), kMember), end; it != end; ++it) {
        members.push_back((*it)[1].str());
    }
    if (members.empty()) {
        error = "struct Strings 里没有解析到任何 std::string 成员：" + headerPath;
        return false;
    }
    return true;
}

bool Validate(const std::vector<std::string>& members,
              const std::map<std::string, std::string>& zh,
              const std::map<std::string, std::string>& en, std::string& error) {
    std::ostringstream problems;

    // 1) 结构体成员必须有译文。
    for (const std::string& m : members) {
        const auto z = zh.find(m);
        const auto e = en.find(m);
        if (z == zh.end()) {
            problems << "  · 缺少中文文案：" << m << "\n";
        } else if (z->second.empty()) {
            problems << "  · 中文文案为空：" << m << "\n";
        }
        if (e == en.end()) {
            problems << "  · 缺少英文文案：" << m << "\n";
        } else if (e->second.empty()) {
            problems << "  · 英文文案为空：" << m << "\n";
        }
    }

    // 2) 文案表里不能有多余的键（多半是拼错了）。
    for (const auto& kv : zh) {
        if (std::find(members.begin(), members.end(), kv.first) == members.end()) {
            problems << "  · 中文文案里有结构体中不存在的键：" << kv.first
                     << "（请检查拼写，或在 struct Strings 里补上该成员）\n";
        }
    }
    for (const auto& kv : en) {
        if (std::find(members.begin(), members.end(), kv.first) == members.end()) {
            problems << "  · 英文文案里有结构体中不存在的键：" << kv.first
                     << "（请检查拼写，或在 struct Strings 里补上该成员）\n";
        }
    }

    if (!problems.str().empty()) {
        error = "文案表与 struct Strings 不一致：\n" + problems.str();
        return false;
    }
    return true;
}

std::string EscapeCppLiteral(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::string BuildInl(const std::vector<Entry>& entries, const std::string& zhSourcePath,
                     const std::string& enSourcePath) {
    std::ostringstream out;
    out << "// ============================================================================\n";
    out << "// 由 Compiler/i18n_gen 自动生成，请勿手工修改（改动会在下次构建时被覆盖）。\n";
    out << "//   中文源：" << zhSourcePath << "\n";
    out << "//   英文源：" << enSourcePath << "\n";
    out << "//   改文案请改上面的源文件，然后重新构建。\n";
    out << "// ============================================================================\n";
    out << "#ifndef PPET_CORE_I18N_STRINGS_INL\n";
    out << "#define PPET_CORE_I18N_STRINGS_INL\n\n";
    out << "#include <cstddef>\n\n";
    out << "// 一条文案：键 + 简体中文 + 英文。\n";
    out << "// 用 X 宏承载，是为了让下面的 constexpr 表与「给结构体赋值」共用同一份数据。\n";
    out << "#define PPET_I18N_ENTRIES(X) \\\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        out << "    X(" << e.key << ", \"" << EscapeCppLiteral(e.zh) << "\", \""
            << EscapeCppLiteral(e.en) << "\")";
        if (i + 1 < entries.size()) out << " \\";
        out << "\n";
    }
    out << "\n";
    out << "namespace ppet {\n";
    out << "namespace i18n_detail {\n\n";
    out << "/// 文案条目表（编译期常量，不占用运行时内存）。\n";
    out << "struct I18nEntry {\n";
    out << "    const char* key;\n";
    out << "    const char* zh;\n";
    out << "    const char* en;\n";
    out << "};\n\n";
    out << "inline constexpr I18nEntry kI18nTable[] = {\n";
    out << "#define PPET_I18N_ROW(key, zh, en) {#key, zh, en},\n";
    out << "    PPET_I18N_ENTRIES(PPET_I18N_ROW)\n";
    out << "#undef PPET_I18N_ROW\n";
    out << "};\n\n";
    out << "inline constexpr std::size_t kI18nTableSize =\n";
    out << "    sizeof(kI18nTable) / sizeof(kI18nTable[0]);\n\n";
    out << "}  // namespace i18n_detail\n";
    out << "}  // namespace ppet\n\n";
    out << "#endif  // PPET_CORE_I18N_STRINGS_INL\n";
    return out.str();
}

}  // namespace i18n_gen
}  // namespace ppet
