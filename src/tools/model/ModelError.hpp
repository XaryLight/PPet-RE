// PPet - C++ 移植版
// 结构化的「模型加载错误」：把过去一句含混的 error 字符串升级成
// 「分类 + 摘要 + 逐条明细 + 可操作建议」，供 Qt 失败弹窗与诊断面板使用。
//
// 【为什么放在头文件里且全部 inline】
//   ppet_model 的十几个测试目标并不都编译 Model.cpp，
//   若把实现放进 .cpp，凡是 include 了 Model.hpp 的轻量目标都会链接失败
//   （undefined reference / typeinfo）。纯 header + inline 可避免这类问题。
#pragma once

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace ppet {

/// 失败原因的大类。弹窗按它显示图标、标题与「怎么办」。
enum class ModelErrorCategory {
    None = 0,
    EmptyPath,           ///< 模型路径为空
    FileNotFound,        ///< 文件或关键资源不存在
    UnsupportedFormat,   ///< 无法识别/不支持的格式或版本
    BackendUnavailable,  ///< 对应后端未编译（典型：没开 Cubism SDK）
    PackageError,        ///< ZIP 包解压/定位失败
    ParseError,          ///< 解析错误：文件损坏、字段非法、越界、JSON 不合法
    NoValidSkeleton,     ///< 未找到有效骨架（骨骼为空或全部无效）
    MissingTexture,      ///< 模型图片 / 贴图缺失（PMX 单独导入最常见）
    MocError,            ///< .moc3 读取或初始化失败
    InternalError,       ///< 其它（内存不足、未知异常…）
};

/// 分类的中文短名（弹窗标题用）。
inline const char* ModelErrorCategoryName(ModelErrorCategory c) {
    switch (c) {
        case ModelErrorCategory::None:               return "无错误";
        case ModelErrorCategory::EmptyPath:          return "路径为空";
        case ModelErrorCategory::FileNotFound:       return "文件不存在";
        case ModelErrorCategory::UnsupportedFormat:  return "格式不支持";
        case ModelErrorCategory::BackendUnavailable: return "后端未启用";
        case ModelErrorCategory::PackageError:       return "模型包错误";
        case ModelErrorCategory::ParseError:         return "解析错误";
        case ModelErrorCategory::NoValidSkeleton:    return "未找到有效骨架";
        case ModelErrorCategory::MissingTexture:     return "模型图片缺失";
        case ModelErrorCategory::MocError:           return "moc3 模型错误";
        case ModelErrorCategory::InternalError:      break;
    }
    return "内部错误";
}

/// 分类对应的可操作建议（弹窗底部与日志里给出）。
inline const char* ModelErrorCategoryAdvice(ModelErrorCategory c) {
    switch (c) {
        case ModelErrorCategory::EmptyPath:
            return "请在设置里重新选择模型文件，或从托盘菜单「导入模型」。";
        case ModelErrorCategory::FileNotFound:
            return "确认文件仍在原位置；若模型被移动过，请重新导入。";
        case ModelErrorCategory::UnsupportedFormat:
            return "本项目支持 .zip / .pmx / .vrm / .model3.json / .moc3；"
                   "Cubism 2 的 .moc + .model.json 暂不支持。";
        case ModelErrorCategory::BackendUnavailable:
            return "Live2D 需要 Cubism SDK for Native：把 Core 放到 api/live2d/csdk/Core/，"
                   "重新 configure 并确认输出里有「启用真实 Cubism SDK」。";
        case ModelErrorCategory::PackageError:
            return "确认是有效的 .zip，且包内含有 .model3.json / .pmx / .vrm 之一。";
        case ModelErrorCategory::ParseError:
            return "文件可能已损坏或版本不兼容；换一份模型，或用原工具重新导出。";
        case ModelErrorCategory::NoValidSkeleton:
            return "模型里没有可用的骨骼数据，通常是导出时未勾选骨架，或文件被截断；"
                   "请换一份完整模型。";
        case ModelErrorCategory::MissingTexture:
            return "PMX 本身不包含图形数据，只记录贴图路径。"
                   "请把 .pmx 与 tex/ 等贴图目录一起打包成 .zip，再从「导入模型」选择该压缩包。";
        case ModelErrorCategory::MocError:
            return "确认 .moc3 与 .model3.json 同目录且配套；"
                   "Cubism SDK 版本需与导出模型的编辑器版本兼容。";
        case ModelErrorCategory::InternalError:
        case ModelErrorCategory::None:
        default:
            return "详情见 ./last.log；若持续出现请把日志与模型信息一并提交 issue。";
    }
    return "";
}

/// 把任意错误串归一成小写，便于关键词匹配（UTF-8 中文按字节处理，不受影响）。
inline std::string LowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

inline bool ContainsAny(const std::string& haystack,
                        std::initializer_list<const char*> needles) {
    for (const char* n : needles) {
        if (haystack.find(n) != std::string::npos) return true;
    }
    return false;
}

/// 从后端返回的原始错误字符串推断分类。
///
/// 各后端（PMX / VRM / Live2D）的错误文案都是中文，关键词足以稳定区分；
/// 调用方若已明确知道所处阶段（例如 ZIP 解压失败），应直接指定分类，
/// 本函数只用于「后端 Load() 返回 false」这种信息不全的兜底场景。
///
/// 顺序很重要：先匹配更具体的类别，再落到宽泛的 ParseError。
inline ModelErrorCategory InferErrorCategory(const std::string& raw) {
    const std::string s = LowerAscii(raw);
    if (s.empty()) return ModelErrorCategory::InternalError;

    if (ContainsAny(s, {"cubism sdk", "后端不可用", "需要编译", "后端未启用"}))
        return ModelErrorCategory::BackendUnavailable;
    // 注意不能用裸 ".zip"：「无法识别的模型格式（支持 .zip / .pmx / .vrm）」
    // 里也带 .zip，但那是格式错误，不该被归成模型包错误。
    if (ContainsAny(s, {"压缩包", "zip 包", "解压", "缓存目录", "归档", "包内"}))
        return ModelErrorCategory::PackageError;
    if (ContainsAny(s, {"骨架", "skeleton", "bone count", "骨骼为空"}))
        return ModelErrorCategory::NoValidSkeleton;
    if (ContainsAny(s, {"贴图", "纹理", "texture", "没有列出任何纹理", "图片文件"}))
        return ModelErrorCategory::MissingTexture;
    if (ContainsAny(s, {"moc", "cubism 模型", "画布信息"}))
        return ModelErrorCategory::MocError;
    if (ContainsAny(s, {"无法识别", "不支持", "版本号不支持", "格式不支持", "不支持的"}))
        return ModelErrorCategory::UnsupportedFormat;
    if (ContainsAny(s, {"路径为空", "路径是空"}))
        return ModelErrorCategory::EmptyPath;
    if (ContainsAny(s, {"不存在", "找不到", "没有找到", "无法打开", "无法读取", "打不开"}))
        return ModelErrorCategory::FileNotFound;
    if (ContainsAny(s, {"解析", "损坏", "截断", "越界", "非法", "异常", "失败", "json",
                        "索引", "数量", "偏移", "溢出", "不是有效", "宽度", "字段"}))
        return ModelErrorCategory::ParseError;
    return ModelErrorCategory::InternalError;
}

/// 一次模型加载的完整失败信息。
struct ModelLoadError {
    ModelErrorCategory category = ModelErrorCategory::None;
    std::string summary;                 ///< 一句话原因（原后端的错误串）
    std::string path;                    ///< 涉及的文件/包路径
    std::vector<std::string> details;    ///< 逐条明细（缺失资源清单、解析上下文…）
    std::string advice;                  ///< 覆盖默认建议（可留空）
    bool warningOnly = false;            ///< true = 加载成功但有隐患，弹「警告」而非「错误」

    bool ok() const { return category == ModelErrorCategory::None; }

    void Clear() {
        category = ModelErrorCategory::None;
        summary.clear();
        path.clear();
        details.clear();
        advice.clear();
        warningOnly = false;
    }

    void Fail(ModelErrorCategory c, std::string why, std::string where = {}) {
        category = c;
        summary = std::move(why);
        path = std::move(where);
        warningOnly = false;
    }

    /// 从后端原始错误串构造（自动推断分类）。
    static ModelLoadError FromRaw(const std::string& raw, const std::string& where = {}) {
        ModelLoadError e;
        if (raw.empty()) return e;
        e.Fail(InferErrorCategory(raw), raw, where);
        return e;
    }

    const char* CategoryName() const { return ModelErrorCategoryName(category); }

    std::string EffectiveAdvice() const {
        return advice.empty() ? ModelErrorCategoryAdvice(category) : advice;
    }

    /// 单行摘要，用于旧的 `std::string& error` 调用点与日志（兼容既有代码）。
    std::string ToString() const {
        if (ok()) return {};
        std::string s = std::string("[") + CategoryName() + "] " + summary;
        if (!path.empty()) s += "（" + path + "）";
        return s;
    }

    /// 多行详细报告，弹窗的「详细信息」区与「复制」按钮用。
    std::string ToReport() const {
        std::string s;
        s += "分类：" + std::string(CategoryName()) + "\n";
        s += "原因：" + summary + "\n";
        if (!path.empty()) s += "文件：" + path + "\n";
        if (!details.empty()) {
            s += "\n详细信息：\n";
            for (std::size_t i = 0; i < details.size(); ++i) {
                s += "  [" + std::to_string(i + 1) + "/" +
                     std::to_string(details.size()) + "] " + details[i] + "\n";
            }
        }
        s += "\n建议：" + EffectiveAdvice() + "\n";
        return s;
    }
};

}  // namespace ppet
