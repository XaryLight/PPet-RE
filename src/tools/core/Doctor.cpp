// PPet - 自检模块实现（见 Doctor.hpp）。
#include <src/tools/core/Doctor.hpp>

#include <src/tools/core/Config.hpp>
#include <api/vendor/Json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace ppet {
namespace fs = std::filesystem;

DoctorCheck& DoctorReport::Add(std::string name, DoctorCheck::Status status,
                               std::string message) {
    checks.push_back(DoctorCheck{std::move(name), status, std::move(message)});
    return checks.back();
}

bool DoctorReport::HasFailure() const {
    for (const auto& c : checks) if (c.status == DoctorCheck::Status::Fail) return true;
    return false;
}
bool DoctorReport::HasWarning() const {
    for (const auto& c : checks) if (c.status == DoctorCheck::Status::Warn) return true;
    return false;
}
int DoctorReport::FailCount() const {
    int n = 0;
    for (const auto& c : checks) if (c.status == DoctorCheck::Status::Fail) ++n;
    return n;
}
int DoctorReport::WarnCount() const {
    int n = 0;
    for (const auto& c : checks) if (c.status == DoctorCheck::Status::Warn) ++n;
    return n;
}

namespace {

std::string StatusLabel(DoctorCheck::Status s) {
    switch (s) {
        case DoctorCheck::Status::Ok:   return "[ OK ]";
        case DoctorCheck::Status::Warn: return "[WARN]";
        case DoctorCheck::Status::Fail: return "[FAIL]";
    }
    return "[????]";
}

/// 判断一个模型路径是否需要在本地检查（在线 URL / 空串跳过）。
bool IsLocalPath(const std::string& p) {
    return !p.empty() && p.find("://") == std::string::npos;
}

/// 试探目录是否可写：在其中创建并删除一个临时文件。
bool IsDirWritable(const fs::path& dir, std::string& error) {
    std::error_code ec;
    fs::path probe = dir / "ppet_doctor_write_probe.tmp";
    {
        std::ofstream ofs(probe, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            error = "无法在目录中创建文件";
            return false;
        }
        ofs << "ok";
        if (!ofs.good()) {
            error = "写入失败";
            ofs.close();
            std::error_code rmEc;
            fs::remove(probe, rmEc);
            return false;
        }
    }
    std::error_code rmEc;
    fs::remove(probe, rmEc);
    (void)ec;
    return true;
}

}  // namespace

DoctorReport RunDoctor(const std::string& configPathArg,
                       const std::string& logPathArg,
                       const std::string& backendLabel) {
    DoctorReport report;

    // 1) 构建期后端状态。
    {
        const std::string label = backendLabel.empty() ? "未知" : backendLabel;
        const bool hasBackend =
            label.find("无后端") == std::string::npos && label != "未知";
        report.Add("Live2D 后端",
                   hasBackend ? DoctorCheck::Status::Ok : DoctorCheck::Status::Warn,
                   "当前构建：" + label +
                       (hasBackend ? "（可加载 Live2D 模型）"
                                   : "（Live2D 模型无法加载，如需请使用含 SDK/桩的构建）"));
    }

    // 2) 配置文件解析与关键字段范围。
    const std::string configPath =
        configPathArg.empty() ? ConfigStore::DefaultConfigPath() : configPathArg;
    ConfigStore store;
    std::string cfgError;
    const bool exists = fs::exists(configPath);
    const bool loaded = store.Load(configPath, cfgError);
    if (!exists && loaded) {
        report.Add("配置文件", DoctorCheck::Status::Warn,
                   "尚不存在（将使用默认配置）：" + configPath);
    } else if (!loaded) {
        report.Add("配置文件", DoctorCheck::Status::Fail,
                   "解析失败：" + cfgError + "（建议备份后重置该文件）");
    } else {
        const auto& cfg = store.config();
        std::string detail = "解析成功：" + configPath +
                             "（" + std::to_string(cfg.modelList.size()) + " 个模型）";
        DoctorCheck::Status st = DoctorCheck::Status::Ok;

        // ConfigStore::Load 会把越界值静默钳制到默认范围，所以从 cfg 读不到原始问题。
        // 这里直接读原始 JSON，把「被自动修正」的字段提示给用户（体检的价值正在于此）。
        // 范围与 Config.cpp 的合法性校验保持一致。
        try {
            std::ifstream ifs(configPath);
            ppet::api::Json raw;
            ifs >> raw;
            auto warnIfOutOfRange = [&](const char* key, bool bad) {
                if (bad) {
                    detail += std::string("；") + key + " 越界已被自动修正";
                    st = DoctorCheck::Status::Warn;
                }
            };
            const auto num = [&](const char* key, double fallback) -> double {
                if (!raw.is_object() || !raw.contains(key) || !raw[key].is_number()) {
                    return fallback;
                }
                return raw[key].get<double>();
            };
            warnIfOutOfRange("renderScale",
                             num("renderScale", 1.0) < 0.25 || num("renderScale", 1.0) > 4.0);
            warnIfOutOfRange("trackingRadius",
                             num("trackingRadius", 400.0) < 50.0 ||
                                 num("trackingRadius", 400.0) > 5000.0);
            warnIfOutOfRange("maxTrackingAngle",
                             num("maxTrackingAngle", 30.0) < 0.0 ||
                                 num("maxTrackingAngle", 30.0) > 90.0);
            warnIfOutOfRange("maxFps",
                             num("maxFps", 60.0) < 0.0 || num("maxFps", 60.0) > 240.0);
        } catch (const std::exception&) {
            // 解析异常已在上面的 Load 里判为 Fail；这里仅跳过原始值检查。
        }

        report.Add("配置文件", st, detail);
    }

    // 3) 模型文件存在性（当前模型 + 列表）。
    {
        std::vector<std::string> paths;
        if (!store.config().modelPath.empty()) paths.push_back(store.config().modelPath);
        for (const auto& p : store.config().modelList) {
            if (std::find(paths.begin(), paths.end(), p) == paths.end()) paths.push_back(p);
        }
        int missing = 0;
        std::string firstMissing;
        int checked = 0;
        for (const auto& p : paths) {
            if (!IsLocalPath(p)) continue;  // 在线 URL 不检查
            ++checked;
            std::error_code ec;
            if (!fs::exists(p, ec) || ec) {
                ++missing;
                if (firstMissing.empty()) firstMissing = p;
            }
        }
        if (missing == 0) {
            report.Add("模型文件", DoctorCheck::Status::Ok,
                       "已检查 " + std::to_string(checked) + " 个本地模型路径，均存在");
        } else {
            report.Add("模型文件", DoctorCheck::Status::Warn,
                       std::to_string(missing) + " 个本地模型路径不存在，首个：" +
                           firstMissing + "（请重新导入或在设置中移除）");
        }
    }

    // 4) 日志目录可写。
    {
        const std::string logPath = logPathArg.empty() ? "last.log" : logPathArg;
        fs::path logFile(logPath);
        fs::path dir = logFile.parent_path();
        if (dir.empty()) dir = fs::path(".");
        std::error_code ec;
        const bool dirExists = fs::exists(dir, ec) && !ec;
        if (!dirExists) {
            report.Add("日志目录", DoctorCheck::Status::Warn,
                       "目录不存在：" + dir.string() + "（启动时将尝试创建）");
        } else {
            std::string err;
            if (IsDirWritable(dir, err)) {
                report.Add("日志目录", DoctorCheck::Status::Ok,
                           "可写：" + dir.string());
            } else {
                report.Add("日志目录", DoctorCheck::Status::Fail,
                           "不可写：" + dir.string() + "（" + err + "）");
            }
        }
    }

    return report;
}

std::string FormatDoctorReport(const DoctorReport& report) {
    std::string out = "PPet 自检报告\n";
    out += "========================================\n";
    for (const auto& c : report.checks) {
        out += StatusLabel(c.status) + " " + c.name + "：" + c.message + "\n";
    }
    out += "----------------------------------------\n";
    out += "结果：" + std::to_string(report.FailCount()) + " 项失败 / " +
           std::to_string(report.WarnCount()) + " 项警告 / " +
           std::to_string(static_cast<int>(report.checks.size()) -
                          report.FailCount() - report.WarnCount()) + " 项正常\n";
    return out;
}

}  // namespace ppet
