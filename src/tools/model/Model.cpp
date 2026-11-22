// PPet - C++ 移植版
// 模型工厂：按类型创建后端实例，并识别模型文件路径。
#include <src/tools/model/Model.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include <src/tools/model/pmx/PmxModel.hpp>
#include <src/tools/model/vrm/VrmModel.hpp>

#if defined(PPET_WITH_CUBISM)
#include <api/live2d/Live2DModel.hpp>
#endif

namespace ppet {
namespace {

/// 取路径的小写扩展名（含点）。
std::string LowerExtension(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string name =
        (slash == std::string::npos) ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

}  // namespace

ModelType DetectModelType(const std::string& path) {
    const std::string ext = LowerExtension(path);

    if (ext == ".pmx") return ModelType::Pmx;
    if (ext == ".vrm") return ModelType::Vrm;

    // Live2D：Cubism 3 起用 .model3.json，v2 用 .model.json。
    // 这里按「扩展名是 .json」且（文件名含 model3 / model，或路径含 live2d）来判断。
    if (ext == ".json") {
        const std::string lower = [&] {
            std::string s = path;
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }();
#if defined(__cpp_lib_string_contains)
        if (lower.contains(".model3.json") || lower.contains(".model.json")) {
            return ModelType::Live2D;
        }
#else
        if (lower.find(".model3.json") != std::string::npos ||
            lower.find(".model.json") != std::string::npos) {
            return ModelType::Live2D;
        }
#endif
        // 其它 .json 交由 Live2D 后端尝试解析（其加载器会校验字段）。
        return ModelType::Live2D;
    }

    return ModelType::Unknown;
}

namespace {

/// 【2.3.4】工厂注册表：新增后端只需注册一个 creator，无需改 switch（开闭原则）。
/// 用函数内静态 map，避免跨 TU 初始化顺序问题。
using ModelCreator = ModelPtr (*)();
std::unordered_map<ModelType, ModelCreator>& Registry() {
    static std::unordered_map<ModelType, ModelCreator> registry = [] {
        std::unordered_map<ModelType, ModelCreator> r;
        r[ModelType::Pmx] = []() -> ModelPtr { return std::make_shared<pmx::PmxModel>(); };
        r[ModelType::Vrm] = []() -> ModelPtr { return std::make_shared<vrm::VrmModel>(); };
#if defined(PPET_WITH_CUBISM)
        r[ModelType::Live2D] = []() -> ModelPtr {
            return std::make_shared<live2d::Live2DModel>();
        };
#else
        // 未编译 Cubism SDK 时不注册 Live2D 后端，CreateModel 返回 nullptr。
#endif
        return r;
    }();
    return registry;
}

}  // namespace

ModelPtr CreateModel(ModelType type) {
    const auto& registry = Registry();
    const auto it = registry.find(type);
    if (it == registry.end()) return nullptr;
    return it->second();
}

}  // namespace ppet
