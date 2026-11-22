// PPet - C++ 移植版
// 控制台格式化纯函数模块的实现（详见 ConsoleFormat.hpp）。
#include <src/tools/app/ConsoleFormat.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

#include <src/tools/core/ModelNaming.hpp>

namespace ppet {
namespace console_fmt {

std::string ConsoleBaseName(const std::string& path) {
    if (path.empty()) return "(未加载)";
    const std::string name = model_naming::FileStem(path);
    return name.empty() ? path : name;
}

std::string ModelNameAt(const AppConfig& cfg, std::size_t index) {
    return model_naming::NameAt(cfg.modelList, cfg.modelNames, index);
}

int FindModelIndex(const AppConfig& cfg, const std::string& path) {
    for (std::size_t i = 0; i < cfg.modelList.size(); ++i) {
        if (cfg.modelList[i] == path) return static_cast<int>(i);
    }
    return -1;
}

std::string CurrentModelName(const AppConfig& cfg) {
    const int idx = FindModelIndex(cfg, cfg.modelPath);
    return idx >= 0 ? ModelNameAt(cfg, static_cast<std::size_t>(idx))
                    : ConsoleBaseName(cfg.modelPath);
}

bool IsConsoleFailure(const std::string& result) {
    if (result.rfind("fail:", 0) == 0 || result.rfind("用法:", 0) == 0 ||
        result.rfind("未知指令:", 0) == 0) {
        return true;
    }
    return result.find("\nfail:") != std::string::npos ||
           result.find("\n用法:") != std::string::npos;
}

const char* TypeName(ModelType type) {
    switch (type) {
        case ModelType::Pmx: return "PMX";
        case ModelType::Vrm: return "VRM";
        case ModelType::Live2D: return "Live2D";
        case ModelType::Unknown:
        default: return "Unknown";
    }
}

bool ParseIndex(const std::string& text, int& out) {
    if (text.empty()) return false;
    try {
        std::size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used != text.size()) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

std::string Join(const std::vector<std::string>& items, const std::string& sep) {
    std::ostringstream os;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) os << sep;
        os << items[i];
    }
    return os.str();
}

std::string Failure(const ModelLoadError& err) {
    return "fail: " + (err.summary.empty() ? err.ToString() : err.summary);
}

std::string Section(const std::string& name) {
    return "[" + name + "]";
}

std::string FormatDebugInfo(const ModelDebugInfo& info,
                            const std::string& rawSection,
                            const DebugViewContext& ctx) {
    std::string section = rawSection;
    std::transform(section.begin(), section.end(), section.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool all = (section.empty() || section == "all" || section == "debuginfo");

    std::ostringstream os;
    os << "Model: " << info.displayName << " [" << TypeName(info.type) << "]\n";
    os << "Path : " << (info.path.empty() ? ctx.fallbackPath : info.path) << "\n";

    if (all || section == "status") {
        os << Section("status 渲染状态") << "\n"
           << "  loaded      : " << (info.loaded ? "true" : "false") << "\n"
           << "  gpuReady    : " << (info.gpuReady ? "true" : "false") << "\n"
           << "  renderingOk : " << (info.renderingOk ? "true" : "false") << "\n"
           << "  glError     : " << info.glError << "\n"
           << "  renderStatus: " << info.renderStatus << "\n"
           << "  modelId     : " << (info.modelId.empty() ? ctx.fallbackPath : info.modelId)
           << "\n";
    }
    if (all || section == "params" || section == "param") {
        os << Section("params 规模参数") << "\n"
           << "  vertices=" << info.vertexCount << "  indices=" << info.indexCount
           << "  triangles=" << info.triangleCount << "\n"
           << "  bones=" << info.boneCount << "  materials=" << info.materialCount
           << "  subMeshes=" << info.subMeshCount << "  morphs=" << info.morphCount << "\n"
           << "  textures=" << info.textureLoaded << "/" << info.textureReferenced
           << "  drawCalls=" << info.drawCalls << "\n"
           << "  modelHeight=" << info.modelHeight << "\n";
    }
    if (all || section == "res" || section == "resources") {
        os << Section("res 加载的资源") << "\n";
        for (const auto& kv : ctx.resources) {
            os << "  " << kv.first << " : " << kv.second << "\n";
        }
        for (const ModelDebugInfo::TextureInfo& tex : info.textures) {
            os << "  texture : " << (tex.name.empty() ? "(无名)" : tex.name)
               << "  " << tex.width << "x" << tex.height
               << (tex.loaded ? "  [loaded]" : "  [missing]")
               << (tex.path.empty() ? "" : "  " + tex.path) << "\n";
        }
        if (ctx.resources.empty() && info.textures.empty()) {
            os << "  (后端未提供资源清单)\n";
        }
        for (const std::string& missing : info.missingResources) {
            os << "  MISSING : " << missing << "\n";
        }
    }
    if (all || section == "bones") {
        os << Section("bones 骨架 (" + std::to_string(info.bones.size()) + ")") << "\n";
        const std::size_t limit = std::min<std::size_t>(info.bones.size(), 40);
        for (std::size_t i = 0; i < limit; ++i) {
            const auto& b = info.bones[i];
            os << "  #" << b.index << " " << b.name << "  parent=" << b.parentIndex
               << "  depth=" << b.depth << "  " << b.flags << "\n";
        }
        if (info.bones.size() > limit) {
            os << "  ... 其余 " << (info.bones.size() - limit) << " 根省略\n";
        }
    }
    if (all || section == "parts" || section == "part") {
        os << Section("parts 模型部分 (" + std::to_string(info.parts.size()) + ")") << "\n";
        const std::size_t limit = std::min<std::size_t>(info.parts.size(), 40);
        for (std::size_t i = 0; i < limit; ++i) {
            const auto& p = info.parts[i];
            os << "  " << p.name << "  indices=" << p.indexCount
               << "  material=" << p.materialIndex << "  " << p.detail << "\n";
        }
    }
    if (all || section == "motion" || section == "actions") {
        os << Section("motion 动作") << "\n"
           << "  status : " << info.motionStatus << "\n"
           << "  summary: " << info.motionSummary << "\n"
           << "  actions: " << Join(ctx.motionNames, ", ") << "\n";
    }
    if (all || section == "expression" || section == "expr") {
        os << Section("expression 表情") << "\n"
           << "  status : " << info.expressionStatus << "\n"
           << "  summary: " << info.expressionSummary << "\n"
           << "  presets: " << Join(ctx.expressionIds, ", ") << "\n";
    }
    if (all || section == "error" || section == "errors") {
        os << Section("error 错误与警告") << "\n";
        if (!ctx.hasLoadError) {
            os << "  (无)\n";
        } else {
            os << "  category: " << ctx.errorCategory << "\n"
               << "  summary : " << ctx.errorSummary << "\n";
            for (const std::string& detail : ctx.errorDetails) {
                os << "    - " << detail << "\n";
            }
        }
        os << "  dialogue: " << info.dialogueStatus << "\n";
    }
    if (all || section == "property" || section == "properties") {
        os << Section("property 性质") << "\n"
           << "  format=" << info.formatVersion << "  encoding=" << info.textEncoding
           << "  skinned=" << info.isSkinned << "  outline=" << info.hasOutline << "\n";
        for (const std::string& p : info.properties) os << "  " << p << "\n";
    }
    return os.str();
}

}  // namespace console_fmt
}  // namespace ppet
