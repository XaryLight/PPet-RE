// PPet - C++ 移植版
// Live2D manifest / motion 解析层单元测试。
//
// 背景：本次为 Live2D 补齐「待机 + 互动动作」，manifest 需要新增解析
// model3.json 的 HitAreas（点击命中区），motion 需要解析 Meta 的
// FadeInTime / FadeOutTime（动作淡入淡出）。这两个是动作调度的输入数据源，
// 必须证明「JSON 里写了 → 解析结果里真的有」，否则调度器无米下锅。
//
// 本测试只依赖 FileIO + nlohmann/json（均为 header-only / 无 GL 依赖），
// 因此三种 CMake 配置（真实 SDK / OFF / STUB）都能编译运行。
#include <cstdio>
#include <filesystem>
#include <string>

#include <src/tools/core/FileIO.hpp>
#include <api/live2d/Live2DManifest.hpp>
#include <api/live2d/Live2DMotion.hpp>

namespace {

namespace fs = std::filesystem;

int failures = 0;
int checks = 0;

void Check(bool cond, const char* what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  [FAIL] %s\n", what);
    }
}

using namespace ppet;
using namespace ppet::live2d;

/// 造一个最小但合法的 Live2D 模型目录：
///   model.model3.json（Moc/Textures/Motions/HitAreas）
///   model.moc3 / tex.png（占位文件，manifest 要求存在）
///   idle.motion3.json / tap_head.motion3.json（含 Fade 元数据）
std::string MakeModelDir() {
    const fs::path dir = fs::temp_directory_path() / "ppet_l2d_manifest_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    WriteFileText((dir / "model.moc3").string(), "MOC3");
    WriteFileText((dir / "tex.png").string(), "PNG");

    const char* model3 = R"({
      "Version": 3,
      "Name": "TestPet",
      "FileReferences": {
        "Moc": "model.moc3",
        "Textures": ["tex.png"],
        "Motions": {
          "Idle": [
            { "File": "idle.motion3.json" },
            { "File": "idle_plain.motion3.json", "FadeInTime": 0.4, "FadeOutTime": 0.6 }
          ],
          "TapHead": [{ "File": "tap_head.motion3.json" }]
        }
      },
      "HitAreas": [
        { "Id": "HitAreaHead", "Name": "head" },
        { "Id": "HitAreaBody", "Name": "body" }
      ]
    })";
    WriteFileText((dir / "model.model3.json").string(), model3);

    const char* idleMotion = R"({
      "Version": 3,
      "Meta": { "Duration": 3.0, "Fps": 30.0, "FadeInTime": 0.5, "FadeOutTime": 0.8 },
      "Curves": [
        { "Target": "Parameter", "Id": "ParamAngleX",
          "Curve": [ { "Time": 0.0, "Value": 0.0 }, { "Time": 3.0, "Value": 10.0 } ] }
      ]
    })";
    WriteFileText((dir / "idle.motion3.json").string(), idleMotion);

    // 这个动作文件不写 Fade，fade 应由 model3.json 的组条目提供（回退路径）。
    const char* plainIdle = R"({
      "Version": 3,
      "Meta": { "Duration": 2.0, "Fps": 30.0 },
      "Curves": [
        { "Target": "Parameter", "Id": "ParamAngleZ",
          "Curve": [ { "Time": 0.0, "Value": 0.0 }, { "Time": 2.0, "Value": 3.0 } ] }
      ]
    })";
    WriteFileText((dir / "idle_plain.motion3.json").string(), plainIdle);

    const char* tapMotion = R"({
      "Version": 3,
      "Meta": { "Duration": 1.5, "Fps": 30.0, "FadeInTime": 0.2, "FadeOutTime": 0.3 },
      "Curves": [
        { "Target": "Parameter", "Id": "ParamAngleY",
          "Curve": [ { "Time": 0.0, "Value": 0.0 }, { "Time": 1.5, "Value": -5.0 } ] }
      ]
    })";
    WriteFileText((dir / "tap_head.motion3.json").string(), tapMotion);

    return dir.string();
}

void TestManifestHitAreas() {
    std::printf("-- model3.json: Motions 组名 + HitAreas 解析 --\n");
    const std::string dir = MakeModelDir();
    const std::string model3 = dir + "/model.model3.json";

    Live2DManifest m;
    std::string error;
    Check(ParseModel3Json(model3, m, error), "解析 model3.json 成功");
    if (!error.empty()) std::printf("    (error=%s)\n", error.c_str());

    Check(m.displayName == "TestPet", "显示名取自 Name 字段");
    Check(m.motionGroups.size() == 2, "动作组数=2 (Idle / TapHead)");
    bool hasIdle = false, hasTap = false;
    for (const auto& g : m.motionGroups) {
        if (g.name == "Idle") {
            hasIdle = true;
            // 关键回归：标准写法是数组，旧实现把它解析成 0 个动作。
            Check(g.files.size() == 2, "Idle 组（标准数组写法）2 个动作");
            Check(g.fadeInTimes.size() == 2 && g.fadeOutTimes.size() == 2,
                  "组级 fade 数组与 files 一一对应");
            if (g.fadeInTimes.size() == 2) {
                Check(g.fadeInTimes[0] < 0.0f, "动作[0] 未指定组级 fade(-1)");
                Check(g.fadeInTimes[1] > 0.39f && g.fadeInTimes[1] < 0.41f,
                      "动作[1] 组级 FadeInTime=0.4");
                Check(g.fadeOutTimes[1] > 0.59f && g.fadeOutTimes[1] < 0.61f,
                      "动作[1] 组级 FadeOutTime=0.6");
            }
        }
        if (g.name == "TapHead") {
            hasTap = true;
            Check(g.files.size() == 1, "TapHead 组 1 个动作");
        }
    }
    Check(hasIdle, "保留 Idle 组名（旧实现会丢组名）");
    Check(hasTap, "保留 TapHead 组名");

    Check(m.hitAreas.size() == 2, "HitAreas 解析出 2 个命中区");
    if (m.hitAreas.size() == 2) {
        Check(m.hitAreas[0].id == "HitAreaHead", "命中区[0].Id");
        Check(m.hitAreas[0].name == "head", "命中区[0].Name=head");
        Check(m.hitAreas[1].id == "HitAreaBody", "命中区[1].Id");
        Check(m.hitAreas[1].name == "body", "命中区[1].Name=body");
    }
}

void TestMotionFade() {
    std::printf("-- motion3.json: FadeInTime / FadeOutTime 解析 --\n");
    const std::string dir = MakeModelDir();

    Motion idle;
    std::string error;
    Check(ParseMotionFile(dir + "/idle.motion3.json", idle, error), "解析 Idle 动作成功");
    Check(idle.duration > 2.99f && idle.duration < 3.01f, "Duration=3.0");
    Check(idle.fadeInTime > 0.49f && idle.fadeInTime < 0.51f, "FadeInTime=0.5");
    Check(idle.fadeOutTime > 0.79f && idle.fadeOutTime < 0.81f, "FadeOutTime=0.8");
    Check(idle.curves.size() == 1, "1 条参数曲线");
    {
        std::vector<float> v;
        idle.Sample(1.5f, v);
        Check(v.size() == 1 && v[0] > 4.0f && v[0] < 6.0f, "曲线采样 t=1.5 -> ≈5.0");
    }

    Motion tap;
    Check(ParseMotionFile(dir + "/tap_head.motion3.json", tap, error), "解析 TapHead 动作成功");
    Check(tap.fadeInTime > 0.19f && tap.fadeInTime < 0.21f, "FadeInTime=0.2");
    Check(tap.fadeOutTime > 0.29f && tap.fadeOutTime < 0.31f, "FadeOutTime=0.3");
}

void TestNoFadeKeepsZero() {
    std::printf("-- 无 Fade 字段时保持 0（旧模型兼容）--\n");
    const std::string dir = MakeModelDir();
    const char* plain = R"({
      "Version": 3,
      "Meta": { "Duration": 2.0, "Fps": 30.0 },
      "Curves": [
        { "Target": "Parameter", "Id": "ParamA",
          "Curve": [ { "Time": 0.0, "Value": 1.0 } ] }
      ]
    })";
    const std::string path = dir + "/plain.motion3.json";
    WriteFileText(path, plain);
    Motion mo;
    std::string error;
    Check(ParseMotionFile(path, mo, error), "解析无 Fade 动作成功");
    Check(mo.fadeInTime == 0.0f, "缺省 FadeInTime=0");
    Check(mo.fadeOutTime == 0.0f, "缺省 FadeOutTime=0");
}

void TestNoHitAreas() {
    std::printf("-- 无 HitAreas 时不崩溃（分区回退路径）--\n");
    const std::string dir = MakeModelDir();
    const char* json = R"({
      "Version": 3,
      "FileReferences": { "Moc": "model.moc3", "Textures": ["tex.png"] }
    })";
    const std::string path = dir + "/nohit.model3.json";
    WriteFileText(path, json);
    Live2DManifest m;
    std::string error;
    Check(ParseModel3Json(path, m, error), "无 HitAreas 也能解析");
    Check(m.hitAreas.empty(), "命中区列表为空");
    Check(m.motionGroups.empty(), "无动作组");
}

}  // namespace

int main() {
    std::printf("=== Live2D manifest/motion 解析单元测试 ===\n");
    TestManifestHitAreas();
    TestMotionFade();
    TestNoFadeKeepsZero();
    TestNoHitAreas();
    std::printf("=== %d 项检查，失败 %d ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
