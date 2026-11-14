// PPet - C++ 移植版
// 配置落盘单测：重点回归「%APPDATA% 指向不可写盘（A: 软驱 / 重定向卷）时
// 配置写不出去、且只报含糊的『无法写入配置文件』」这一类故障。
//
// 覆盖：
//   1) DefaultConfigPath 返回的目录一定是可写的（本次修复的核心保证）；
//   2) Save/Load 往返字段不丢；
//   3) 写入失败时不再吞掉 GetLastError / errno，错误信息带系统原因；
//   4) ExecutableDir 可取得且不含尾部分隔符（便携回退用）。
#include <src/tools/core/Config.hpp>
#include <src/tools/core/FileIO.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

void CheckEq(const std::string& actual, const std::string& expected, const char* what) {
    if (actual != expected) {
        std::printf("[FAIL] %s（实际 \"%s\"，期望 \"%s\"）\n", what, actual.c_str(),
                    expected.c_str());
        ++g_failures;
    } else {
        std::printf("[ OK ] %s\n", what);
    }
}

namespace fs = std::filesystem;

}  // namespace

int main() {
    using namespace ppet;
    std::printf("== 配置落盘单测 ==\n");

    // --- 1) DefaultConfigPath 必须指向可写目录 ---
    {
        const std::string cfgPath = ConfigStore::DefaultConfigPath();
        Check(!cfgPath.empty(), "DefaultConfigPath 非空");

        // 实测其父目录可建、可写：这是本次修复的核心不变量。
        const fs::path parent = fs::path(cfgPath).parent_path();
        std::error_code ec;
        fs::create_directories(parent, ec);
        Check(!ec, "DefaultConfigPath 的父目录可创建");
        const std::string probe = cfgPath + ".writetest";
        Check(WriteFileText(probe, "ok"), "DefaultConfigPath 所在目录可写入");
        fs::remove(fs::path(probe), ec);
    }

    // --- 2) Save / Load 往返 ---
    {
        const std::string dir = "./ppet_config_test_tmp";
        std::error_code ec;
        fs::create_directories(fs::path(dir), ec);
        const std::string path = JoinPath(dir, "config.json");

        ConfigStore store;
        store.config().language = Language::English;
        store.config().maxFps = 30;
        store.config().fullscreenTracking = false;
        store.config().maxTrackingAngle = 45.0f;
        std::string err;
        Check(store.Save(path, err), "Save 成功");
        Check(err.empty(), "Save 无错误串");

        ConfigStore loaded;
        Check(loaded.Load(path, err), "Load 成功");
        Check(loaded.config().language == Language::English, "language 已持久化");
        Check(loaded.config().maxFps == 30, "maxFps 已持久化");
        Check(loaded.config().fullscreenTracking == false, "fullscreenTracking 已持久化");
        Check(loaded.config().maxTrackingAngle == 45.0f, "maxTrackingAngle 已持久化");

        // 首次运行（文件不存在）应返回默认配置且不报错。
        ConfigStore fresh;
        Check(fresh.Load(JoinPath(dir, "does_not_exist.json"), err), "缺文件 Load 视为默认");
        Check(err.empty(), "缺文件 Load 无错误");

        fs::remove(fs::path(path), ec);
        fs::remove(fs::path(dir), ec);
    }

    // --- 3) 写入失败必须给出系统级原因（不再吞掉 GetLastError / errno）---
    {
        std::string sysErr;
        // 父目录不存在 → CreateFileW / fopen 失败，应带系统错误文本。
        const bool ok = WriteFileText("/no_such_dir_ppet_xyz/sub/config.json", "x", &sysErr);
        Check(!ok, "父目录不存在时 WriteFileText 失败");
        Check(!sysErr.empty(), "失败时给出系统错误原因（不再吞掉 GetLastError）");

        std::string emptyErr;
        Check(!WriteFileText("", "x", &emptyErr), "空路径写入失败");
        CheckEq(emptyErr, "路径为空", "空路径错误信息明确");

        // Save 失败时错误信息应包含系统原因与『目录创建』说明。
        const std::string dir = "./ppet_config_test_bad";
        std::error_code ec;
        fs::create_directories(fs::path(dir), ec);
        const std::string fileParent = dir + "/afile";
        Check(WriteFileText(fileParent, "x"), "创建占位文件");
        // 父路径是已存在的普通文件 → create_directories 失败。
        const std::string badPath = fileParent + "/config.json";
        ConfigStore s2;
        std::string serr;
        Check(!s2.Save(badPath, serr), "父为文件时 Save 失败");
        Check(serr.find("无法创建配置目录") != std::string::npos,
              "Save 失败说明是目录创建问题");
        Check(serr.find("（") != std::string::npos, "Save 失败带系统原因（括号括起）");

        fs::remove(fs::path(fileParent), ec);
        fs::remove(fs::path(dir), ec);
    }

    // --- 4) ExecutableDir 可取得且不含尾部分隔符 ---
    {
        const std::string exeDir = ExecutableDir();
        Check(!exeDir.empty(), "ExecutableDir 非空");
        Check(exeDir.back() != '/' && exeDir.back() != '\\',
              "ExecutableDir 无尾部斜杠");
    }

    std::printf(g_failures == 0 ? "\n全部通过\n" : "\n存在 %d 处失败\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
