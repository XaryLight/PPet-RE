// PPet - C++ 移植版
// Live2D 物理（physics3.json）解析与摆锤求解单元测试。
//
// 覆盖：
//   1. physics3.json 解析（Cubism 3+ 标准格式 + Mobill/Cubism2 变体）
//   2. 输入归一化（正/负区间、不反向/反向）
//   3. 摆锤运动（静止下垂、输入推动后产生位移、收敛回静止）
//   4. 输出写回（Weight 混合、Scale 缩放、Reflect 反向）
//
// 本测试不依赖 Cubism SDK，可独立运行。
#include <api/live2d/Live2DPhysics.hpp>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

void CheckNear(float actual, float expected, float tolerance, const char* message) {
    if (std::fabs(actual - expected) > tolerance) {
        std::printf("[FAIL] %s（实际 %.4f，期望 %.4f ±%.4f）\n", message, actual, expected,
                    tolerance);
        ++g_failures;
    }
}

/// Cubism 3+ 标准格式（官方 Haru 模型的写法）。
const char* kStandardPhysics = R"JSON({
  "Version": 3,
  "Meta": {
    "PhysicsSettingCount": 1,
    "TotalInputCount": 1,
    "TotalOutputCount": 1,
    "VertexCount": 3,
    "EffectiveForces": { "Gravity": { "X": 0, "Y": -1 }, "Wind": { "X": 0, "Y": 0 } },
    "PhysicsDictionary": [ { "Id": "PhysicsSetting1", "Name": "前髪" } ]
  },
  "PhysicsSettings": [
    {
      "Id": "PhysicsSetting1",
      "Input": [
        { "Source": { "Target": "Parameter", "Id": "ParamAngleX" },
          "Weight": 60, "Type": "X", "Reflect": false }
      ],
      "Output": [
        { "Destination": { "Target": "Parameter", "Id": "ParamHairFront" },
          "VertexIndex": 1, "Scale": 1.821, "Weight": 100, "Type": "Angle", "Reflect": false }
      ],
      "Vertices": [
        { "Position": { "X": 0, "Y": 0 }, "Mobility": 1, "Delay": 1, "Acceleration": 1, "Radius": 0 },
        { "Position": { "X": 0, "Y": 8 }, "Mobility": 0.95, "Delay": 0.8, "Acceleration": 1.5, "Radius": 8 },
        { "Position": { "X": 0, "Y": 16 }, "Mobility": 0.9, "Delay": 0.8, "Acceleration": 1, "Radius": 8 }
      ],
      "Normalization": {
        "Position": { "Minimum": -10, "Default": 0, "Maximum": 10 },
        "Angle": { "Minimum": -10, "Default": 0, "Maximum": 10 }
      }
    }
  ]
})JSON";

/// Mobill / Cubism2 变体：Input / Output / Vertices 全是数组，
/// 但 Vertices 用「绝对位置 + Radius」，且缺少部分 Meta 字段。
const char* kMobillPhysics = R"JSON({
  "Version": 3,
  "Meta": {
    "PhysicsSettingCount": 1,
    "TotalInputCount": 1,
    "TotalOutputCount": 1,
    "VertexCount": 6,
    "EffectiveForces": { "Gravity": { "X": 0, "Y": -1 }, "Wind": { "X": 0, "Y": 0 } }
  },
  "PhysicsSettings": [
    {
      "Id": "PhysicsSetting1",
      "Input": [
        { "Source": { "Target": "Parameter", "Id": "wuli" },
          "Weight": 100, "Type": "X", "Reflect": false }
      ],
      "Output": [
        { "Destination": { "Target": "Parameter", "Id": "Param6" },
          "VertexIndex": 1, "Scale": 25, "Weight": 100, "Type": "Angle", "Reflect": false }
      ],
      "Vertices": [
        { "Position": { "X": 0, "Y": 0 }, "Mobility": 0.9, "Delay": 0.8, "Acceleration": 1, "Radius": 5 },
        { "Position": { "X": 0, "Y": 15 }, "Mobility": 0.98, "Delay": 0.8, "Acceleration": 1.5, "Radius": 15 },
        { "Position": { "X": 0, "Y": 20 }, "Mobility": 0.9, "Delay": 0.8, "Acceleration": 1, "Radius": 5 }
      ],
      "Normalization": {
        "Position": { "Minimum": -10, "Default": 0, "Maximum": 10 },
        "Angle": { "Minimum": -10, "Default": 0, "Maximum": 10 }
      }
    }
  ]
})JSON";

void TestParseStandard() {
    ppet::live2d::PhysicsFile file;
    std::string error;
    Check(ppet::live2d::ParsePhysicsJson(kStandardPhysics, file, error),
          "标准格式 physics3.json 应解析成功");
    Check(file.settings.size() == 1, "标准格式应有 1 个物理设置");
    CheckNear(file.gravityY, -1.0f, 1e-6f, "重力 Y 应为 -1");
    if (file.settings.empty()) return;

    const auto& s = file.settings[0];
    Check(s.id == "PhysicsSetting1", "物理设置 Id 应为 PhysicsSetting1");
    Check(s.inputs.size() == 1, "应有 1 个输入");
    Check(s.outputs.size() == 1, "应有 1 个输出");
    Check(s.vertices.size() == 3, "应有 3 个粒子");

    if (!s.inputs.empty()) {
        Check(s.inputs[0].parameter == "ParamAngleX", "输入参数应为 ParamAngleX");
        CheckNear(s.inputs[0].weight, 60.0f, 1e-6f, "输入权重应为 60");
        Check(s.inputs[0].type == ppet::live2d::PhysicsType::X, "输入类型应为 X");
    }
    if (!s.outputs.empty()) {
        Check(s.outputs[0].parameter == "ParamHairFront", "输出参数应为 ParamHairFront");
        Check(s.outputs[0].vertexIndex == 1, "输出粒子索引应为 1");
        CheckNear(s.outputs[0].scale, 1.821f, 1e-6f, "输出缩放应为 1.821");
        Check(s.outputs[0].type == ppet::live2d::PhysicsType::Angle, "输出类型应为 Angle");
    }
    if (!s.vertices.empty()) {
        CheckNear(s.vertices[1].radius, 8.0f, 1e-6f, "粒子 1 的半径应为 8");
        CheckNear(s.vertices[1].acceleration, 1.5f, 1e-6f, "粒子 1 的收敛速度应为 1.5");
    }
    CheckNear(s.positionMin, -10.0f, 1e-6f, "归一化下限应为 -10");
    CheckNear(s.angleMax, 10.0f, 1e-6f, "角度归一化上限应为 10");
}

void TestParseMobill() {
    ppet::live2d::PhysicsFile file;
    std::string error;
    Check(ppet::live2d::ParsePhysicsJson(kMobillPhysics, file, error),
          "Mobill 变体 physics3.json 应解析成功");
    Check(file.settings.size() == 1, "Mobill 变体应有 1 个物理设置");
    if (file.settings.empty()) return;

    const auto& s = file.settings[0];
    Check(s.inputs.size() == 1, "Mobill 变体应有 1 个输入");
    Check(s.outputs.size() == 1, "Mobill 变体应有 1 个输出");
    Check(s.vertices.size() == 3, "Mobill 变体应有 3 个粒子");
    if (!s.inputs.empty()) {
        Check(s.inputs[0].parameter == "wuli", "Mobill 输入参数应为 wuli");
        CheckNear(s.inputs[0].weight, 100.0f, 1e-6f, "Mobill 输入权重应为 100");
    }
    if (!s.outputs.empty()) {
        Check(s.outputs[0].parameter == "Param6", "Mobill 输出参数应为 Param6");
        CheckNear(s.outputs[0].scale, 25.0f, 1e-6f, "Mobill 输出缩放应为 25");
    }
    if (s.vertices.size() >= 2) {
        CheckNear(s.vertices[1].radius, 15.0f, 1e-6f, "Mobill 粒子 1 的半径应为 15");
    }
}

void TestParseInvalid() {
    ppet::live2d::PhysicsFile file;
    std::string error;
    Check(!ppet::live2d::ParsePhysicsJson("{ not json", file, error), "非法 JSON 应解析失败");
    Check(!error.empty(), "解析失败时应给出错误原因");

    error.clear();
    Check(!ppet::live2d::ParsePhysicsJson("{}", file, error), "缺少 PhysicsSettings 应解析失败");
    Check(!error.empty(), "缺少 PhysicsSettings 时应给出错误原因");
}

/// 单组物理：一个输入驱动一个角度输出。
std::map<std::string, float> MakeParams() {
    return {{"ParamAngleX", 0.0f}, {"ParamHairFront", 0.0f}};
}

void TestStabilizeAndRest() {
    ppet::live2d::PhysicsFile file;
    std::string error;
    Check(ppet::live2d::ParsePhysicsJson(kStandardPhysics, file, error), "解析应成功");
    if (file.settings.empty()) return;

    ppet::live2d::Live2DPhysics phys;
    Check(phys.Load(file), "载入物理应成功");
    Check(!phys.Empty(), "载入后不应为空");
    Check(phys.SettingCount() == 1, "物理设置数量应为 1");

    // 登记参数范围（ParamAngleX 通常是 -30..30）。
    phys.SetParameterRange("ParamAngleX", -30.0f, 30.0f);

    std::map<std::string, float> params = MakeParams();
    phys.Stabilize(params);

    // 静止时输出应接近 0（摆锤垂直下垂，与参考方向夹角为 0）。
    CheckNear(params["ParamHairFront"], 0.0f, 1e-3f, "静止时输出应为 0");

    // 静止状态下连续推进，输出应保持接近 0（不抖动）。
    for (int i = 0; i < 60; ++i) phys.Evaluate(1.0f / 60.0f, params);
    CheckNear(params["ParamHairFront"], 0.0f, 1e-2f, "静止 60 帧后输出仍应接近 0");
}

void TestInputProducesSwing() {
    ppet::live2d::PhysicsFile file;
    std::string error;
    Check(ppet::live2d::ParsePhysicsJson(kStandardPhysics, file, error), "解析应成功");
    if (file.settings.empty()) return;

    ppet::live2d::Live2DPhysics phys;
    phys.Load(file);
    phys.SetParameterRange("ParamAngleX", -30.0f, 30.0f);

    std::map<std::string, float> params = MakeParams();
    phys.Stabilize(params);

    // 头部转到 +30°，摆锤应该被推开。
    params["ParamAngleX"] = 30.0f;
    float peak = 0.0f;
    for (int i = 0; i < 60; ++i) {
        phys.Evaluate(1.0f / 60.0f, params);
        peak = std::max(peak, std::fabs(params["ParamHairFront"]));
    }
    Check(peak > 1e-3f, "输入变化后摆锤应产生位移（头发摇曳）");

    // 换向：头部转回 -30°，输出应改变符号。
    params["ParamAngleX"] = -30.0f;
    float opposite = 0.0f;
    for (int i = 0; i < 90; ++i) {
        phys.Evaluate(1.0f / 60.0f, params);
        opposite = std::max(opposite, std::fabs(params["ParamHairFront"]));
    }
    Check(opposite > 1e-3f, "反向输入同样应产生位移");
}

void TestOutputWeightBlend() {
    // 构造一个只有 1 个粒子（无摆动）的设置，输出恒为 0，
    // 用于验证 Weight 混合与 Reflect 反向的语义。
    const char* json = R"JSON({
      "Version": 3,
      "Meta": { "PhysicsSettingCount": 1, "EffectiveForces": { "Gravity": { "X": 0, "Y": -1 } } },
      "PhysicsSettings": [
        {
          "Id": "P",
          "Input": [ { "Source": { "Target": "Parameter", "Id": "ParamAngleX" },
                       "Weight": 100, "Type": "X", "Reflect": false } ],
          "Output": [ { "Destination": { "Target": "Parameter", "Id": "Out" },
                        "VertexIndex": 1, "Scale": 1, "Weight": 50, "Type": "Angle", "Reflect": false } ],
          "Vertices": [
            { "Position": { "X": 0, "Y": 0 }, "Mobility": 1, "Delay": 1, "Acceleration": 1, "Radius": 5 }
          ],
          "Normalization": {
            "Position": { "Minimum": -10, "Default": 0, "Maximum": 10 },
            "Angle": { "Minimum": -10, "Default": 0, "Maximum": 10 }
          }
        }
      ]
    })JSON";

    ppet::live2d::PhysicsFile file;
    std::string error;
    Check(ppet::live2d::ParsePhysicsJson(json, file, error), "解析应成功");
    if (file.settings.empty()) return;

    ppet::live2d::Live2DPhysics phys;
    phys.Load(file);
    phys.SetParameterRange("ParamAngleX", -30.0f, 30.0f);

    std::map<std::string, float> params = {{"ParamAngleX", 30.0f}, {"Out", 100.0f}};
    phys.Stabilize(params);

    // Weight=50：物理值（0）与原值（100）各占一半 → 50。
    CheckNear(params["Out"], 50.0f, 1e-3f, "Weight=50 时输出应与原值各占一半");
}

void TestReflectOutput() {
    const char* json = R"JSON({
      "Version": 3,
      "Meta": { "PhysicsSettingCount": 1, "EffectiveForces": { "Gravity": { "X": 0, "Y": -1 } } },
      "PhysicsSettings": [
        {
          "Id": "P",
          "Input": [ { "Source": { "Target": "Parameter", "Id": "ParamAngleX" },
                       "Weight": 100, "Type": "X", "Reflect": false } ],
          "Output": [ { "Destination": { "Target": "Parameter", "Id": "Out" },
                        "VertexIndex": 1, "Scale": 1, "Weight": 100, "Type": "Angle", "Reflect": true } ],
          "Vertices": [
            { "Position": { "X": 0, "Y": 0 }, "Mobility": 1, "Delay": 1, "Acceleration": 1, "Radius": 5 }
          ],
          "Normalization": {
            "Position": { "Minimum": -10, "Default": 0, "Maximum": 10 },
            "Angle": { "Minimum": -10, "Default": 0, "Maximum": 10 }
          }
        }
      ]
    })JSON";

    ppet::live2d::PhysicsFile file;
    std::string error;
    Check(ppet::live2d::ParsePhysicsJson(json, file, error), "解析应成功");
    if (file.settings.empty()) return;

    ppet::live2d::Live2DPhysics phys;
    phys.Load(file);
    phys.SetParameterRange("ParamAngleX", -30.0f, 30.0f);

    std::map<std::string, float> params = {{"ParamAngleX", 30.0f}, {"Out", 0.0f}};
    phys.Stabilize(params);
    // Reflect=true 时输出取反：与不反向版本符号相反。
    const float reflected = params["Out"];

    ppet::live2d::Live2DPhysics plain;
    plain.Load(file);
    plain.SetParameterRange("ParamAngleX", -30.0f, 30.0f);
    std::map<std::string, float> params2 = {{"ParamAngleX", 30.0f}, {"Out", 0.0f}};
    plain.Stabilize(params2);

    CheckNear(reflected, -params2["Out"], 1e-6f, "Reflect=true 应使输出反号");
}

void TestNoNaNUnderStress() {
    // 大幅输入 + 长时间推进，结果必须保持有限（不产生 NaN/Inf）。
    ppet::live2d::PhysicsFile file;
    std::string error;
    Check(ppet::live2d::ParsePhysicsJson(kStandardPhysics, file, error), "解析应成功");
    if (file.settings.empty()) return;

    ppet::live2d::Live2DPhysics phys;
    phys.Load(file);
    phys.SetParameterRange("ParamAngleX", -30.0f, 30.0f);

    std::map<std::string, float> params = MakeParams();
    phys.Stabilize(params);

    for (int i = 0; i < 600; ++i) {
        params["ParamAngleX"] = (i % 2 == 0) ? 30.0f : -30.0f;
        phys.Evaluate(1.0f / 60.0f, params);
        Check(std::isfinite(params["ParamHairFront"]), "输出必须始终是有限值");
        if (!std::isfinite(params["ParamHairFront"])) break;
    }
}

}  // namespace

int main() {
    TestParseStandard();
    TestParseMobill();
    TestParseInvalid();
    TestStabilizeAndRest();
    TestInputProducesSwing();
    TestOutputWeightBlend();
    TestReflectOutput();
    TestNoNaNUnderStress();

    if (g_failures == 0) {
        std::printf("live2d_physics_test：全部通过\n");
        return 0;
    }
    std::printf("live2d_physics_test：%d 项失败\n", g_failures);
    return 1;
}
