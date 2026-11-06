// PPet - C++ 移植版
// 设置界面实现：独立 Qt 6 窗口（QDialog + QTabWidget）。
//
// 设计要点（详见 SettingsPanel.hpp 顶部注释）：
//   - 用原生 Qt 控件构建界面，中文与高 DPI 天然支持；
//   - 不需要第二个 GL 上下文，彻底消除「ImGui 初始化失败 / 污染主窗口渲染」；
//   - 每帧由 App 调用 PumpEvents() 驱动 Qt 事件分发，与 GLFW 主循环共用主线程；
//   - 所有信号用 lambda 连接，本类不是 QObject，无需 moc / AUTOMOC。
#include "ui/SettingsPanel.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QEventLoop>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include "core/Log.hpp"

namespace ppet {

namespace {

/// 把 std::string 转成 Qt 字符串（UTF-8 直通）。
QString ToQString(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

// 标签页固定索引，Refresh() 时按索引设置文案。
enum TabIndex {
    kTabModel = 0,
    kTabRender = 1,
    kTabWindow = 2,
    kTabLang = 3,
    kTabAbout = 4,
};

struct SettingsPanel::Impl {
    QDialog* dialog = nullptr;
    QTabWidget* tabs = nullptr;

    // 模型
    QListWidget* modelList = nullptr;
    QPushButton* importBtn = nullptr;
    QPushButton* removeBtn = nullptr;
    QLabel* onlineUrlLabel = nullptr;
    QLineEdit* urlEdit = nullptr;
    QPushButton* importUrlBtn = nullptr;
    QLabel* onlineHintLabel = nullptr;

    // 渲染与追踪
    QLabel* renderSizeLabel = nullptr;
    QSlider* renderScaleSlider = nullptr;
    QLabel* renderScaleValue = nullptr;
    QCheckBox* fullscreenChk = nullptr;
    QLabel* trackingRadiusLabel = nullptr;
    QSlider* radiusSlider = nullptr;
    QLabel* radiusValue = nullptr;
    QLabel* maxAngleLabel = nullptr;
    QSlider* angleSlider = nullptr;
    QLabel* angleValue = nullptr;
    QLabel* trackingHint = nullptr;

    // 窗口
    QCheckBox* alwaysOnTopChk = nullptr;
    QCheckBox* ignoreMouseChk = nullptr;
    QCheckBox* showToolChk = nullptr;

    // 语言
    QComboBox* langCombo = nullptr;

    // 关于
    QTextEdit* aboutEdit = nullptr;
    QPushButton* clearBtn = nullptr;

    // 刷新时用它屏蔽「因 setValue/setCurrentRow 触发的信号 -> 回调 -> 再刷新」回路。
    bool updating = false;
};

SettingsPanel::SettingsPanel() : impl_(std::make_unique<Impl>()) {}

SettingsPanel::~SettingsPanel() { Shutdown(); }

bool SettingsPanel::Initialize(std::string& error) {
    if (impl_->dialog) return true;

    // Qt 必须在主线程且 QApplication 已创建后才能创建 QWidget。
    if (!QApplication::instance()) {
        error = "QApplication 尚未创建，无法创建设置窗口";
        return false;
    }

    auto* dialog = new QDialog(nullptr);
    dialog->setWindowTitle("PPet 设置");
    dialog->setMinimumSize(560, 460);
    dialog->resize(720, 560);
    // 关闭设置窗口不应退出整个程序（主窗口是 GLFW 窗口，不在 Qt 的管理范围内）。
    dialog->setAttribute(Qt::WA_QuitOnClose, false);
    // 默认置顶，与原 ImGui 设置窗口（GLFW_FLOATING）行为一致。
    dialog->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    impl_->dialog = dialog;

    BuildUi();

    // 先按当前文案/配置刷新一次，保证首次显示就是正确语言与数值。
    Refresh();

    dialog->hide();
    return true;
}

void SettingsPanel::BuildUi() {
    auto* dialog = impl_->dialog;
    auto* root = new QVBoxLayout(dialog);

    auto* tabs = new QTabWidget(dialog);
    impl_->tabs = tabs;
    root->addWidget(tabs, 1);

    // ---------------- 模型 ----------------
    {
        auto* page = new QWidget(tabs);
        auto* layout = new QVBoxLayout(page);

        auto* listGroup = new QGroupBox("模型列表", page);
        auto* listLayout = new QVBoxLayout(listGroup);
        auto* modelList = new QListWidget(listGroup);
        impl_->modelList = modelList;
        listLayout->addWidget(modelList);
        layout->addWidget(listGroup);

        auto* btnRow = new QHBoxLayout();
        auto* importBtn = new QPushButton("导入模型", page);
        auto* removeBtn = new QPushButton("移除模型", page);
        impl_->importBtn = importBtn;
        impl_->removeBtn = removeBtn;
        btnRow->addWidget(importBtn);
        btnRow->addWidget(removeBtn);
        btnRow->addStretch(1);
        layout->addLayout(btnRow);

        auto* onlineUrlLabel = new QLabel("在线模型地址（URL）", page);
        impl_->onlineUrlLabel = onlineUrlLabel;
        layout->addWidget(onlineUrlLabel);

        auto* urlRow = new QHBoxLayout();
        auto* urlEdit = new QLineEdit(page);
        urlEdit->setPlaceholderText("https://.../model.vrm");
        auto* importUrlBtn = new QPushButton("导入在线模型", page);
        impl_->urlEdit = urlEdit;
        impl_->importUrlBtn = importUrlBtn;
        urlRow->addWidget(urlEdit, 1);
        urlRow->addWidget(importUrlBtn);
        layout->addLayout(urlRow);

        auto* onlineHintLabel =
            new QLabel("填入 .model3.json / .pmx / .vrm 的直链后点击导入", page);
        onlineHintLabel->setWordWrap(true);
        impl_->onlineHintLabel = onlineHintLabel;
        layout->addWidget(onlineHintLabel);

        layout->addStretch(1);
        tabs->addTab(page, "模型");
    }

    // ---------------- 渲染与追踪 ----------------
    {
        auto* page = new QWidget(tabs);
        auto* layout = new QVBoxLayout(page);

        auto* renderGroup = new QGroupBox("渲染大小", page);
        auto* renderLayout = new QVBoxLayout(renderGroup);
        auto* renderSizeLabel = new QLabel("渲染大小", renderGroup);
        impl_->renderSizeLabel = renderSizeLabel;
        auto* scaleRow = new QHBoxLayout();
        auto* renderScaleSlider = new QSlider(Qt::Horizontal, renderGroup);
        renderScaleSlider->setRange(25, 400);  // 0.25× ~ 4.00×
        renderScaleSlider->setValue(100);
        auto* renderScaleValue = new QLabel("1.00×", renderGroup);
        impl_->renderScaleSlider = renderScaleSlider;
        impl_->renderScaleValue = renderScaleValue;
        scaleRow->addWidget(renderSizeLabel, 1);
        scaleRow->addWidget(renderScaleSlider, 3);
        scaleRow->addWidget(renderScaleValue);
        renderLayout->addLayout(scaleRow);
        layout->addWidget(renderGroup);

        auto* trackingGroup = new QGroupBox("全屏追踪", page);
        auto* trackingLayout = new QVBoxLayout(trackingGroup);
        auto* fullscreenChk = new QCheckBox("全屏追踪", trackingGroup);
        impl_->fullscreenChk = fullscreenChk;
        trackingLayout->addWidget(fullscreenChk);

        auto* radiusRow = new QHBoxLayout();
        auto* trackingRadiusLabel = new QLabel("追踪半径", trackingGroup);
        auto* radiusSlider = new QSlider(Qt::Horizontal, trackingGroup);
        radiusSlider->setRange(50, 2000);
        radiusSlider->setValue(400);
        auto* radiusValue = new QLabel("400 px", trackingGroup);
        impl_->trackingRadiusLabel = trackingRadiusLabel;
        impl_->radiusSlider = radiusSlider;
        impl_->radiusValue = radiusValue;
        radiusRow->addWidget(trackingRadiusLabel, 1);
        radiusRow->addWidget(radiusSlider, 3);
        radiusRow->addWidget(radiusValue);
        trackingLayout->addLayout(radiusRow);

        auto* angleRow = new QHBoxLayout();
        auto* maxAngleLabel = new QLabel("最大偏转角度", trackingGroup);
        auto* angleSlider = new QSlider(Qt::Horizontal, trackingGroup);
        angleSlider->setRange(0, 90);
        angleSlider->setValue(30);
        auto* angleValue = new QLabel("30°", trackingGroup);
        impl_->maxAngleLabel = maxAngleLabel;
        impl_->angleSlider = angleSlider;
        impl_->angleValue = angleValue;
        angleRow->addWidget(maxAngleLabel, 1);
        angleRow->addWidget(angleSlider, 3);
        angleRow->addWidget(angleValue);
        trackingLayout->addLayout(angleRow);

        auto* trackingHint = new QLabel(
            "开启后，模型的眼睛与朝向会跟随整个屏幕范围内的鼠标位置", trackingGroup);
        trackingHint->setWordWrap(true);
        impl_->trackingHint = trackingHint;
        trackingLayout->addWidget(trackingHint);

        layout->addWidget(trackingGroup);
        layout->addStretch(1);
        tabs->addTab(page, "渲染与追踪");
    }

    // ---------------- 窗口 ----------------
    {
        auto* page = new QWidget(tabs);
        auto* layout = new QVBoxLayout(page);
        auto* windowGroup = new QGroupBox("窗口", page);
        auto* windowLayout = new QVBoxLayout(windowGroup);
        impl_->alwaysOnTopChk = new QCheckBox("窗口置顶", windowGroup);
        impl_->ignoreMouseChk = new QCheckBox("忽略鼠标点击", windowGroup);
        impl_->showToolChk = new QCheckBox("显示工具栏", windowGroup);
        windowLayout->addWidget(impl_->alwaysOnTopChk);
        windowLayout->addWidget(impl_->ignoreMouseChk);
        windowLayout->addWidget(impl_->showToolChk);
        layout->addWidget(windowGroup);
        layout->addStretch(1);
        tabs->addTab(page, "窗口");
    }

    // ---------------- 语言 ----------------
    {
        auto* page = new QWidget(tabs);
        auto* layout = new QVBoxLayout(page);
        auto* langGroup = new QGroupBox("语言", page);
        auto* langLayout = new QHBoxLayout(langGroup);
        auto* langCombo = new QComboBox(langGroup);
        langCombo->addItem("简体中文");
        langCombo->addItem("English");
        impl_->langCombo = langCombo;
        langLayout->addWidget(langCombo);
        langLayout->addStretch(1);
        layout->addWidget(langGroup);
        layout->addStretch(1);
        tabs->addTab(page, "语言");
    }

    // ---------------- 关于 ----------------
    {
        auto* page = new QWidget(tabs);
        auto* layout = new QVBoxLayout(page);
        auto* aboutGroup = new QGroupBox("关于", page);
        auto* aboutLayout = new QVBoxLayout(aboutGroup);
        auto* aboutEdit = new QTextEdit(aboutGroup);
        aboutEdit->setReadOnly(true);
        impl_->aboutEdit = aboutEdit;
        aboutLayout->addWidget(aboutEdit);
        layout->addWidget(aboutGroup);
        layout->addStretch(1);
        tabs->addTab(page, "关于");
    }

    // ---------------- 底部：清除设置 ----------------
    auto* bottom = new QHBoxLayout();
    bottom->addStretch(1);
    auto* clearBtn = new QPushButton("清除设置", dialog);
    impl_->clearBtn = clearBtn;
    bottom->addWidget(clearBtn);
    root->addLayout(bottom);

    // ===================== 信号连接（全部用 lambda，无需 moc） =====================
    auto* w = impl_.get();

    // 模型列表：选中即切换当前模型。
    QObject::connect(impl_->modelList, &QListWidget::currentRowChanged,
                     [this, w](int row) {
                         if (w->updating) return;
                         if (row >= 0 && onSelectModel) onSelectModel(row);
                     });

    // 导入模型（本地文件）。
    QObject::connect(impl_->importBtn, &QPushButton::clicked,
                     [this]() { OpenImportDialog(); });

    // 移除模型。
    QObject::connect(impl_->removeBtn, &QPushButton::clicked, [this, w]() {
        const int row = w->modelList->currentRow();
        if (row < 0) return;
        if (onRemoveModel) onRemoveModel(row);
        Refresh();
    });

    // 导入在线模型（URL）。
    QObject::connect(impl_->importUrlBtn, &QPushButton::clicked, [this, w]() {
        const QString text = w->urlEdit->text().trimmed();
        if (text.isEmpty()) return;
        if (onImportModel) onImportModel(text.toStdString());
        Refresh();
    });

    // 渲染大小。
    QObject::connect(impl_->renderScaleSlider, &QSlider::valueChanged,
                     [this, w](int value) {
                         if (w->updating) return;
                         const float scale = static_cast<float>(value) / 100.0f;
                         if (config) config->renderScale = scale;
                         w->renderScaleValue->setText(
                             QString::number(scale, 'f', 2) + "×");
                         if (onConfigChanged) onConfigChanged();
                     });

    // 全屏追踪开关。
    QObject::connect(impl_->fullscreenChk, &QCheckBox::toggled,
                     [this, w](bool checked) {
                         if (w->updating) return;
                         if (config) config->fullscreenTracking = checked;
                         if (onConfigChanged) onConfigChanged();
                     });

    // 追踪半径。
    QObject::connect(impl_->radiusSlider, &QSlider::valueChanged,
                     [this, w](int value) {
                         if (w->updating) return;
                         if (config) config->trackingRadius = static_cast<float>(value);
                         w->radiusValue->setText(QString::number(value) + " px");
                         if (onConfigChanged) onConfigChanged();
                     });

    // 最大偏转角度。
    QObject::connect(impl_->angleSlider, &QSlider::valueChanged,
                     [this, w](int value) {
                         if (w->updating) return;
                         if (config) config->maxTrackingAngle = static_cast<float>(value);
                         w->angleValue->setText(QString::number(value) + "°");
                         if (onConfigChanged) onConfigChanged();
                     });

    // 窗口相关开关。
    QObject::connect(impl_->alwaysOnTopChk, &QCheckBox::toggled,
                     [this, w](bool checked) {
                         if (w->updating) return;
                         if (config) config->alwaysOnTop = checked;
                         if (onConfigChanged) onConfigChanged();
                     });
    QObject::connect(impl_->ignoreMouseChk, &QCheckBox::toggled,
                     [this, w](bool checked) {
                         if (w->updating) return;
                         if (config) config->ignoreMouseEvents = checked;
                         if (onConfigChanged) onConfigChanged();
                     });
    QObject::connect(impl_->showToolChk, &QCheckBox::toggled,
                     [this, w](bool checked) {
                         if (w->updating) return;
                         if (config) config->showTool = checked;
                         if (onConfigChanged) onConfigChanged();
                     });

    // 语言切换。
    QObject::connect(impl_->langCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [this, w](int index) {
                         if (w->updating) return;
                         if (config)
                             config->language =
                                 (index == 0) ? Language::Chinese : Language::English;
                         if (onConfigChanged) onConfigChanged();
                     });

    // 清除设置（带二次确认）。
    QObject::connect(impl_->clearBtn, &QPushButton::clicked, [this, w]() {
        QMessageBox::StandardButton reply = QMessageBox::question(
            w->dialog, ToQString(strings ? strings->clearSettings : "清除设置"),
            strings ? ToQString(strings->clearSettings + "？")
                    : "清除设置？",
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
        if (onClearSettings) onClearSettings();
        Refresh();
    });
}

void SettingsPanel::Refresh() {
    if (!impl_->dialog) return;
    const Strings empty;
    const Strings& s = strings ? *strings : empty;
    const AppConfig& cfg = config ? *config : AppConfig();
    auto* w = impl_.get();

    w->updating = true;  // 屏蔽因下面 setValue/setCurrentRow 触发的回调回路

    impl_->dialog->setWindowTitle(ToQString(s.settingsTitle));

    impl_->tabs->setTabText(kTabModel, ToQString(s.modelList));
    impl_->tabs->setTabText(kTabRender, ToQString(s.renderSize));
    impl_->tabs->setTabText(kTabWindow, ToQString(s.windowSection));
    impl_->tabs->setTabText(kTabLang, ToQString(s.language));
    impl_->tabs->setTabText(kTabAbout, ToQString(s.about));

    impl_->importBtn->setText(ToQString(s.importModel));
    impl_->removeBtn->setText(ToQString(s.removeModel));
    impl_->onlineUrlLabel->setText(ToQString(s.onlineModelUrl));
    impl_->importUrlBtn->setText(ToQString(s.importOnlineModel));
    impl_->onlineHintLabel->setText(ToQString(s.onlineModelHint));

    impl_->renderSizeLabel->setText(ToQString(s.renderSize));
    impl_->renderScaleSlider->setValue(
        static_cast<int>(cfg.renderScale * 100.0f + 0.5f));
    impl_->renderScaleValue->setText(
        QString::number(cfg.renderScale, 'f', 2) + "×");

    impl_->fullscreenChk->setText(ToQString(s.fullscreenTracking));
    impl_->fullscreenChk->setChecked(cfg.fullscreenTracking);
    impl_->trackingRadiusLabel->setText(ToQString(s.trackingRadius));
    impl_->radiusSlider->setValue(static_cast<int>(cfg.trackingRadius + 0.5f));
    impl_->radiusValue->setText(QString::number(static_cast<int>(cfg.trackingRadius + 0.5f)) + " px");
    impl_->maxAngleLabel->setText(ToQString(s.maxTrackingAngle));
    impl_->angleSlider->setValue(static_cast<int>(cfg.maxTrackingAngle + 0.5f));
    impl_->angleValue->setText(QString::number(static_cast<int>(cfg.maxTrackingAngle + 0.5f)) + "°");
    impl_->trackingHint->setText(ToQString(s.trackingHint));

    impl_->alwaysOnTopChk->setText(ToQString(s.alwaysOnTopLabel));
    impl_->alwaysOnTopChk->setChecked(cfg.alwaysOnTop);
    impl_->ignoreMouseChk->setText(ToQString(s.ignoreMouseLabel));
    impl_->ignoreMouseChk->setChecked(cfg.ignoreMouseEvents);
    impl_->showToolChk->setText(ToQString(s.showToolLabel));
    impl_->showToolChk->setChecked(cfg.showTool);

    impl_->langCombo->setCurrentIndex(cfg.language == Language::Chinese ? 0 : 1);

    impl_->aboutEdit->setPlainText(ToQString(s.aboutText));

    impl_->clearBtn->setText(ToQString(s.clearSettings));

    // 重建模型列表并高亮当前模型。
    impl_->modelList->clear();
    int currentRow = -1;
    for (std::size_t i = 0; i < cfg.modelList.size(); ++i) {
        impl_->modelList->addItem(ToQString(cfg.modelList[i]));
        if (cfg.modelList[i] == cfg.modelPath) currentRow = static_cast<int>(i);
    }
    if (currentRow >= 0) impl_->modelList->setCurrentRow(currentRow);

    w->updating = false;
}

void SettingsPanel::OpenImportDialog() {
    if (!impl_->dialog) return;
    const Strings empty;
    const Strings& s = strings ? *strings : empty;
    const QString filter = ToQString(
        s.modelFileFilter.empty()
            ? "Model files (*.json *.pmx *.vrm *.model3.json)"
            : s.modelFileFilter);
    const QString path = QFileDialog::getOpenFileName(
        impl_->dialog, ToQString(s.importModel), QString(), filter);
    if (!path.isEmpty()) {
        if (onImportModel) onImportModel(path.toStdString());
        Refresh();  // 模型列表可能因导入而新增
    }
}

void SettingsPanel::PumpEvents() {
    if (QApplication::instance()) {
        QApplication::processEvents(QEventLoop::AllEvents, 16);
    }
}

bool SettingsPanel::IsVisible() const {
    return impl_->dialog && impl_->dialog->isVisible();
}

void SettingsPanel::Show() {
    if (!impl_->dialog) return;
    impl_->dialog->show();
    impl_->dialog->raise();
    impl_->dialog->activateWindow();
}

void SettingsPanel::Hide() {
    if (impl_->dialog) impl_->dialog->hide();
}

void SettingsPanel::Toggle() {
    if (IsVisible())
        Hide();
    else
        Show();
}

QWidget* SettingsPanel::Handle() const { return impl_->dialog; }

void SettingsPanel::Shutdown() {
    if (impl_->dialog) {
        impl_->dialog->deleteLater();
        impl_->dialog = nullptr;
    }
}

}  // namespace ppet
