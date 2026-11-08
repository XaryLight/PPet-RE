// PPet - C++ 移植版
// 控制台指令解析与格式化单元测试（纯逻辑，无需图形环境）。
#include <src/tools/app/ConsoleCommand.hpp>

#include <cstdio>
#include <string>
#include <vector>

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

using namespace ppet::console;

void TestTokenize() {
    std::printf("-- 分词 Tokenize --\n");
    std::vector<std::string> out;
    std::string error;
    Check(Tokenize("list", out, error) && out.size() == 1 && out[0] == "list",
          "单个词");
    Check(Tokenize("  select   0  ", out, error) && out.size() == 2 &&
              out[0] == "select" && out[1] == "0",
          "多余空白被忽略");
    Check(Tokenize("add \"/path/with space/model.pmx\"", out, error) &&
              out.size() == 2 && out[1] == "/path/with space/model.pmx",
          "双引号内保留空格");
    Check(Tokenize("speak '你好 世界'", out, error) && out.size() == 2 &&
              out[1] == "你好 世界",
          "单引号内保留空格与中文");
    Check(Tokenize("add \"\"", out, error) && out.size() == 2 && out[1].empty(),
          "空字符串参数");
    Check(!Tokenize("add \"未闭合", out, error) && !error.empty(), "未闭合引号报错");
}

void TestParseCommand() {
    std::printf("-- 指令解析 ParseCommand --\n");
    Command cmd = ParseCommand("SELECT 0");
    Check(cmd.ok && !cmd.empty && cmd.verb == "select", "指令名转小写");
    Check(cmd.args.size() == 1 && cmd.args[0] == "0", "参数解析");
    Check(cmd.tail == "0", "tail 从第一个参数开始");

    cmd = ParseCommand("speak 你好  世界 ！");
    Check(cmd.verb == "speak" && cmd.tail == "你好  世界 ！", "tail 保留原始空格");

    cmd = ParseCommand("action touch_head.motion3.json do");
    Check(cmd.args.size() == 2 && cmd.args[1] == "do", "多参数指令");

    cmd = ParseCommand("   ");
    Check(cmd.empty, "空行");
    cmd = ParseCommand("# 注释");
    Check(cmd.empty, "注释行");
    cmd = ParseCommand("");
    Check(cmd.empty, "空字符串");
}

void TestFormatTable() {
    std::printf("-- 表格格式化 FormatTable --\n");
    const std::string table = FormatTable(
        {"Idx", "Name", "Path"},
        {{"0", "* nengdai_2", "/tmp/nengdai/nengdai_2.model3.json"},
         {"10", "aidang_2", "/tmp/aidang_2.model3.json"}});
    Check(table.find("Idx") == 0, "表头在第一行");
    Check(table.find("Name") != std::string::npos, "表头含 Name");
    Check(table.find("Path") != std::string::npos, "表头含 Path");
    Check(table.find("---") != std::string::npos, "有分隔线");
    Check(table.find("* nengdai_2") != std::string::npos, "当前项标记 * 保留");
    // 第二列宽度应被第一行的 "* nengdai_2"（11 字符）撑开，
    // 因此短行 "aidang_2" 后面至少有 2 个空格分隔。
    Check(table.find("aidang_2   ") != std::string::npos, "列宽对齐填充空格");
    Check(FormatTable({}, {}).empty(), "空表头返回空串");
}

void TestHelp() {
    std::printf("-- 帮助文本 --\n");
    std::string help = HelpText();
    Check(!help.empty(), "帮助非空");
    for (const char* keyword : {"list", "select", "debug", "re", "action", "speak"}) {
        Check(help.find(keyword) != std::string::npos, "帮助含指令说明");
    }
}

}  // namespace

int main() {
    std::printf("=== ConsoleCommand 单元测试 ===\n");
    TestTokenize();
    TestParseCommand();
    TestFormatTable();
    TestHelp();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
