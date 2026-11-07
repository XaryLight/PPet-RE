// PPet - C++ 移植版
// 「模型加载失败」弹窗：把结构化的 ModelLoadError 用 Qt 呈现给用户。
//
// 【为什么不用 QMessageBox::critical 一把梭】
//   用户需要看到的不止一句「加载失败」：失败分类、逐条明细（缺了哪几张贴图、
//   解析停在哪一步）、可操作建议、日志位置、一键复制去提 issue。
//   自定义 QDialog 能把这些分层展示；且沿用 SettingsPanel/DebugPanel 的
//   「非 QObject + 主循环 PumpEvents」模式，不需要 moc / AUTOMOC。
//
// 【非模态】
//   用 show() 而非 exec()：exec() 会阻塞 GLFW 主循环，桌宠会卡住不动。
#pragma once

#include <string>

#include <src/tools/model/ModelError.hpp>

class QWidget;

namespace ppet {

struct Strings;

/// 构造弹窗但不显示（供冒烟测试断言控件内容）。
/// 返回的是 QDialog*（以 QWidget* 传出），调用方可自行 show() 或 delete。
QWidget* BuildModelErrorDialog(const ModelLoadError& error, const Strings& strings,
                               const std::string& logPath, QWidget* parent = nullptr);

/// 构造并**非模态显示**弹窗。关闭时自动释放（WA_DeleteOnClose）。
/// 同一时刻只保留一个：再次调用会先关掉上一个，避免连续失败叠出一堆窗口。
void ShowModelErrorDialog(const ModelLoadError& error, const Strings& strings,
                          const std::string& logPath);

}  // namespace ppet
