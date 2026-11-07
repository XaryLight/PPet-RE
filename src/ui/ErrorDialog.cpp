// PPet - C++ 移植版
// 「模型加载失败」弹窗实现。详见 ErrorDialog.hpp。
#include <src/ui/ErrorDialog.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include <src/tools/core/I18n.hpp>

namespace ppet {
namespace {

QString Q(const std::string& s) { return QString::fromStdString(s); }

/// HTML 转义：路径里可能有 & < >，拼进富文本标签会破坏结构。
QString Escape(const std::string& s) {
    return QString::fromStdString(s)
        .toHtmlEscaped();
}

}  // namespace

QWidget* BuildModelErrorDialog(const ModelLoadError& error, const Strings& strings,
                               const std::string& logPath, QWidget* parent) {
    const bool warning = error.warningOnly;
    const QString title = warning ? Q(strings.errTitleWarning) : Q(strings.errTitleFailed);
    const char* color = warning ? "#f9a825" : "#c62828";

    auto* dialog = new QDialog(parent);
    dialog->setObjectName("ppetModelErrorDialog");
    dialog->setWindowTitle(title);
    dialog->setMinimumSize(560, 420);
    dialog->resize(680, 500);
    dialog->setAttribute(Qt::WA_QuitOnClose, false);
    dialog->setWindowFlag(Qt::WindowStaysOnTopHint, true);

    auto* root = new QVBoxLayout(dialog);

    // --- 标题行：状态灯 + 标题 ---
    auto* header = new QLabel(
        QString("<span style='font-size:20px;color:%1;'>●</span>&nbsp;&nbsp;"
                "<b style='font-size:15px;'>%2</b>")
            .arg(color, Escape(title.toStdString())), dialog);
    root->addWidget(header);

    // --- 失败分类（显眼）---
    auto* category = new QLabel(
        QString("<b style='color:%1;'>%2</b>").arg(
            color, Escape(std::string("[") + error.CategoryName() + "]")), dialog);
    category->setObjectName("ppetErrorCategory");
    root->addWidget(category);

    // --- 一句话原因 ---
    auto* summary = new QLabel(Escape(error.summary), dialog);
    summary->setObjectName("ppetErrorSummary");
    summary->setWordWrap(true);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(summary);

    if (!error.path.empty()) {
        auto* path = new QLabel(Escape("文件：" + error.path), dialog);
        path->setObjectName("ppetErrorPath");
        path->setWordWrap(true);
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        path->setStyleSheet("color:#666;");
        root->addWidget(path);
    }

    // --- 详细信息（逐条明细 + 建议）---
    auto* group = new QGroupBox(Q(strings.errDetailGroup), dialog);
    auto* groupLayout = new QVBoxLayout(group);
    auto* detail = new QTextEdit(group);
    detail->setObjectName("ppetErrorDetails");
    detail->setReadOnly(true);
    detail->setLineWrapMode(QTextEdit::NoWrap);
    QFont mono("Monospace");
    mono.setStyleHint(QFont::TypeWriter);
    mono.setPointSize(9);
    detail->setFont(mono);
    detail->setPlainText(Q(error.ToReport()));
    groupLayout->addWidget(detail);
    root->addWidget(group, 1);

    // --- 建议（高亮）---
    auto* advice = new QLabel(
        QString("<b>%1：</b>%2").arg(Q(strings.errAdvice),
                                     Escape(error.EffectiveAdvice())), dialog);
    advice->setObjectName("ppetErrorAdvice");
    advice->setWordWrap(true);
    advice->setStyleSheet("background:#fff8e1;padding:8px;border-left:3px solid #f9a825;");
    root->addWidget(advice);

    // --- 日志位置 ---
    if (!logPath.empty()) {
        auto* log = new QLabel(Escape(strings.errLogHint + logPath), dialog);
        log->setObjectName("ppetErrorLog");
        log->setTextInteractionFlags(Qt::TextSelectableByMouse);
        log->setStyleSheet("color:#777;");
        root->addWidget(log);
    }

    // --- 按钮 ---
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* copyBtn = new QPushButton(Q(strings.errCopy), dialog);
    copyBtn->setObjectName("ppetErrorCopy");
    auto* closeBtn = new QPushButton(Q(strings.errClose), dialog);
    closeBtn->setObjectName("ppetErrorClose");
    buttons->addWidget(copyBtn);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);

    const QString report = Q(error.ToReport());
    const QString copiedText = Q(strings.errCopied);
    QObject::connect(copyBtn, &QPushButton::clicked,
                     [copyBtn, report, copiedText]() {
                         if (QClipboard* clipboard = QApplication::clipboard()) {
                             clipboard->setText(report);
                         }
                         copyBtn->setText(copiedText);
                     });
    QObject::connect(closeBtn, &QPushButton::clicked, dialog, &QDialog::close);

    dialog->setLayout(root);
    return dialog;
}

void ShowModelErrorDialog(const ModelLoadError& error, const Strings& strings,
                          const std::string& logPath) {
    if (!QApplication::instance()) return;  // 无 Qt 环境（无头测试）时静默跳过

    // 同一时刻只保留一个弹窗。QPointer 会在窗口被销毁后自动置空。
    static QPointer<QDialog> current;
    if (current) current->close();

    auto* dialog = qobject_cast<QDialog*>(
        BuildModelErrorDialog(error, strings, logPath, nullptr));
    if (!dialog) return;
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    current = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    // 立刻把事件分发出去，避免要等下一帧 PumpEvents 才画出内容。
    QApplication::processEvents();
}

}  // namespace ppet
