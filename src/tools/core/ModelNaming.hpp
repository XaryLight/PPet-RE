// PPet - C++ 移植版
// 模型显示名：从路径推导默认名、重名加序号、名字列表与路径列表对齐。
//
// 【为什么不用 std::filesystem】
//   Windows 的 std::filesystem::path 从 char* 构造时会用
//   MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS) 做窄->宽转换。
//   模型路径可能来自旧配置、zip 条目名、剪贴板或在线地址，字节序列不保证是
//   合法 UTF-8；一旦含非法字节就会抛
//   `filesystem error: in __char_to_wide: Illegal byte sequence`，
//   让 `list` 这类只读命令整条失败（用户在 Windows 实测复现）。
//   显示名只是「给人看的字符串」，用纯字节切分即可，完全不碰 locale / 编码转换，
//   因此对任意字节都安全、可单测。
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace ppet::model_naming {

/// 取路径最后一段（同时兼容 '/' 与 '\\'），并去掉 URL 查询/片段。
/// 不抛异常、不做编码转换、不访问磁盘。
inline std::string LeafName(std::string_view path) {
    if (const std::size_t q = path.find_first_of("?#"); q != std::string_view::npos) {
        path = path.substr(0, q);
    }
    // 结尾的斜杠不算文件名（目录路径）。
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
        path.remove_suffix(1);
    }
    const std::size_t pos = path.find_last_of("/\\");
    return std::string(pos == std::string_view::npos ? path : path.substr(pos + 1));
}

inline bool EndsWithAsciiCase(std::string_view text, std::string_view suffix) {
    if (suffix.size() > text.size()) return false;
    const std::string_view tail = text.substr(text.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(tail[i]);
        const unsigned char b = static_cast<unsigned char>(suffix[i]);
        const char la = static_cast<char>(a >= 'A' && a <= 'Z' ? a + 32 : a);
        const char lb = static_cast<char>(b >= 'A' && b <= 'Z' ? b + 32 : b);
        if (la != lb) return false;
    }
    return true;
}

/// 文件名去后缀：默认名。
/// 复合后缀 `.model3.json` 优先整体去掉，否则 "nengdai_2.model3" 会很难看。
inline std::string FileStem(std::string_view path) {
    std::string name = LeafName(path);
    if (name.empty()) return {};

    // 长后缀在前，保证复合后缀优先匹配。
    static constexpr std::string_view kSuffixes[] = {
        ".model3.json", ".pmx", ".vrm", ".moc3", ".zip", ".json", ".glb", ".gltf",
    };
    for (const std::string_view suffix : kSuffixes) {
        if (EndsWithAsciiCase(name, suffix)) {
            name.resize(name.size() - suffix.size());
            return name;
        }
    }
    // 通用后缀：去掉最后一个点之后的部分（点号不作为首字符时不算后缀）。
    if (const std::size_t dot = name.find_last_of('.'); dot != std::string::npos && dot > 0) {
        name.resize(dot);
    }
    return name;
}

inline bool ContainsName(const std::vector<std::string>& names, const std::string& candidate) {
    return std::find(names.begin(), names.end(), candidate) != names.end();
}

/// 与已有名字重名时在后面标注序号：`abc` -> `abc (2)` -> `abc (3)`。
/// 用有界循环防御极端情况下的名字洪泛。
inline std::string UniqueName(const std::string& base,
                              const std::vector<std::string>& existing) {
    if (!ContainsName(existing, base)) return base;
    for (int i = 2; i < 100000; ++i) {
        std::string candidate = base + " (" + std::to_string(i) + ")";
        if (!ContainsName(existing, candidate)) return candidate;
    }
    return base + " (overflow)";
}

/// 把「路径列表 + 已保存的名字列表」对齐成一份可直接显示的名单。
/// 规则：
///   - 保存的名字非空则沿用；
///   - 否则取文件名（去后缀）；
///   - 文件名为空时用「模型 N」兜底；
///   - 最终逐个做重名去重，即使旧配置里已经重名也能修正。
inline std::vector<std::string> ResolveNames(const std::vector<std::string>& paths,
                                             const std::vector<std::string>& stored) {
    std::vector<std::string> out;
    out.reserve(paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) {
        std::string base = (i < stored.size()) ? stored[i] : std::string();
        if (base.empty()) base = FileStem(paths[i]);
        if (base.empty()) base = "模型 " + std::to_string(i + 1);
        out.push_back(UniqueName(base, out));
    }
    return out;
}

/// 安全取第 i 个显示名（越界/空列表回退到路径文件名）。
inline std::string NameAt(const std::vector<std::string>& paths,
                          const std::vector<std::string>& stored, std::size_t index) {
    if (index < stored.size() && !stored[index].empty()) return stored[index];
    if (index < paths.size()) {
        const std::string stem = FileStem(paths[index]);
        if (!stem.empty()) return stem;
    }
    return "模型 " + std::to_string(index + 1);
}

}  // namespace ppet::model_naming
