// PPet - 自检模块（报告 4.2.3，--doctor 命令行）
//
// 目标：一条命令给出「体检报告」，覆盖用户最常踩的四类问题：
//   1. 配置文件能否解析、关键字段是否在合法范围（P0 配置崩溃的历史痛点）；
//   2. 当前模型及模型列表里的本地文件是否仍然存在、可读（白模/丢图的高频原因）；
//   3. Cubism 后端状态（真实 SDK / 内置桩 / 无后端），解释 Live2D 模型能否加载；
//   4. 日志目录是否可写（IO 出错的历史痛点，见 IoError.hpp）。
//
// 设计为纯 C++（只依赖标准库 + Config），不碰 GUI / OpenGL 上下文，
// 因此能在无显示的 CI / 服务器环境里运行，也能被单测直接调用。
#pragma once

#include <string>
#include <vector>

namespace ppet {

/// 单项检查结果。
struct DoctorCheck {
    enum class Status { Ok, Warn, Fail };
    std::string name;     ///< 检查项名称（如「配置文件」）
    Status status = Status::Ok;
    std::string message;  ///< 人可读的结果/建议
};

/// 体检报告。
struct DoctorReport {
    std::vector<DoctorCheck> checks;

    /// 添加一项，返回引用便于链式设置。
    DoctorCheck& Add(std::string name, DoctorCheck::Status status, std::string message);

    bool HasFailure() const;   ///< 是否存在 Fail
    bool HasWarning() const;   ///< 是否存在 Warn
    int FailCount() const;
    int WarnCount() const;
};

/// 运行体检。
///   configPath   ：要检查的配置文件路径（传空串用 ConfigStore::DefaultConfigPath）。
///   logPath      ：要试探写权限的日志路径（传空串用 "last.log"）。
///   backendLabel ：构建期的后端标签，由调用方按编译宏给出（见 main.cpp），
///                  这样 Doctor 本身无需 include Cubism / GLFW 头。
DoctorReport RunDoctor(const std::string& configPath,
                       const std::string& logPath,
                       const std::string& backendLabel);

/// 把报告渲染成一段面向终端的文本（UTF-8）。
/// 逐行形如：  [ OK ] 配置文件：解析成功（3 个模型）
///            [WARN] 模型文件：/path/x.pmx 不存在
///            [FAIL] 日志目录：不可写（Permission denied）
std::string FormatDoctorReport(const DoctorReport& report);

}  // namespace ppet
