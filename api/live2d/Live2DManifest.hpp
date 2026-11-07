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
///
/// Cubism 3/4 规范里组值是数组，元素可带 FadeInTime / FadeOutTime：
///   "Idle": [ { "File": "a.motion3.json", "FadeInTime": 0.5, "FadeOutTime": 0.5 } ]
/// 这两个 fade 与 motion3.json 的 Meta.Fade* 二选一：优先用 motion 文件自带值，
/// 缺失时回退到这里的组级值（-1 表示该动作未指定）。
struct Live2DMotionGroup {
    std::string name;              ///< 组名（例如 "Idle"）
    std::vector<std::string> files; ///< 该组下的动作文件（.motion3.json）绝对路径
    std::vector<float> fadeInTimes;  ///< 与 files 一一对应，-1 = 未指定
    std::vector<float> fadeOutTimes; ///< 与 files 一一对应，-1 = 未指定
    /// 与 files 一一对应的**文件名**（不含目录，例如 "idle.motion3.json"）。
    ///
    /// 为什么需要它：很多模型（尤其是国内常见的一体成型模型包）把所有动作
    /// 都塞在 model3.json 的**空名组** `""` 下，组名完全不带语义：
    ///   "Motions": { "": [ { "File": "motions/idle.motion3.json" },
    ///                      { "File": "motions/touch_head.motion3.json" }, ... ] }
    /// 这时只能靠**文件名**区分「待机动作」与「点击互动动作」。
    /// 靠组名分类会让这类模型一个 Idle 组都没有——待机时一个动作都不播，
    /// 表现为「模型完全不动」（本项目实际遇到过的真实模型）。
    std::vector<std::string> fileNames;
};

/// 一个动画条目（model3.json 顶层的 Animations）。
struct Live2DAnimation {
    std::string name;  ///< 动画名
    std::string file;  ///< 动画文件（.motion3.json）绝对路径
};

/// 一个点击命中区（model3.json 顶层的 HitAreas）。
/// Cubism 用 Drawable 的 ID 关联区域，Name 是部位名（head/body/leg/...），
/// 用于把鼠标点击映射到对应的互动动作组（如 TapHead / TapBody）。
struct Live2DHitArea {
    std::string id;    ///< Drawable ID（如 "HitAreaHead"），精确命中判定用
    std::string name;  ///< 部位名（如 "head"），动作组映射用
};

/// .model3.json 的解析结果。
struct Live2DManifest {
    std::string model3Path;   ///< .model3.json 的绝对路径
    std::string baseDir;      ///< 相对路径的基准目录（.model3.json 所在目录）
    std::string mocPath;      ///< moc3 文件的绝对路径
    std::string physicsPath;  ///< physics3.json 的绝对路径（空串 = 模型没有物理）
    std::string displayName;  ///< 模型显示名（取自 model3.json 的 Name 字段，缺省用文件名）

    std::vector<std::string> texturePaths;    ///< 纹理绝对路径（顺序与 FileReferences.Textures 一致）
    std::vector<std::string> textureRelative; ///< 纹理原始相对路径（用于排查问题）

    std::vector<Live2DMotionGroup> motionGroups; ///< 动作组
    std::vector<Live2DAnimation> animations;     ///< 动画
    std::vector<Live2DHitArea> hitAreas;         ///< 点击命中区
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
