// PPet - C++ 移植版
// 结构化模型加载错误（ModelError.hpp）单元测试。
//
// 覆盖：分类名/建议完整性、真实错误串的分类推断、明细与建议组装、
// 兼容旧代码的单行摘要、多行报告、状态复位。
#include <cstdio>
#include <string>

#include <src/tools/model/ModelError.hpp>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
}

}  // namespace

int main() {
    using ppet::ModelErrorCategory;
    using ppet::ModelLoadError;

    std::printf("== 分类名与建议 ==\n");
    for (int i = 0; i <= static_cast<int>(ModelErrorCategory::InternalError); ++i) {
        const auto c = static_cast<ModelErrorCategory>(i);
        Check(ppet::ModelErrorCategoryName(c)[0] != '\0', "分类 " + std::to_string(i) + " 有名称");
        Check(ppet::ModelErrorCategoryAdvice(c)[0] != '\0', "分类 " + std::to_string(i) + " 有建议");
    }

    std::printf("== 真实错误串的分类推断 ==\n");
    struct Case {
        const char* raw;
        ModelErrorCategory expected;
        const char* label;
    };
    const Case cases[] = {
        {"Live2D 模型需要编译 Cubism SDK 后端（CMake 选项 PPET_WITH_CUBISM）",
         ModelErrorCategory::BackendUnavailable, "后端未启用"},
        {"压缩包内没有找到模型文件（支持 .model3.json / .pmx / .vrm）",
         ModelErrorCategory::PackageError, "ZIP 包内无模型"},
        {"未找到有效骨架：骨骼数为 0", ModelErrorCategory::NoValidSkeleton, "无有效骨架"},
        {"模型配置文件没有列出任何纹理", ModelErrorCategory::MissingTexture, "缺贴图"},
        {"moc 文件解析失败（文件可能损坏或不是有效的 Cubism 模型）：a.moc3",
         ModelErrorCategory::MocError, "moc3 损坏"},
        {"无法识别的模型格式（支持 .zip / .pmx / .vrm）",
         ModelErrorCategory::UnsupportedFormat, "格式不支持"},
        {"模型配置文件不存在：/tmp/a.model3.json",
         ModelErrorCategory::FileNotFound, "文件不存在"},
        {"面索引数量不是 3 的倍数（文件可能已损坏）",
         ModelErrorCategory::ParseError, "索引解析错误"},
        {"JSON 解析失败：unexpected token", ModelErrorCategory::ParseError, "JSON 解析错误"},
        {"accessor 的 bufferView 索引越界", ModelErrorCategory::ParseError, "glTF 越界"},
        {"内存不足，无法创建 Live2D 模型", ModelErrorCategory::InternalError, "内存不足"},
    };
    for (const Case& c : cases) {
        const auto got = ppet::InferErrorCategory(c.raw);
        Check(got == c.expected,
              std::string(c.label) + " -> " + ppet::ModelErrorCategoryName(got));
    }

    std::printf("== 结构化组装 ==\n");
    ModelLoadError e;
    Check(e.ok(), "默认构造无错误");
    Check(e.ToString().empty(), "无错误时单行摘要为空");

    e.Fail(ModelErrorCategory::MissingTexture, "缺少 3 张贴图", "/tmp/m.pmx");
    e.details.push_back("/tmp/m/tex/body.png");
    e.details.push_back("/tmp/m/tex/face.png");
    Check(!e.ok(), "Fail 后为失败状态");
    Check(e.ToString().find("[模型图片缺失]") != std::string::npos,
          "单行摘要带分类标签");
    Check(e.ToString().find("/tmp/m.pmx") != std::string::npos, "单行摘要带路径");

    const std::string report = e.ToReport();
    Check(report.find("分类：模型图片缺失") != std::string::npos, "报告含分类");
    Check(report.find("[1/2] /tmp/m/tex/body.png") != std::string::npos, "报告含编号明细");
    Check(report.find("打包成 .zip") != std::string::npos, "报告含可操作建议");

    e.advice = "自定义建议";
    Check(e.EffectiveAdvice() == "自定义建议", "自定义建议优先");
    e.Clear();
    Check(e.ok() && e.details.empty() && e.advice.empty(), "Clear 复位全部字段");

    std::printf("== FromRaw 兜底 ==\n");
    const ModelLoadError raw = ModelLoadError::FromRaw("贴图文件不存在：a.png", "a.pmx");
    Check(raw.category == ModelErrorCategory::MissingTexture, "FromRaw 推断分类");
    Check(raw.path == "a.pmx", "FromRaw 保留路径");
    const ModelLoadError empty = ModelLoadError::FromRaw("", "x");
    Check(empty.ok(), "空错误串不算失败");

    std::printf("\nmodel_error_test: %d 项检查，%d 项失败\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
