// PPet - C++ 移植版
// IModel 能力查询与统一口型入口的接口契约单测（报告 2.3.4）：
//   - 默认能力位为 false，后端按实际支持 override
//   - SetMouth 统一路由：原生 LipSync 走 SetLipSync，其余映射到元音 weights
//   - 工厂对已注册类型返回实例、对 Unknown 返回 nullptr（注册表模式）
// 纯逻辑，零 GUI/GL 依赖。
#include <src/tools/model/Model.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

using ppet::IModel;
using ppet::ModelDebugInfo;
using ppet::ModelType;

// 最小假后端：验证基类默认能力与 SetMouth 路由。
class FakeModel : public IModel {
public:
    enum class Kind { Plain, Vowel, Native } kind = Kind::Plain;

    bool Load(const std::string&, std::string&) override { return true; }
    void Update(const ppet::RenderContext&) override {}
    void Render(const ppet::RenderContext&) override {}
    void OnResize(int, int) override {}
    ModelType Type() const override { return ModelType::Unknown; }
    bool IsLoaded() const override { return true; }
    std::string DisplayName() const override { return "Fake"; }

    bool SupportsLipSync() const override { return kind == Kind::Native; }
    bool SupportsExpressions() const override {
        return kind == Kind::Vowel || kind == Kind::Native;
    }

    bool SetLipSync(float open, float form) override {
        if (kind != Kind::Native) return false;
        lastLipOpen = open;
        lastLipForm = form;
        ++lipCalls;
        return true;
    }
    bool SetExpressionWeights(
        const std::vector<std::pair<std::string, float>>& weights) override {
        if (kind != Kind::Vowel) return false;
        lastWeights = weights;
        ++weightCalls;
        return true;
    }

    int lipCalls = 0;
    int weightCalls = 0;
    float lastLipOpen = -1.0f;
    float lastLipForm = -1.0f;
    std::vector<std::pair<std::string, float>> lastWeights;
};

}  // namespace

int main() {
    std::printf("== IModel 能力查询与统一口型入口 ==\n");

    // 1) 默认能力位：空后端全部 false。
    {
        FakeModel m;
        Check(!m.SupportsExpressions(), "默认 SupportsExpressions=false");
        Check(!m.SupportsLipSync(), "默认 SupportsLipSync=false");
        Check(!m.SupportsMotion(), "默认 SupportsMotion=false");
    }

    // 2) SetMouth 路由到原生 LipSync。
    {
        FakeModel m;
        m.kind = FakeModel::Kind::Native;
        Check(m.SetMouth(0.7f, 0.4f), "原生后端 SetMouth 返回 true");
        Check(m.lipCalls == 1 && m.weightCalls == 0, "原生后端走 SetLipSync");
        Check(std::fabs(m.lastLipOpen - 0.7f) < 1e-4f &&
                  std::fabs(m.lastLipForm - 0.4f) < 1e-4f,
              "SetMouth 透传 open/form 到 SetLipSync");
    }

    // 3) SetMouth 路由到元音 weights（PMX/VRM 型）。
    {
        FakeModel m;
        m.kind = FakeModel::Kind::Vowel;
        Check(m.SetMouth(0.6f, 0.5f), "元音后端 SetMouth 返回 true");
        Check(m.weightCalls == 1 && m.lipCalls == 0, "元音后端走 SetExpressionWeights");
        Check(m.lastWeights.size() == 2 && m.lastWeights[0].first == "aa" &&
                  m.lastWeights[1].first == "ih",
              "open/form 映射为 aa/ih 两个元音通道");
        Check(std::fabs(m.lastWeights[0].second - 0.6f) < 1e-4f,
              "aa 权重等于开口度");
    }

    // 4) SetMouth 输入钳制到 [0,1]。
    {
        FakeModel m;
        m.kind = FakeModel::Kind::Native;
        m.SetMouth(2.0f, -3.0f);
        Check(std::fabs(m.lastLipOpen - 1.0f) < 1e-4f &&
                  std::fabs(m.lastLipForm - 0.0f) < 1e-4f,
              "SetMouth 把越界 open/form 钳制到 [0,1]");
    }

    // 5) 工厂注册表：已注册类型返回实例，Unknown 返回 nullptr。
    {
        auto pmx = ppet::CreateModel(ModelType::Pmx);
        Check(pmx != nullptr, "工厂创建 PMX 实例");
        auto vrm = ppet::CreateModel(ModelType::Vrm);
        Check(vrm != nullptr, "工厂创建 VRM 实例");
        auto unknown = ppet::CreateModel(ModelType::Unknown);
        Check(unknown == nullptr, "工厂对 Unknown 返回 nullptr");
    }

    std::printf("== 结果：%s（失败 %d 项）==\n",
                g_failures == 0 ? "全部通过" : "存在失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
