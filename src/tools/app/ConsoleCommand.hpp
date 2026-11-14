// PPet - C++ 移植版
// 控制台指令：解析与格式化（纯逻辑，零依赖，可单测）。
//
// 【为什么单独一层】
//   主程序把「读 stdin」放在后台线程、「执行命令」放在渲染主线程，
//   中间通过队列传递。解析与表格格式化不碰任何 App / Qt / GL 状态，
//   因此可以脱离图形环境做单元测试，也便于脚本化批量验证。
#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace ppet::console {

/// 一条已解析的指令。
struct Command {
    std::string verb;                 ///< 小写指令名
    std::vector<std::string> args;    ///< 分词后的参数（引号内的空格会保留）
    std::string tail;                 ///< 第一个参数之后的原始剩余文本（供 speak 保留空格）
    std::string raw;                  ///< 原始输入行
    bool empty = false;               ///< 空行 / 纯注释
    bool ok = true;
    std::string error;                ///< 解析失败原因（如引号未闭合）
};

/// 分词：按空白切分，支持 "..." 与 '...' 引号；反斜杠转义空格。
/// @return 是否解析成功（false 表示引号未闭合，error 会被写入）。
inline bool Tokenize(const std::string& line, std::vector<std::string>& out,
                     std::string& error) {
    out.clear();
    error.clear();
    std::string cur;
    char quote = 0;
    bool hasToken = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else if (c == '\\' && i + 1 < line.size()) {
                cur.push_back(line[++i]);
            } else {
                cur.push_back(c);
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            hasToken = true;   // 允许空字符串参数 ""
            continue;
        }
        if (c == '\\' && i + 1 < line.size() && std::isspace(static_cast<unsigned char>(line[i + 1]))) {
            cur.push_back(line[++i]);
            hasToken = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (hasToken) {
                out.push_back(cur);
                cur.clear();
                hasToken = false;
            }
            continue;
        }
        cur.push_back(c);
        hasToken = true;
    }
    if (quote != 0) {
        error = "引号未闭合";
        return false;
    }
    if (hasToken) out.push_back(cur);
    return true;
}

/// 解析一行输入。
inline Command ParseCommand(const std::string& line) {
    Command cmd;
    cmd.raw = line;
    std::vector<std::string> tokens;
    if (!Tokenize(line, tokens, cmd.error)) {
        cmd.ok = false;
        return cmd;
    }
    if (tokens.empty() || (!tokens[0].empty() && tokens[0][0] == '#')) {
        cmd.empty = true;
        return cmd;
    }
    cmd.verb = tokens[0];
    std::transform(cmd.verb.begin(), cmd.verb.end(), cmd.verb.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    cmd.args.assign(tokens.begin() + 1, tokens.end());

    // tail：从第一个参数在原始行里的位置开始，保留后续所有空白与引号内容。
    // 用「跳过 verb 后第一个非空白字符」定位，而不是简单拼接 args，
    // 因为 speak 需要保留用户输入的原始空格。
    std::size_t pos = line.find_first_not_of(" \t\r\n");
    if (pos != std::string::npos) {
        pos = line.find_first_of(" \t", pos);
        if (pos != std::string::npos) {
            pos = line.find_first_not_of(" \t", pos);
            if (pos != std::string::npos) cmd.tail = line.substr(pos);
        }
    }
    return cmd;
}

/// 等宽列对齐的表格。用户示例里的 `Idx Name Path` 表头就靠它排版。
/// 每列宽度取该列所有单元格的最大显示长度（按 UTF-8 字节近似，中文场景
/// 由调用方自行补充空格；这里不做 wcwidth，避免引入 ICU 依赖）。
inline std::string FormatTable(const std::vector<std::string>& headers,
                               const std::vector<std::vector<std::string>>& rows) {
    if (headers.empty()) return std::string();
    const std::size_t cols = headers.size();
    std::vector<std::size_t> widths(headers.size());
    for (std::size_t c = 0; c < cols; ++c) widths[c] = headers[c].size();
    for (const auto& row : rows) {
        for (std::size_t c = 0; c < cols && c < row.size(); ++c) {
            widths[c] = std::max(widths[c], row[c].size());
        }
    }
    auto formatRow = [&](const std::vector<std::string>& cells) {
        std::ostringstream os;
        for (std::size_t c = 0; c < cols; ++c) {
            const std::string value = c < cells.size() ? cells[c] : std::string();
            os << value;
            if (c + 1 < cols) {
                os << std::string(widths[c] - value.size() + 2, ' ');
            }
        }
        return os.str();
    };
    std::string out = formatRow(headers);
    out += "\n";
    for (std::size_t i = 0; i < cols; ++i) {
        out += std::string(widths[i], '-');
        if (i + 1 < cols) out += "  ";
    }
    for (const auto& row : rows) {
        out += "\n" + formatRow(row);
    }
    return out;
}

/// 解析动作目标：同时支持「按名字」与「按数字索引（idx）」调试动画。
/// - token 为纯数字且在 [0, names.size()) 内：返回对应 names[idx]，ok=true（命中索引）。
/// - token 为纯数字但越界：返回 {"", false}（调用方应提示可用范围）。
/// - 其它情况：原样返回 token，ok=true（交给 PlayMotion 按名字 / 模糊匹配）。
/// 索引与 `action list` 展示的 Idx 列一一对应（同一份 MotionNames() 快照）。
inline std::pair<std::string, bool> ResolveMotionTarget(
    const std::string& token, const std::vector<std::string>& names) {
    if (!token.empty()) {
        bool allDigit = true;
        for (char c : token) {
            if (!std::isdigit(static_cast<unsigned char>(c))) { allDigit = false; break; }
        }
        if (allDigit) {
            const int idx = std::atoi(token.c_str());
            if (idx >= 0 && static_cast<std::size_t>(idx) < names.size()) {
                return {names[static_cast<std::size_t>(idx)], true};
            }
            return {"", false};
        }
    }
    return {token, true};
}

/// 帮助文本（控制台 help 指令的输出）。
inline const char* HelpText() {
    return
        "PPet 控制台指令（输入 help 查看本表，quit 退出）\n"
        "  list                         列出模型（Idx Name Path，* 为当前）\n"
        "  add <path> [name]            添加并加载模型；默认名=文件名去后缀，重名自动加序号\n"
        "  rename <idx> [name]          改名；不写名字则恢复默认名\n"
        "  select <idx>                 切换到第 idx 个模型\n"
        "  remove <idx>                 从列表移除第 idx 个模型\n"
        "  debug [section]              输出模型分析（status|res|params|bones|parts|\n"
        "                               motion|expression|error|all，缺省 = all）\n"
        "  re [model|draw|all]          重载模型 / 重建渲染资源 / 两者\n"
        "  action list                  列出可播放的动作（含 Idx 索引）\n"
        "  action <name|idx> do|stop    按名字或索引播放 / 停止动作\n"
        "  action stop                  停止当前动作\n"
        "  expression list              列出表情预设\n"
        "  expression <id>              切换表情\n"
        "  speak <text>                 显示气泡文字并同步口型\n"
        "  status                       运行时状态（帧率 / 对话 / 追踪）\n"
        "  help                         本帮助\n"
        "  quit                         退出程序\n"
        "说明：参数可加引号，例如 add \"/path/with space/model.pmx\"。";
}

}  // namespace ppet::console
