// PPet - 自检模块单测（报告 4.2.3）。
// 覆盖：后端标签透传、配置文件存在/损坏/字段越界、模型路径检查、
//       日志目录可写、报告统计与格式化。全部用临时目录，不碰真实配置。
#include <src/tools/core/Doctor.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

namespace fs = std::filesystem;

/// 按检查项名字找到状态。
ppet::DoctorCheck::Status StatusOf(const ppet::DoctorReport& r, const std::string& name) {
    for (const auto& c : r.checks) if (c.name == name) return c.status;
    return ppet::DoctorCheck::Status::Fail;  // 找不到当成失败，便于暴露问题
}

}  // namespace

int main() {
    using namespace ppet;
    std::printf("== 自检模块单测 ==\n");

    const fs::path tmp = fs::temp_directory_path() / "ppet_doctor_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    // --- 1) 后端标签透传（无后端应 Warn） ---
    {
        DoctorReport r = RunDoctor((tmp / "none.json").string(),
                                   (tmp / "last.log").string(), "无后端");
        Check(StatusOf(r, "Live2D 后端") == DoctorCheck::Status::Warn,
              "无后端标签报告 Warn");
        Check(!FormatDoctorReport(r).empty(), "格式化报告非空");
    }

    // --- 2) 配置文件不存在 => Warn（用默认配置），不致命 ---
    {
        const std::string missing = (tmp / "missing.json").string();
        DoctorReport r = RunDoctor(missing, (tmp / "last.log").string(), "内置 Cubism Core 桩");
        Check(StatusOf(r, "配置文件") == DoctorCheck::Status::Warn,
              "配置文件不存在报告 Warn");
        Check(StatusOf(r, "Live2D 后端") == DoctorCheck::Status::Ok,
              "桩后端报告 Ok");
    }

    // --- 3) 合法配置 + 存在的模型 => 全 Ok ---
    {
        const fs::path model = tmp / "sample.pmx";
        { std::ofstream m(model, std::ios::binary); m << "PMX"; }
        const fs::path cfg = tmp / "good.json";
        {
            std::ofstream c(cfg);
            c << R"({"modelList":[")" << model.string() << R"("],"renderScale":1.0})";
        }
        DoctorReport r = RunDoctor(cfg.string(), (tmp / "last.log").string(), "真实 Cubism SDK");
        Check(StatusOf(r, "配置文件") == DoctorCheck::Status::Ok, "合法配置报告 Ok");
        Check(StatusOf(r, "模型文件") == DoctorCheck::Status::Ok, "存在的模型报告 Ok");
        Check(!r.HasFailure() && !r.HasWarning(), "健康配置无失败无警告");
    }

    // --- 4) 损坏配置 => Fail ---
    {
        const fs::path cfg = tmp / "broken.json";
        { std::ofstream c(cfg); c << "{not valid json"; }
        DoctorReport r = RunDoctor(cfg.string(), (tmp / "last.log").string(), "真实 Cubism SDK");
        Check(StatusOf(r, "配置文件") == DoctorCheck::Status::Fail, "损坏配置报告 Fail");
        Check(r.HasFailure() && r.FailCount() >= 1, "损坏配置计入失败数");
    }

    // --- 5) 字段越界 => Warn ---
    {
        const fs::path cfg = tmp / "oob.json";
        { std::ofstream c(cfg); c << R"({"renderScale":99.0,"maxTrackingAngle":200.0})"; }
        DoctorReport r = RunDoctor(cfg.string(), (tmp / "last.log").string(), "真实 Cubism SDK");
        Check(StatusOf(r, "配置文件") == DoctorCheck::Status::Warn, "越界字段报告 Warn");
        Check(r.HasWarning() && r.WarnCount() >= 1, "越界字段计入警告数");
    }

    // --- 6) 缺失模型 => Warn ---
    {
        const fs::path cfg = tmp / "missing_model.json";
        { std::ofstream c(cfg); c << R"({"modelList":["/no/such/pet.pmx"]})"; }
        DoctorReport r = RunDoctor(cfg.string(), (tmp / "last.log").string(), "真实 Cubism SDK");
        Check(StatusOf(r, "模型文件") == DoctorCheck::Status::Warn, "缺失模型报告 Warn");
    }

    fs::remove_all(tmp, ec);

    std::printf("\n结果：%d 项失败\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
