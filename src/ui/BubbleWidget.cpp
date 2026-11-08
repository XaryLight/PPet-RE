// PPet - C++ 移植版
// 台词气泡窗口实现。详见 BubbleWidget.hpp。
#include <src/ui/BubbleWidget.hpp>

#include <algorithm>

#include <QString>
#include <QApplication>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

namespace ppet {

BubbleWidget::BubbleWidget() = default;

BubbleWidget::~BubbleWidget() { Shutdown(); }

bool BubbleWidget::Initialize(std::string& error) {
    if (window_) return true;  // 已创建，幂等
    if (!QApplication::instance()) {
        error = "QApplication 尚未创建，无法创建台词气泡窗口";
        return false;
    }

    // 无边框、独立工具窗口、不抢焦点。
    // 遵循既有约定：置顶只作用于渲染窗口，Qt GUI（气泡/设置/诊断）保持普通层级，
    // 避免盖住用户正在操作的其他窗口。
    auto* window = new QWidget(nullptr);
    window->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool |
                           Qt::WindowDoesNotAcceptFocus);
    window->setAttribute(Qt::WA_TranslucentBackground, true);
    window->setStyleSheet("background:transparent;");

    auto* label = new QLabel(window);
    label->setObjectName("ppetBubbleLabel");
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    label->setMaximumWidth(280);
    label->setStyleSheet(
        "QLabel{"
        "background:#ffffff;color:#222222;"
        "border:1px solid #d0d0d0;border-radius:10px;"
        "padding:6px 10px;font-size:13px;"
        "}");

    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(label);
    window->setLayout(layout);
    window->adjustSize();
    window->hide();

    window_ = window;
    label_ = label;
    return true;
}

void BubbleWidget::Shutdown() {
    if (window_) {
        // 异步删除：事件循环会在安全时刻释放资源，避免跨线程/当前事件释放。
        window_->deleteLater();
        window_ = nullptr;
    }
    label_ = nullptr;
}

void BubbleWidget::SetText(const std::string& text) {
    if (!label_ || !window_) return;
    if (text.empty()) {
        label_->clear();
        return;
    }
    label_->setText(QString::fromStdString(text));
    window_->adjustSize();
}

void BubbleWidget::MoveTo(int x, int y) {
    if (!window_) return;
    window_->move(x, y);
}

void BubbleWidget::SetMaxWidth(int pixels) {
    const int clamped = std::clamp(pixels, 120, 900);
    if (clamped == maxWidth_ && label_) return;  // 每帧调用时短路，避免重复解析样式表
    maxWidth_ = clamped;
    if (!label_ || !window_) return;
    label_->setMaximumWidth(maxWidth_);
    // 字号随窗口放大（约 1/20 宽度），但限制在 12~20px，避免小窗口字过大。
    const int fontPx = std::clamp(maxWidth_ / 20, 12, 20);
    label_->setStyleSheet(
        QString("QLabel{background:#ffffff;color:#222222;"
                "border:1px solid #d0d0d0;border-radius:10px;"
                "padding:6px 10px;font-size:%1px;}")
            .arg(fontPx));
    window_->adjustSize();
}

int BubbleWidget::Width() const {
    return window_ ? window_->width() : maxWidth_;
}

int BubbleWidget::Height() const {
    return window_ ? window_->height() : 46;
}

void BubbleWidget::SetVisible(bool visible) {
    if (!window_) return;
    if (visible) {
        window_->show();
        window_->raise();
    } else {
        window_->hide();
    }
}

bool BubbleWidget::IsVisible() const {
    return window_ && window_->isVisible();
}

std::string BubbleWidget::Text() const {
    if (!label_) return {};
    return label_->text().toStdString();
}

}  // namespace ppet
