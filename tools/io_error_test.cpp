// PPet - C++ 移植版
// IO 错误诊断（IoError.hpp）单元测试（Q3）：
//   - 常见系统错误码能给出中文原因（权限不足 / 磁盘已满 / 只读 / 路径不存在…）
//   - std::error_code 与 FileIO 的 outError 文本都能格式化出错误码与原因
//   - 任何失败描述都不为空、都含操作 / 路径 / 原因三段
//
// 回归目标：用户「无法创建目录、但 ./ppet 可以」时，日志里必须出现
// 系统错误码与可操作原因，而不是一句无法排查的「无法创建目录」。
//
// 纯头文件、零依赖，三种 CMake 配置均可编译运行。
#include <src/tools/core/IoError.hpp>

#include <cstdio>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <cerrno>
#endif

namespace {

using namespace ppet;

int failures = 0;
int checks = 0;

void Check(bool cond, const char* what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  [FAIL] %s\n", what);
    }
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

#if defined(_WIN32)
constexpr int kPermissionCode = 5;    // ERROR_ACCESS_DENIED
constexpr int kDiskFullCode = 112;    // ERROR_DISK_FULL
constexpr int kPathNotFoundCode = 3;  // ERROR_PATH_NOT_FOUND
#else
constexpr int kPermissionCode = EACCES;
constexpr int kDiskFullCode = ENOSPC;
constexpr int kPathNotFoundCode = ENOENT;
#endif

void TestHints() {
    std::printf("-- 常见错误码的中文原因 --\n");
    Check(Contains(IoErrorHint(kPermissionCode), "权限不足"),
          "权限不足码给出「权限不足」原因");
    Check(Contains(IoErrorHint(kDiskFullCode), "磁盘空间已满"),
          "磁盘满码给出「磁盘空间已满」原因");
    Check(!IoErrorHint(kPathNotFoundCode).empty(), "路径不存在码有说明");
    Check(IoErrorHint(999999).empty(), "未知码返回空串（不胡乱解释）");
}

void TestErrorCodeText() {
    std::printf("-- std::error_code 格式化 --\n");
    const std::error_code ec(kPermissionCode, std::system_category());
    const std::string text = IoErrorText(ec);
    Check(Contains(text, "[" + std::to_string(kPermissionCode) + "]"),
          "含系统错误码 [N]");
    Check(Contains(text, "权限不足"), "含中文原因");
    Check(!text.empty() && text.size() > ec.message().size(), "比原始系统文本更详细");

    const std::string zero = IoErrorText(std::error_code{});
    Check(!zero.empty() && Contains(zero, "未知原因"),
          "ec 为 0 时给明确占位，不返回空串");

    const std::error_code full(kDiskFullCode, std::system_category());
    Check(Contains(IoErrorText(full), "磁盘空间已满"), "磁盘满的中文原因");
}

void TestSysErrorText() {
    std::printf("-- FileIO outError 文本规范化 --\n");
    const std::string empty = IoErrorText(std::string_view{});
    Check(!empty.empty() && Contains(empty, "未知原因"),
          "空系统文本给可操作占位，绝不静默");

    // 模拟 FileIO 的 Windows/POSIX 统一格式：「系统文本（错误码 N）」
    const std::string withCode =
        "Access is denied.（错误码 " + std::to_string(kPermissionCode) + "）";
    const std::string formatted = IoErrorText(std::string_view(withCode));
    Check(Contains(formatted, "Access is denied."), "保留系统原始文本");
    Check(Contains(formatted, "权限不足"), "从「错误码 N」提取并追加中文原因");

    const std::string noCode = IoErrorText(std::string_view("Operation not permitted"));
    Check(Contains(noCode, "Operation not permitted"), "无错误码时原样保留文本");
}

void TestExtractErrorCode() {
    std::printf("-- 错误码提取 --\n");
    using ppet::io_error_detail::ExtractErrorCode;
    Check(ExtractErrorCode("Permission denied（错误码 13）") == 13,
          "从「错误码 N」提取数字");
    Check(ExtractErrorCode("错误码 112）") == 112, "标记在开头也能提取");
    Check(ExtractErrorCode("Permission denied") == -1, "无标记返回 -1");
    Check(ExtractErrorCode("错误码 abc") == -1, "标记后无数字返回 -1");
    Check(ExtractErrorCode("") == -1, "空文本返回 -1");
}

void TestDescribeFailure() {
    std::printf("-- 完整失败描述 --\n");
    const std::string full = DescribeIoFailure(
        "无法创建解压目录", "A:\\Roaming\\PPet\\models",
        IoErrorText(std::error_code(kPermissionCode, std::system_category())));
    Check(Contains(full, "无法创建解压目录"), "含操作");
    Check(Contains(full, "A:\\Roaming\\PPet\\models"), "含路径");
    Check(Contains(full, "权限不足"), "含可操作原因");
    Check(Contains(full, "[" + std::to_string(kPermissionCode) + "]"), "含错误码");

    const std::string noReason = DescribeIoFailure("无法写入文件", "x.bin", "");
    Check(Contains(noReason, "未提供系统原因"), "原因为空时给占位，不静默");

    const std::string noPath = DescribeIoFailure("读取失败", "", "磁盘错误");
    Check(!Contains(noPath, "：") && Contains(noPath, "读取失败"),
          "路径为空时不产生多余冒号");
}

}  // namespace

int main() {
    std::printf("=== IoError 单元测试（Q3）===\n");
    TestHints();
    TestErrorCodeText();
    TestSysErrorText();
    TestExtractErrorCode();
    TestDescribeFailure();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
