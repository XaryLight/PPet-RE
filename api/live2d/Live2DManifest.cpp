// PPet - C++ 移植版
// Live2D 模型清单解析实现。
#include <src/tools/core/FileIO.hpp>
#include <api/live2d/Live2DManifest.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#include <nlohmann/json.hpp>

namespace ppet::live2d {
namespace {

namespace fs = std::filesystem;

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

/// ASCII 小写化（UTF-8 多字节序列原样保留）。
std::string AsciiLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/// 文件名是否是 *.model3.json。
bool IsModel3FileName(const std::string& name) {
    const std::string lower = AsciiLower(name);
    return lower.size() > 12 &&
           lower.compare(lower.size() - 12, 12, ".model3.json") == 0;
}

/// 把路径规范化成绝对路径（不要求文件存在）。
std::string AbsolutePath(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(ToFsPath(path), ec);
    if (ec) return FromFsPath(ToFsPath(path));
    return FromFsPath(p.lexically_normal());
}

/// 判断 child 是否位于 base 之内（按路径分量比较，避免 "/a/bc" 被 "/a/b" 误判为子路径）。
bool IsInside(const fs::path& base, const fs::path& child) {
    auto b = base.begin();
    auto c = child.begin();
    for (; b != base.end(); ++b, ++c) {
        if (c == child.end()) return false;
        if (*b != *c) return false;
    }
    return true;
}

/// 在目录里找出第一个 *.model3.json。找不到返回空串。
std::string FindModel3InDir(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return "";

    std::vector<std::string> candidates;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return "";
    for (const auto& entry : it) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) continue;
        const std::string name = FromFsPath(entry.path().filename());
        if (IsModel3FileName(name)) candidates.push_back(FromFsPath(entry.path()));
    }
    if (candidates.empty()) return "";
    // 排序后取第一个，保证结果稳定可复现。
    std::sort(candidates.begin(), candidates.end());
    return candidates.front();
}

/// 从 JSON 对象里取字符串字段，失败返回空串。
std::string JsonString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return "";
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

/// 从组条目里读出可选的 FadeInTime / FadeOutTime（缺省 -1）。
float JsonFade(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return -1.0f;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return -1.0f;
    return std::max(0.0f, it->get<float>());
}

/// 收集动作组里的动作文件与组级 fade（绝对路径 / 秒）。
///
/// 【支持三种写法，覆盖 Cubism 3/4 与常见工具】
///   1) 标准数组："Idle": [ { "File": "a.motion3.json", "FadeInTime": 0.5 }, ... ]
///   2) 单对象  ："Idle": { "File": "a.motion3.json" }
///   3) 字符串  ："Idle": "a.motion3.json"  或 { "File": ["a", "b"] }
/// 旧实现只认 2/3，遇到标准数组直接 return，导致真实模型动作组全被解析为空。
void CollectMotionFiles(const nlohmann::json& groupValue, const std::string& baseDir,
                        std::vector<std::string>& outFiles,
                        std::vector<float>& outFadeIn,
                        std::vector<float>& outFadeOut) {
    auto pushFile = [&](const std::string& rel, float fin, float fout) {
        std::string abs;
        if (ResolveInsideBase(baseDir, rel, abs)) {
            outFiles.push_back(abs);
            outFadeIn.push_back(fin);
            outFadeOut.push_back(fout);
        }
    };

    if (groupValue.is_array()) {
        // 标准写法：数组的每个元素是一个动作条目对象。
        for (const auto& item : groupValue) {
            CollectMotionFiles(item, baseDir, outFiles, outFadeIn, outFadeOut);
        }
        return;
    }
    if (groupValue.is_string()) {
        pushFile(groupValue.get<std::string>(), -1.0f, -1.0f);
        return;
    }
    if (!groupValue.is_object()) return;

    const auto it = groupValue.find("File");
    if (it == groupValue.end()) return;
    const nlohmann::json& file = it.value();
    const float fin = JsonFade(groupValue, "FadeInTime");
    const float fout = JsonFade(groupValue, "FadeOutTime");

    if (file.is_string()) {
        pushFile(file.get<std::string>(), fin, fout);
    } else if (file.is_array()) {
        for (const auto& entry : file) {
            if (entry.is_string()) {
                pushFile(entry.get<std::string>(), fin, fout);
            } else if (entry.is_object()) {
                // 少数工具把 File 写成对象数组，条目内可能自带 fade。
                const auto fit = entry.find("File");
                if (fit == entry.end() || !fit->is_string()) continue;
                const float efin = JsonFade(entry, "FadeInTime");
                const float efout = JsonFade(entry, "FadeOutTime");
                pushFile(fit->get<std::string>(),
                         efin >= 0.0f ? efin : fin,
                         efout >= 0.0f ? efout : fout);
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 路径安全
// ---------------------------------------------------------------------------

bool ResolveInsideBase(const std::string& baseDir, const std::string& relative,
                       std::string& out) {
    if (relative.empty()) return false;

    // 绝对路径直接拒绝：模型里的引用必须是相对路径。
    const fs::path rel = ToFsPath(relative);
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) return false;

    // 含盘符的路径（如 "C:\\xxx"）在 Windows 上也是绝对路径。
    if (relative.size() >= 2 && relative[1] == ':') return false;

    std::error_code ec;
    const fs::path base = fs::weakly_canonical(ToFsPath(baseDir), ec);
    const fs::path baseForCompare = ec ? ToFsPath(baseDir).lexically_normal() : base;

    const fs::path child = fs::weakly_canonical(baseForCompare / rel, ec);
    const fs::path childForCompare = ec ? (baseForCompare / rel).lexically_normal() : child;

    if (!IsInside(baseForCompare, childForCompare)) return false;

    out = FromFsPath(childForCompare);
    return true;
}

// ---------------------------------------------------------------------------
// 定位 .model3.json
// ---------------------------------------------------------------------------

bool LocateModel3Json(const std::string& path, std::string& outModel3, std::string& outError) {
    if (path.empty()) {
        outError = "Live2D 模型路径为空";
        return false;
    }

    std::error_code ec;
    const fs::path input = ToFsPath(path);

    // 1) 目录：在目录里找 *.model3.json。
    if (fs::is_directory(input, ec)) {
        const std::string found = FindModel3InDir(input);
        if (found.empty()) {
            outError = "目录 " + FromFsPath(input) + " 里没有找到 .model3.json 文件";
            return false;
        }
        outModel3 = found;
        return true;
    }

    // 2) 文件：按扩展名分情况处理。
    const std::string ext = LowerExtension(path);

    if (ext == ".model3.json") {
        if (!fs::is_regular_file(input, ec)) {
            outError = "模型配置文件不存在：" + path;
            return false;
        }
        outModel3 = AbsolutePath(path);
        return true;
    }

    if (ext == ".json") {
        // 自身可能就是 model3.json（只是文件名不标准），先按内容判断；
        // 不像的话再去同目录里找标准的 *.model3.json。
        if (fs::is_regular_file(input, ec)) {
            outModel3 = AbsolutePath(path);
            return true;
        }
        const fs::path dir = input.parent_path();
        const std::string found = FindModel3InDir(dir.empty() ? fs::path(".") : dir);
        if (!found.empty()) {
            outModel3 = found;
            return true;
        }
        outError = "找不到模型配置文件：" + path;
        return false;
    }

    if (ext == ".moc3" || ext == ".moc") {
        const fs::path dir = input.parent_path();
        const std::string found = FindModel3InDir(dir.empty() ? fs::path(".") : dir);
        if (!found.empty()) {
            outModel3 = found;
            return true;
        }
        outError = "只找到 " + FromFsPath(input.filename()) +
                   "，但同目录下没有对应的 .model3.json，无法确定纹理与动作";
        return false;
    }

    outError = "不支持的 Live2D 模型路径：" + path;
    return false;
}

// ---------------------------------------------------------------------------
// 解析 .model3.json
// ---------------------------------------------------------------------------

bool ParseModel3Json(const std::string& model3Path, Live2DManifest& out, std::string& outError) {
    std::error_code ec;
    if (!fs::is_regular_file(ToFsPath(model3Path), ec)) {
        outError = "模型配置文件不存在：" + model3Path;
        return false;
    }

    nlohmann::json doc;
    try {
        // 走跨平台读取接口：Windows 上窄字符路径打不开含中文的模型目录。
        std::string text;
        if (!ReadFileText(model3Path, text)) {
            outError = "无法打开模型配置文件：" + model3Path;
            return false;
        }
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        outError = std::string("模型配置文件不是合法 JSON：") + e.what();
        return false;
    } catch (const std::exception& e) {
        outError = std::string("读取模型配置文件失败：") + e.what();
        return false;
    }

    if (!doc.is_object()) {
        outError = "模型配置文件根节点不是对象";
        return false;
    }

    // --- FileReferences -----------------------------------------------------
    const nlohmann::json* refs = nullptr;
    if (doc.contains("FileReferences") && doc.at("FileReferences").is_object()) {
        refs = &doc.at("FileReferences");
    }
    if (!refs) {
        outError = "模型配置文件缺少 FileReferences 字段";
        return false;
    }

    std::string baseDir = FromFsPath(ToFsPath(model3Path).parent_path());
    if (baseDir.empty()) baseDir = ".";

    // --- moc3 ---------------------------------------------------------------
    const std::string mocRelative = JsonString(*refs, "Moc");
    if (mocRelative.empty()) {
        outError = "模型配置文件缺少 FileReferences.Moc";
        return false;
    }
    std::string mocPath;
    if (!ResolveInsideBase(baseDir, mocRelative, mocPath)) {
        outError = "模型引用的 moc 文件路径非法（越出模型目录）：" + mocRelative;
        return false;
    }
    if (!fs::is_regular_file(ToFsPath(mocPath), ec)) {
        outError = "moc 文件不存在：" + mocPath;
        return false;
    }

    // --- 纹理 ---------------------------------------------------------------
    std::vector<std::string> texturePaths;
    std::vector<std::string> textureRelative;
    if (refs->contains("Textures") && refs->at("Textures").is_array()) {
        for (const auto& item : refs->at("Textures")) {
            if (!item.is_string()) continue;
            const std::string rel = item.get<std::string>();
            std::string abs;
            // 越界路径直接丢弃，不让整个模型加载失败。
            if (!ResolveInsideBase(baseDir, rel, abs)) continue;
            texturePaths.push_back(abs);
            textureRelative.push_back(rel);
        }
    }
    if (texturePaths.empty()) {
        outError = "模型配置文件没有列出任何纹理";
        return false;
    }

    // --- 动作组 -------------------------------------------------------------
    std::vector<Live2DMotionGroup> motionGroups;
    if (refs->contains("Motions") && refs->at("Motions").is_object()) {
        for (const auto& item : refs->at("Motions").items()) {
            Live2DMotionGroup group;
            group.name = item.key();
            CollectMotionFiles(item.value(), baseDir, group.files,
                               group.fadeInTimes, group.fadeOutTimes);
            motionGroups.push_back(group);
        }
    }

    // --- 动画（顶层 Animations）---------------------------------------------
    std::vector<Live2DAnimation> animations;
    if (doc.contains("Animations") && doc.at("Animations").is_object()) {
        for (const auto& item : doc.at("Animations").items()) {
            const nlohmann::json& value = item.value();
            if (!value.is_object()) continue;
            const auto it = value.find("File");
            if (it == value.end() || !it->is_string()) continue;
            std::string abs;
            if (!ResolveInsideBase(baseDir, it->get<std::string>(), abs)) continue;
            Live2DAnimation anim;
            anim.name = item.key();
            anim.file = abs;
            animations.push_back(anim);
        }
    }

    // --- 点击命中区（顶层 HitAreas）-----------------------------------------
    // Cubism 3 规范：HitAreas 是顶层数组，元素形如
    //   { "Id": "HitAreaHead", "Name": "head" }
    // Name 是部位名，用于把点击位置映射到互动动作组（TapHead 等）。
    std::vector<Live2DHitArea> hitAreas;
    if (doc.contains("HitAreas") && doc.at("HitAreas").is_array()) {
        for (const auto& item : doc.at("HitAreas")) {
            if (!item.is_object()) continue;
            Live2DHitArea area;
            area.id = JsonString(item, "Id");
            area.name = JsonString(item, "Name");
            if (area.id.empty() && area.name.empty()) continue;
            hitAreas.push_back(std::move(area));
        }
    }

    // --- 输出 ---------------------------------------------------------------
    out = Live2DManifest();
    out.model3Path = AbsolutePath(model3Path);
    out.baseDir = baseDir;
    out.mocPath = mocPath;
    out.texturePaths = texturePaths;
    out.textureRelative = textureRelative;
    out.motionGroups = motionGroups;
    out.animations = animations;
    out.hitAreas = hitAreas;

    std::string name = JsonString(doc, "Name");
    if (name.empty()) name = JsonString(doc, "name");
    if (name.empty()) {
        // 退回到文件名（去掉 ".model3.json" 后缀）。
        std::string fileName = FromFsPath(ToFsPath(model3Path).filename());
        const std::size_t dot = fileName.find_last_of('.');
        if (dot != std::string::npos && dot > 0) fileName = fileName.substr(0, dot);
        name = fileName;
    }
    out.displayName = name;

    return true;
}

}  // namespace ppet::live2d
