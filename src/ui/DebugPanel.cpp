// PPet - C++ 移植版
// 模型调试/诊断面板实现。详见 DebugPanel.hpp。
#include "ui/DebugPanel.hpp"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <utility>
#include <vector>

#include <QApplication>
#include <QAbstractItemView>
#include <QClipboard>
#include <QDialog>
#include <QEventLoop>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace ppet {
namespace {

QString ToQString(const std::string& s) { return QString::fromStdString(s); }

/// HTML 转义：模型路径/名称里可能有 & < >，直接拼进 HTML 会破坏结构。
std::string EscapeHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

/// 用两列表格渲染一组「字段: 值」，比纯文本对齐更可靠（中英文宽度不同）。
std::string HtmlRows(const std::vector<std::pair<std::string, std::string>>& rows) {
    std::string html = "<table width='100%' cellpadding='2' cellspacing='0'>";
    for (const auto& [key, value] : rows) {
        html += "<tr><td style='color:#777;white-space:nowrap;padding-right:12px;'>" +
                EscapeHtml(key) + "</td><td style='white-space:nowrap;'>" +
                EscapeHtml(value) + "</td></tr>";
    }
    html += "</table>";
    return html;
}

std::string HtmlSection(const std::string& title,
                        const std::vector<std::pair<std::string, std::string>>& rows) {
    return "<h3 style='margin:10px 0 4px 0;'>" + EscapeHtml(title) + "</h3>" + HtmlRows(rows);
}

std::string Vec3Text(const Vec3& v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%.3f, %.3f, %.3f)", static_cast<double>(v.x),
                  static_cast<double>(v.y), static_cast<double>(v.z));
    return buf;
}

std::string Vec2Text(const Vec2& v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.1f, %.1f)", static_cast<double>(v.x),
                  static_cast<double>(v.y));
    return buf;
}

/// 只读、等宽字体的 HTML 视图。
QTextEdit* MakeReportView(QWidget* parent) {
    auto* view = new QTextEdit(parent);
    view->setReadOnly(true);
    view->setLineWrapMode(QTextEdit::NoWrap);
    QFont font("Monospace");
    font.setStyleHint(QFont::TypeWriter);
    font.setPointSize(9);
    view->setFont(font);
    return view;
}

enum TabIndex {
    kTabOverview = 0,
    kTabParams = 1,
    kTabParts = 2,
    kTabResources = 3,
};

}  // namespace

struct DebugPanel::Impl {
    QDialog* dialog = nullptr;
    QTabWidget* tabs = nullptr;

    QTextEdit* overviewView = nullptr;
    QTextEdit* paramsView = nullptr;
    QTableWidget* partsTable = nullptr;
    QTextEdit* propertiesView = nullptr;
    QTableWidget* missingTable = nullptr;
    QLabel* logLabel = nullptr;
    QLabel* hintLabel = nullptr;
    QLabel* groupMissingLabel = nullptr;
    QPushButton* copyBtn = nullptr;

    // 节流与增量重建
    std::chrono::steady_clock::time_point lastUpdate{};
    std::string lastModelKey;
    ModelDebugInfo lastModel;
    RuntimeStats lastRuntime;
    bool everUpdated = false;
};

DebugPanel::DebugPanel() : impl_(std::make_unique<Impl>()) {}
DebugPanel::~DebugPanel() { Shutdown(); }

void DebugPanel::BuildUi() {
    QDialog* dialog = impl_->dialog;
    const Strings empty;
    const Strings& s = strings ? *strings : empty;

    auto* root = new QVBoxLayout(dialog);
    impl_->tabs = new QTabWidget(dialog);
    root->addWidget(impl_->tabs);

    // --- Tab 1：概览与渲染 ---
    {
        auto* page = new QWidget(dialog);
        auto* layout = new QVBoxLayout(page);
        impl_->overviewView = MakeReportView(page);
        layout->addWidget(impl_->overviewView);
        impl_->tabs->addTab(page, ToQString(s.debugTabOverview));
    }

    // --- Tab 2：参数 ---
    {
        auto* page = new QWidget(dialog);
        auto* layout = new QVBoxLayout(page);
        impl_->paramsView = MakeReportView(page);
        layout->addWidget(impl_->paramsView);
        impl_->tabs->addTab(page, ToQString(s.debugTabParams));
    }

    // --- Tab 3：模型部分（子网格 / 材质）---
    {
        auto* page = new QWidget(dialog);
        auto* layout = new QVBoxLayout(page);
        impl_->partsTable = new QTableWidget(page);
        impl_->partsTable->setColumnCount(4);
        impl_->partsTable->setHorizontalHeaderLabels(
            {"名称 Name", "三角形 Triangles", "材质 Material", "详情 Details"});
        impl_->partsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        impl_->partsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        impl_->partsTable->setAlternatingRowColors(true);
        impl_->partsTable->verticalHeader()->setVisible(false);
        impl_->partsTable->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(impl_->partsTable);
        impl_->tabs->addTab(page, ToQString(s.debugTabParts));
    }

    // --- Tab 4：性质与资源 ---
    {
        auto* page = new QWidget(dialog);
        auto* layout = new QVBoxLayout(page);

        impl_->propertiesView = MakeReportView(page);
        impl_->propertiesView->setMaximumHeight(230);
        layout->addWidget(impl_->propertiesView);

        impl_->groupMissingLabel = new QLabel(ToQString(s.debugGroupMissing), page);
        layout->addWidget(impl_->groupMissingLabel);
        impl_->missingTable = new QTableWidget(page);
        impl_->missingTable->setColumnCount(1);
        impl_->missingTable->setHorizontalHeaderLabels({"资源路径 Resource path"});
        impl_->missingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        impl_->missingTable->verticalHeader()->setVisible(false);
        impl_->missingTable->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(impl_->missingTable);

        impl_->logLabel = new QLabel(page);
        impl_->logLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(impl_->logLabel);

        impl_->hintLabel = new QLabel(ToQString(s.debugAutoRefresh), page);
        impl_->hintLabel->setWordWrap(true);
        impl_->hintLabel->setStyleSheet("color:#777;");
        layout->addWidget(impl_->hintLabel);

        auto* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        impl_->copyBtn = new QPushButton(ToQString(s.debugCopy), page);
        buttons->addWidget(impl_->copyBtn);
        layout->addLayout(buttons);

        QObject::connect(impl_->copyBtn, &QPushButton::clicked, [this]() {
            // 把当前快照拼成纯文本，方便贴到 issue 里。
            std::ostringstream os;
            const ModelDebugInfo& m = impl_->lastModel;
            const RuntimeStats& r = impl_->lastRuntime;
            os << "PPet 模型诊断摘要\n"
               << "================\n"
               << "模型: " << m.displayName << "\n"
               << "类型: " << ModelTypeName(m.type) << " / " << m.formatVersion << "\n"
               << "路径: " << m.path << "\n"
               << "来源: " << (m.sourceArchive.empty() ? "单文件" : m.sourceArchive) << "\n"
               << "状态: " << m.renderStatus << "\n"
               << "渲染正常: " << (m.renderingOk ? "是" : "否") << "\n"
               << "GL 错误: " << r.glError << "\n"
               << "顶点/三角形/骨骼/材质/子网格: " << m.vertexCount << " / "
               << m.triangleCount << " / " << m.boneCount << " / " << m.materialCount << " / "
               << m.subMeshCount << "\n"
               << "贴图(引用/已加载/缺失): " << m.textureReferenced << " / "
               << m.textureLoaded << " / " << m.missingResources.size() << "\n"
               << "FPS/帧时/帧数: " << r.fps << " / " << r.frameMs << " ms / "
               << r.frameCount << "\n"
               << "视口: " << r.viewportWidth << "x" << r.viewportHeight << "\n"
               << "性质:\n";
            for (const std::string& p : m.properties) os << "  - " << p << "\n";
            if (!m.missingResources.empty()) {
                os << "缺失资源:\n";
                for (const std::string& p : m.missingResources) os << "  - " << p << "\n";
            }
            if (QClipboard* clipboard = QApplication::clipboard()) {
                clipboard->setText(QString::fromStdString(os.str()));
            }
            if (onCopyDiagnostics) onCopyDiagnostics();
            const Strings emptyStrings;
            const Strings& s2 = strings ? *strings : emptyStrings;
            impl_->copyBtn->setText(ToQString(s2.debugCopied));
            QTimer::singleShot(1500, impl_->copyBtn, [this]() {
                const Strings emptyAgain;
                const Strings& s3 = strings ? *strings : emptyAgain;
                if (impl_->copyBtn) impl_->copyBtn->setText(ToQString(s3.debugCopy));
            });
        });

        impl_->tabs->addTab(page, ToQString(s.debugTabResources));
    }

    dialog->setLayout(root);
}

bool DebugPanel::Initialize(std::string& error) {
    if (impl_->dialog) return true;
    if (!QApplication::instance()) {
        error = "QApplication 尚未创建，无法创建调试面板";
        return false;
    }

    const Strings empty;
    const Strings& s = strings ? *strings : empty;

    auto* dialog = new QDialog(nullptr);
    dialog->setWindowTitle(ToQString(s.debugTitle.empty() ? std::string("PPet 模型诊断")
                                                         : s.debugTitle));
    dialog->setMinimumSize(620, 480);
    dialog->resize(780, 600);
    dialog->setAttribute(Qt::WA_QuitOnClose, false);
    dialog->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    impl_->dialog = dialog;

    BuildUi();
    impl_->lastUpdate = std::chrono::steady_clock::now();
    return true;
}

void DebugPanel::Shutdown() {
    if (!impl_) return;
    if (impl_->dialog) {
        impl_->dialog->deleteLater();
        impl_->dialog = nullptr;
    }
    impl_->tabs = nullptr;
    impl_->overviewView = nullptr;
    impl_->paramsView = nullptr;
    impl_->partsTable = nullptr;
    impl_->propertiesView = nullptr;
    impl_->missingTable = nullptr;
    impl_->logLabel = nullptr;
    impl_->hintLabel = nullptr;
    impl_->groupMissingLabel = nullptr;
    impl_->copyBtn = nullptr;
    impl_->lastModelKey.clear();
    impl_->everUpdated = false;
}

void DebugPanel::PumpEvents() {
    if (QApplication::instance()) {
        QApplication::processEvents(QEventLoop::AllEvents, 16);
    }
}

bool DebugPanel::IsVisible() const {
    return impl_ && impl_->dialog && impl_->dialog->isVisible();
}

bool DebugPanel::ShouldRefresh() const {
    if (!IsVisible()) return false;
    if (!impl_->everUpdated) return true;
    return std::chrono::steady_clock::now() - impl_->lastUpdate >=
           std::chrono::milliseconds(200);
}

void DebugPanel::Show() {
    if (!impl_ || !impl_->dialog) return;
    impl_->dialog->show();
    impl_->dialog->raise();
    impl_->dialog->activateWindow();
    // 立刻刷新一次，避免打开后要等一个节流周期才看到内容。
    impl_->lastUpdate = std::chrono::steady_clock::time_point{};
}

void DebugPanel::Hide() {
    if (impl_ && impl_->dialog) impl_->dialog->hide();
}

void DebugPanel::Toggle() {
    if (IsVisible()) Hide();
    else Show();
}

QWidget* DebugPanel::Handle() const { return impl_ ? impl_->dialog : nullptr; }

void DebugPanel::Refresh() {
    if (!impl_ || !impl_->tabs) return;
    const Strings empty;
    const Strings& s = strings ? *strings : empty;
    impl_->dialog->setWindowTitle(ToQString(s.debugTitle.empty() ? std::string("PPet 模型诊断")
                                                                 : s.debugTitle));
    impl_->tabs->setTabText(kTabOverview, ToQString(s.debugTabOverview));
    impl_->tabs->setTabText(kTabParams, ToQString(s.debugTabParams));
    impl_->tabs->setTabText(kTabParts, ToQString(s.debugTabParts));
    impl_->tabs->setTabText(kTabResources, ToQString(s.debugTabResources));
    if (impl_->copyBtn) impl_->copyBtn->setText(ToQString(s.debugCopy));
    if (impl_->hintLabel) impl_->hintLabel->setText(ToQString(s.debugAutoRefresh));
    if (impl_->groupMissingLabel) {
        impl_->groupMissingLabel->setText(ToQString(s.debugGroupMissing));
    }
    impl_->everUpdated = false;  // 强制下一帧重刷（含文案）
}

void DebugPanel::Update(const ModelDebugInfo& model, const RuntimeStats& runtime) {
    if (!impl_ || !impl_->dialog || !impl_->dialog->isVisible()) return;

    const auto now = std::chrono::steady_clock::now();
    if (impl_->everUpdated &&
        now - impl_->lastUpdate < std::chrono::milliseconds(200)) {
        return;
    }
    impl_->lastUpdate = now;
    impl_->everUpdated = true;
    impl_->lastModel = model;
    impl_->lastRuntime = runtime;

    const Strings empty;
    const Strings& s = strings ? *strings : empty;

    // --- 状态灯颜色与结论 ---
    enum class State { Ok, Warn, Error, Idle };
    State state = State::Idle;
    if (!model.loaded) {
        state = State::Idle;
    } else if (!model.renderingOk || runtime.glError != 0) {
        state = State::Error;
    } else if (!model.missingResources.empty() ||
               (model.textureReferenced > 0 && model.textureLoaded < model.textureReferenced)) {
        state = State::Warn;
    } else {
        state = State::Ok;
    }
    const char* color = state == State::Ok      ? "#2e7d32"
                        : state == State::Warn  ? "#f9a825"
                        : state == State::Error ? "#c62828"
                                                : "#888888";
    const std::string stateText =
        !model.loaded ? s.debugNoModel
        : state == State::Ok     ? s.debugStatusOk
        : state == State::Warn   ? s.debugStatusWarn
                                 : s.debugStatusError;

    // --- Tab 1：概览与渲染 ---
    {
        std::vector<std::pair<std::string, std::string>> rows;
        rows.emplace_back("模型名 Name", model.displayName.empty() ? "-" : model.displayName);
        rows.emplace_back("类型 Type", std::string(ModelTypeName(model.type)) +
                                          (model.formatVersion.empty()
                                               ? ""
                                               : " / " + model.formatVersion));
        rows.emplace_back("路径 Path", model.path.empty() ? "-" : model.path);
        rows.emplace_back("来源 Source", model.sourceArchive.empty()
                                            ? "单文件 / Single file"
                                            : "ZIP → " + model.sourceArchive);
        rows.emplace_back("加载 Loaded", model.loaded ? "是 / Yes" : "否 / No");
        rows.emplace_back("结论 Status", model.renderStatus.empty() ? "-" : model.renderStatus);

        std::vector<std::pair<std::string, std::string>> renderRows;
        renderRows.emplace_back("GPU 就绪", model.gpuReady ? "是" : "否（等待首帧）");
        renderRows.emplace_back("GL 错误", runtime.glError == 0
                                              ? "0（无）"
                                              : std::to_string(runtime.glError));
        renderRows.emplace_back("Draw calls / 帧", std::to_string(model.drawCalls));
        renderRows.emplace_back("贴图 引用/已加载/缺失",
                                std::to_string(model.textureReferenced) + " / " +
                                    std::to_string(model.textureLoaded) + " / " +
                                    std::to_string(model.missingResources.size()));
        renderRows.emplace_back("FPS / 帧时",
                                std::to_string(static_cast<int>(runtime.fps)) + " / " +
                                    std::to_string(static_cast<int>(runtime.frameMs)) + " ms");
        renderRows.emplace_back("帧数 Frames", std::to_string(runtime.frameCount));
        renderRows.emplace_back("视口 Viewport", std::to_string(runtime.viewportWidth) + " x " +
                                                    std::to_string(runtime.viewportHeight));
        renderRows.emplace_back("渲染缩放 Scale", std::to_string(runtime.renderScale));
        renderRows.emplace_back("桌面 Desktop", std::to_string(runtime.desktopWidth) + " x " +
                                                    std::to_string(runtime.desktopHeight));
        renderRows.emplace_back("全屏追踪 Tracker", runtime.trackerRunning ? "运行中" : "关闭");
        renderRows.emplace_back("全局光标 Cursor", runtime.hasGlobalCursor
                                                      ? Vec2Text(runtime.globalCursor)
                                                      : "不可用");
        renderRows.emplace_back("局部光标 Local", Vec2Text(runtime.localCursor));

        std::string html = "<p style='font-size:14px;'><span style='color:" +
                           std::string(color) + ";'>●</span> <b>" + EscapeHtml(stateText) +
                           "</b></p>";
        html += HtmlSection("模型概览 / Overview", rows);
        html += HtmlSection("渲染状态 / Render status", renderRows);
        impl_->overviewView->setHtml(ToQString(html));
    }

    // --- Tab 2：参数 ---
    {
        std::vector<std::pair<std::string, std::string>> rows;
        rows.emplace_back("顶点数 Vertices", std::to_string(model.vertexCount));
        rows.emplace_back("索引数 Indices", std::to_string(model.indexCount));
        rows.emplace_back("三角形 Triangles", std::to_string(model.triangleCount));
        rows.emplace_back("骨骼 Bones", std::to_string(model.boneCount));
        rows.emplace_back("材质 Materials", std::to_string(model.materialCount));
        rows.emplace_back("子网格 SubMeshes", std::to_string(model.subMeshCount));
        rows.emplace_back("Morph / 表情 Morphs", std::to_string(model.morphCount));
        rows.emplace_back("贴图引用 Textures", std::to_string(model.textureReferenced));
        rows.emplace_back("贴图已加载 Loaded", std::to_string(model.textureLoaded));
        rows.emplace_back("缺失资源 Missing", std::to_string(model.missingResources.size()));
        rows.emplace_back("包围盒最小 BoundsMin", Vec3Text(model.boundsMin));
        rows.emplace_back("包围盒最大 BoundsMax", Vec3Text(model.boundsMax));
        rows.emplace_back("模型高度 Height", std::to_string(model.modelHeight));
        rows.emplace_back("骨骼蒙皮 Skinned", model.isSkinned ? "是 / Yes" : "否 / No");
        rows.emplace_back("双面材质 DoubleSided", model.doubleSided ? "有" : "无");
        rows.emplace_back("描边 Outline", model.hasOutline ? "有" : "无");
        impl_->paramsView->setHtml(ToQString(HtmlRows(rows)));
    }

    // --- Tab 3/4 的列表只在模型变化时重建，避免每 200 ms 闪烁 ---
    const std::string key = model.path + "|" + std::to_string(model.parts.size()) + "|" +
                            std::to_string(model.materialCount) + "|" +
                            std::to_string(model.missingResources.size());
    if (key == impl_->lastModelKey) return;
    impl_->lastModelKey = key;

    {
        QTableWidget* table = impl_->partsTable;
        table->setRowCount(static_cast<int>(model.parts.size()));
        for (std::size_t i = 0; i < model.parts.size(); ++i) {
            const ModelDebugInfo::Part& part = model.parts[i];
            table->setItem(static_cast<int>(i), 0,
                           new QTableWidgetItem(ToQString(part.name)));
            table->setItem(static_cast<int>(i), 1,
                           new QTableWidgetItem(
                               ToQString(std::to_string(part.indexCount / 3))));
            table->setItem(static_cast<int>(i), 2,
                           new QTableWidgetItem(
                               ToQString(std::to_string(part.materialIndex))));
            table->setItem(static_cast<int>(i), 3,
                           new QTableWidgetItem(ToQString(part.detail)));
        }
        table->resizeColumnsToContents();
        if (model.parts.empty()) {
            table->setRowCount(1);
            table->setItem(0, 0, new QTableWidgetItem(ToQString(s.debugNoModel)));
        }
    }

    {
        std::vector<std::pair<std::string, std::string>> rows;
        rows.emplace_back("格式版本 Format", model.formatVersion.empty() ? "-" : model.formatVersion);
        rows.emplace_back("文本编码 Encoding", model.textEncoding.empty() ? "-" : model.textEncoding);
        rows.emplace_back("渲染正常", model.renderingOk ? "是 / Yes" : "否 / No");
        for (const std::string& p : model.properties) {
            const std::size_t eq = p.find('=');
            if (eq == std::string::npos) {
                rows.emplace_back("性质", p);
            } else {
                rows.emplace_back(p.substr(0, eq), p.substr(eq + 1));
            }
        }
        impl_->propertiesView->setHtml(ToQString(HtmlRows(rows)));

        QTableWidget* table = impl_->missingTable;
        if (model.missingResources.empty()) {
            table->setRowCount(1);
            table->setItem(0, 0, new QTableWidgetItem(ToQString(s.debugMissingNone)));
        } else {
            table->setRowCount(static_cast<int>(model.missingResources.size()));
            for (std::size_t i = 0; i < model.missingResources.size(); ++i) {
                table->setItem(static_cast<int>(i), 0,
                               new QTableWidgetItem(ToQString(model.missingResources[i])));
            }
            table->setToolTip(ToQString(s.debugMissingHint));
        }
        table->resizeColumnsToContents();
    }

    // --- 日志位置 ---
    {
        std::string logLine = s.debugLogHint + (runtime.logPath.empty() ? "./last.log"
                                                                       : runtime.logPath);
        impl_->logLabel->setText(ToQString(logLine));
    }
}

}  // namespace ppet
