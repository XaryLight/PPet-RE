// PPet - C++ 移植版
// 「模型加载失败」弹窗（Qt 6）冒烟测试。
//
// 验证点：
//   1. 弹窗能创建，标题/分类/摘要/明细/建议/日志各区域都有内容；
//   2. 明细区包含逐条编号的资源清单与建议；
//   3. 复制详情、关闭按钮、非模态显示不崩溃；
//   4. 警告型（warningOnly）弹窗标题不同。
//
// 用 offscreen 平台插件运行（见 CMake 的 QT_QPA_PLATFORM=offscreen）。
#include <cstdio>
#include <string>

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>

#include <src/tools/core/I18n.hpp>
#include <src/tools/model/ModelError.hpp>
#include <src/ui/ErrorDialog.hpp>

namespace {

int Fail(const char* what) {
    std::printf("error_dialog_smoke: %s\n", what);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication qtApp(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    if (!QApplication::instance()) return 2;

    const ppet::Strings strings = ppet::GetStrings(ppet::Language::Chinese);

    ppet::ModelLoadError error;
    error.Fail(ppet::ModelErrorCategory::MissingTexture,
               "模型已加载，但缺少 2 张贴图（可能白模）", "/tmp/models/demo.pmx");
    error.details.push_back("/tmp/models/tex/body.png");
    error.details.push_back("/tmp/models/tex/face.png");

    QWidget* widget = ppet::BuildModelErrorDialog(error, strings, "./last.log");
    auto* dialog = qobject_cast<QDialog*>(widget);
    if (!dialog) return Fail("BuildModelErrorDialog 返回空");
    if (dialog->objectName() != "ppetModelErrorDialog") return Fail("窗口 objectName 不符");
    if (dialog->windowTitle().isEmpty()) return Fail("窗口标题为空");

    auto* category = dialog->findChild<QLabel*>("ppetErrorCategory");
    if (!category || !category->text().contains("模型图片缺失"))
        return Fail("分类标签未显示分类名");

    auto* summary = dialog->findChild<QLabel*>("ppetErrorSummary");
    if (!summary || !summary->text().contains("缺少 2 张贴图")) return Fail("摘要未显示原因");

    auto* details = dialog->findChild<QTextEdit*>("ppetErrorDetails");
    if (!details) return Fail("缺少详细信息区");
    const QString report = details->toPlainText();
    if (!report.contains("[1/2] /tmp/models/tex/body.png")) return Fail("明细缺逐条清单");
    if (!report.contains("打包成 .zip")) return Fail("明细缺建议");

    auto* advice = dialog->findChild<QLabel*>("ppetErrorAdvice");
    if (!advice || advice->text().isEmpty()) return Fail("建议区为空");

    auto* logLabel = dialog->findChild<QLabel*>("ppetErrorLog");
    if (!logLabel || !logLabel->text().contains("last.log")) return Fail("日志路径未显示");

    if (auto* copy = dialog->findChild<QPushButton*>("ppetErrorCopy")) {
        copy->click();
        QApplication::processEvents();
        if (copy->text().isEmpty()) return Fail("复制按钮点击后文本为空");
    } else {
        return Fail("缺少复制按钮");
    }

    // 非模态显示路径：不应阻塞、不应崩溃。
    ppet::ShowModelErrorDialog(error, strings, "./last.log");
    QApplication::processEvents();

    // 警告型标题应与失败型不同。
    ppet::ModelLoadError warning = error;
    warning.warningOnly = true;
    QWidget* w2 = ppet::BuildModelErrorDialog(warning, strings, "./last.log");
    auto* d2 = qobject_cast<QDialog*>(w2);
    if (!d2 || d2->windowTitle() == dialog->windowTitle())
        return Fail("警告型弹窗标题未区分");

    delete d2;
    delete dialog;
    QApplication::processEvents();

    std::printf("error_dialog_smoke: 通过（分类=%s 明细=%zu 条）\n",
                error.CategoryName(), error.details.size());
    return 0;
}
