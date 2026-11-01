// PPet - C++ 移植版
// Live2D 模型清单：解析 .model3.json，抽出 moc3 文件、纹理、动作与动画信息。
//
// 为什么要单独成文件：
//   1. 解析逻辑不依赖 Cubism SDK，因此可以在没有 SDK 的环境里直接单元测试；
//   2. 路径安全检查（防目录穿越）集中在这里，避免散落在各处被遗漏；
//   3. Live2DModel.cpp 只负责「模型生命周期 + 参数驱动 + 绘制」，
//      JSON 结构上的差异全部收敛在这里。
#pragma once

#include <string>
#include <vector>

namespace ppet::live2d {

/// 一个动作组（model3.json 的 FileReferences.Motions 的一项）。
struct Live2DMotionGroup {
    std::string name;              ///< 组名（例如 "Idle"）
    std::vector<std::string> files; ///< 该组下的动作文件（.motion3.json）绝对路径
};

/// 一个动画条目（model3.json 顶层的 Animations）。
struct Live2DAnimation {
    std::string name;  ///< 动画名
    std::string file;  ///< 动画文件（.motion3.json）绝对路径
};

/// .model3.json 的解析结果。
struct Live2DManifest {
    std::string model3Path;   ///< .model3.json 的绝对路径
    std::string baseDir;      ///< 相对路径的基准目录（.model3.json 所在目录）
    std::string mocPath;      ///< moc3 文件的绝对路径
    std::string displayName;  ///< 模型显示名（取自 model3.json 的 Name 字段，缺省用文件名）

    std::vector<std::string> texturePaths;    ///< 纹理绝对路径（顺序与 FileReferences.Textures 一致）
    std::vector<std::string> textureRelative; ///< 纹理原始相对路径（用于排查问题）

    std::vector<Live2DMotionGroup> motionGroups; ///< 动作组
    std::vector<Live2DAnimation> animations;     ///< 动画
};

/// 定位 .model3.json。
///
/// path 可能是：
///   - .model3.json 文件本身；
///   - 模型目录（目录里含 *.model3.json）；
///   - 其它 .json（自身不是 model3.json 时，在同目录里找 *.model3.json）；
///   - .moc3 文件（在同目录里找对应的 *.model3.json）。
///
/// @param path        输入路径
/// @param outModel3   输出的 .model3.json 绝对路径
/// @param outError    失败时写入中文原因
/// @return 成功返回 true。
bool LocateModel3Json(const std::string& path, std::string& outModel3, std::string& outError);

/// 解析 .model3.json。
///
/// @param model3Path .model3.json 的绝对路径
/// @param out        解析结果
/// @param outError   失败时写入中文原因
/// @return 成功返回 true。
bool ParseModel3Json(const std::string& model3Path, Live2DManifest& out, std::string& outError);

/// 把相对路径拼到基准目录下，并确保结果仍在基准目录内。
///
/// 防目录穿越：模型文件里的相对路径（纹理、动作）不允许用 ".." 跳出模型目录，
/// 否则一个恶意模型包就能让程序读取模型目录之外的文件。
///
/// @param baseDir 基准目录（绝对路径）
/// @param relative 相对路径（允许含子目录）
/// @param out     拼接后的绝对路径
/// @return 路径合法返回 true；越界或非法返回 false。
bool ResolveInsideBase(const std::string& baseDir, const std::string& relative, std::string& out);

}  // namespace ppet::live2d
