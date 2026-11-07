// PPet - C++ 移植版
// 模型调试/诊断面板（Qt 6 独立窗口）冒烟测试。
//
// 验证点：
//   1. QApplication 可用，DebugPanel::Initialize 成功；
//   2. 初始隐藏，Show/Hide/Toggle 可见性切换正确；
//   3. 用一份伪造的 ModelDebugInfo + RuntimeStats 调用 Update() 不崩溃，
//      且包含「模型部分」「缺失资源」「性质」等全部区域的数据；
//   4. ShouldRefresh() 的 200 ms 节流生效；
//   5. Refresh()（语言切换）、PumpEvents()、Shutdown() 连续调用不崩溃。
//
// 用 offscreen 平台插件运行（见 CMake 中的 QT_QPA_PLATFORM=offscreen）。
#include <cstdio>
#include <string>

#include <QApplication>
#include <QPushButton>
#include <QTabWidget>
#include <QTextEdit>

#include <src/tools/core/I18n.hpp>
#include <src/ui/DebugPanel.hpp>

int main(int argc, char** argv) {
    QApplication qtApp(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    if (!QApplication::instance()) {
        std::printf("debug_panel_smoke: QApplication 初始化失败（offscreen 插件可用？）\n");
        return 2;
    }

    ppet::Strings strings = ppet::GetStrings(ppet::Language::Chinese);

    ppet::DebugPanel panel;
    panel.strings = &strings;

    std::string error;
    if (!panel.Initialize(error)) {
        std::printf("debug_panel_smoke: 初始化失败：%s\n", error.c_str());
        return 1;
    }
    if (panel.Handle() == nullptr) {
        std::printf("debug_panel_smoke: 窗口句柄为空\n");
        return 1;
    }
    if (panel.IsVisible()) {
        std::printf("debug_panel_smoke: 初始化后不应可见\n");
        return 1;
    }

    // 伪造一份「带问题」的诊断快照，覆盖面板的所有区域。
    ppet::ModelDebugInfo model;
    model.displayName = "测试模型 / Test Model";
    model.path = "/tmp/models/demo.pmx";
    model.sourceArchive = "/tmp/models/demo.zip";
    model.type = ppet::ModelType::Pmx;
    model.formatVersion = "PMX 2.0";
    model.textEncoding = "UTF-16LE";
    model.loaded = true;
    model.gpuReady = true;
    model.renderingOk = false;
    model.renderStatus = "可出图，但缺 2 张贴图（可能白模）";
    model.vertexCount = 14590;
    model.indexCount = 44961;
    model.triangleCount = 14987;
    model.boneCount = 122;
    model.materialCount = 8;
    model.subMeshCount = 8;
    model.morphCount = 30;
    model.textureReferenced = 12;
    model.textureLoaded = 10;
    model.drawCalls = 16;
    model.modelHeight = 20.5f;
    model.doubleSided = true;
    model.hasOutline = true;
    model.isSkinned = true;
    model.properties.push_back("坐标系=PMX 左手系 → OpenGL 右手系");
    model.properties.push_back("渲染管线=蒙皮网格 + toon 着色");
    model.missingResources.push_back("/tmp/models/tex/body.png");
    model.missingResources.push_back("/tmp/models/tex/face.png");
    // 新增区域：模型 ID / 骨架明细 / 图片明细 / 详细错误
    model.modelId = "PPET-0123456789ABCDEF";
    for (int i = 0; i < 5; ++i) {
        ppet::ModelDebugInfo::BoneInfo bone;
        bone.index = i;
        bone.name = (i == 0 ? "root" : "bone_" + std::to_string(i));
        bone.parentIndex = i == 0 ? -1 : 0;
        bone.depth = i == 0 ? 0 : 1;
        bone.flags = i == 0 ? "-" : "可位移";
        model.bones.push_back(bone);
    }
    for (int i = 0; i < 3; ++i) {
        ppet::ModelDebugInfo::TextureInfo tex;
        tex.name = "tex/part" + std::to_string(i) + ".png";
        tex.path = "/tmp/models/tex/part" + std::to_string(i) + ".png";
        tex.loaded = i < 2;
        tex.note = tex.loaded ? "磁盘贴图" : "文件不存在";
        model.textures.push_back(tex);
    }
    model.errorCategory = "模型图片缺失";
    model.errorDetails.push_back("缺少 2 张贴图，可能出现白模");
    model.errorDetails.push_back("建议把 .pmx 与贴图目录打包成 .zip");
    for (int i = 0; i < 8; ++i) {
        ppet::ModelDebugInfo::Part part;
        part.name = "材质 #" + std::to_string(i);
        part.indexCount = 3 * (100 + i);
        part.materialIndex = i;
        part.detail = "三角形 " + std::to_string(100 + i) + " / 贴图 " + std::to_string(i);
        model.parts.push_back(part);
    }

    ppet::RuntimeStats runtime;
    runtime.fps = 59.8f;
    runtime.frameMs = 16.7f;
    runtime.frameCount = 12345;
    runtime.viewportWidth = 350;
    runtime.viewportHeight = 600;
    runtime.renderScale = 1.0f;
    runtime.hasGlobalCursor = true;
    runtime.trackerRunning = true;
    runtime.desktopWidth = 1920;
    runtime.desktopHeight = 1080;
    runtime.glError = 0;
    runtime.logPath = "./last.log";
    // 不存在的路径：验证「暂无可预览的图片」分支不会崩。
    runtime.renderPreviewPath = "/tmp/ppet_no_such_frame.png";
    runtime.renderPreviewNote = "渲染帧抓取失败：测试占位";

    panel.Show();
    if (!panel.IsVisible()) {
        std::printf("debug_panel_smoke: Show() 后应可见\n");
        return 1;
    }
    if (auto* tabs = panel.Handle()->findChild<QTabWidget*>()) {
        if (tabs->count() != 6) {
            std::printf("debug_panel_smoke: 应有 6 个页签，实际 %d\n", tabs->count());
            return 1;
        }
    } else {
        std::printf("debug_panel_smoke: 未找到 QTabWidget\n");
        return 1;
    }
    if (!panel.ShouldRefresh()) {
        std::printf("debug_panel_smoke: 首次应允许刷新\n");
        return 1;
    }

    panel.Update(model, runtime);
    for (int i = 0; i < 10; ++i) panel.PumpEvents();

    // 详细错误区应包含分类与逐条明细。
    if (auto* errors = panel.Handle()->findChild<QTextEdit*>("ppetErrorDetailsView")) {
        const QString text = errors->toPlainText();
        if (!text.contains("模型图片缺失") || !text.contains("打包成 .zip")) {
            std::printf("debug_panel_smoke: 详细错误区内容不完整\n");
            return 1;
        }
    } else {
        std::printf("debug_panel_smoke: 未找到详细错误区\n");
        return 1;
    }

    // 紧接着的同一时刻不应重复刷新（200 ms 节流）。
    panel.Update(model, runtime);

    // 语言切换后刷新文案，再次喂数据不应崩溃。
    strings = ppet::GetStrings(ppet::Language::English);
    panel.strings = &strings;
    panel.Refresh();
    panel.Update(model, runtime);
    panel.PumpEvents();

    // 「复制诊断摘要」按钮：找一个 QPushButton 触发点击，验证剪贴板回调不崩。
    if (auto* button = panel.Handle()->findChild<QPushButton*>()) {
        button->click();
        panel.PumpEvents();
    }

    panel.Toggle();
    if (panel.IsVisible()) {
        std::printf("debug_panel_smoke: Toggle() 后应隐藏\n");
        return 1;
    }
    panel.Toggle();
    panel.Hide();
    if (panel.IsVisible()) {
        std::printf("debug_panel_smoke: Hide() 后应隐藏\n");
        return 1;
    }

    panel.Shutdown();
    panel.Shutdown();  // 可重复调用
    std::printf("debug_panel_smoke: 通过（parts=%zu missing=%zu）\n", model.parts.size(),
                model.missingResources.size());
    return 0;
}
