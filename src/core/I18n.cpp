// PPet - C++ 移植版
// 国际化文案实现。
#include "core/I18n.hpp"

namespace ppet {

Strings GetStrings(Language lang) {
    Strings s;

    if (lang == Language::Chinese) {
        s.alwaysOnTop = "@置顶";
        s.ignoreMouseEvents = "忽略点击";
        s.openAtLogin = "开机启动";
        s.tools = "小工具";
        s.language = "语言";
        s.settings = "配置";
        s.prevModel = "上一个模型";
        s.nextModel = "下一个模型";
        s.reRender = "重新渲染";
        s.debug = "调试";
        s.feedback = "反馈";
        s.about = "关于";
        s.quit = "退出";

        s.modelList = "模型列表";
        s.importModel = "导入模型";
        s.importOnlineModel = "导入在线模型";
        s.onlineModelUrl = "在线模型地址（URL）";
        s.onlineModelHint = "填入 .model3.json / .pmx / .vrm 的直链后点击导入";
        s.removeModel = "移除模型";
        s.clearSettings = "清除设置";
        s.renderSize = "渲染大小";
        s.fullscreenTracking = "全屏追踪";
        s.trackingRadius = "追踪半径";
        s.maxTrackingAngle = "最大偏转角度";
        s.alwaysOnTopLabel = "窗口置顶";
        s.ignoreMouseLabel = "忽略鼠标点击";
        s.showToolLabel = "显示工具栏";
        s.settingsTitle = "PPet 设置";
        s.noFontNote = "未找到中文字体，中文可能显示为方块";

        s.importFailed = "导入模型失败";
        s.removeFailed = "移除模型失败";
        s.modelLoadFailed = "模型加载失败";
        s.selectModelFile = "请选择模型配置文件";
        s.importButton = "导入模型";
        s.modelFileFilter = "模型文件 (*.json *.pmx *.vrm *.model3.json)";
        s.aboutText = "PPet - 桌面宠物（C++ 移植版）\n支持 Live2D / PMX / VRM 模型";
        s.modelTypeLive2D = "Live2D";
        s.modelTypePmx = "PMX";
        s.modelTypeVrm = "VRM";
        s.noModel = "尚未加载模型";
        s.trackingHint = "开启后，模型的眼睛与朝向会跟随整个屏幕范围内的鼠标位置";
    } else {
        s.alwaysOnTop = "Always On Top";
        s.ignoreMouseEvents = "Ignore Mouse Events";
        s.openAtLogin = "Open At Login";
        s.tools = "Tools";
        s.language = "Language";
        s.settings = "Settings";
        s.prevModel = "Prev Model";
        s.nextModel = "Next Model";
        s.reRender = "ReRender";
        s.debug = "Debug";
        s.feedback = "Feedback";
        s.about = "About";
        s.quit = "Quit";

        s.modelList = "Model List";
        s.importModel = "Import Model";
        s.importOnlineModel = "Import Online Model";
        s.onlineModelUrl = "Online Model URL";
        s.onlineModelHint = "Paste a direct link to .model3.json / .pmx / .vrm, then import";
        s.removeModel = "Remove Model";
        s.clearSettings = "Clear Canvas Settings";
        s.renderSize = "Render Size";
        s.fullscreenTracking = "Full-screen Tracking";
        s.trackingRadius = "Tracking Radius";
        s.maxTrackingAngle = "Max Tracking Angle";
        s.alwaysOnTopLabel = "Always On Top";
        s.ignoreMouseLabel = "Ignore Mouse Events";
        s.showToolLabel = "Show Toolbar";
        s.settingsTitle = "PPet Settings";
        s.noFontNote = "No CJK font found; Chinese text may show as boxes";

        s.importFailed = "Import model failed";
        s.removeFailed = "Remove model failed";
        s.modelLoadFailed = "Failed to load model";
        s.selectModelFile = "Please select a model configuration file";
        s.importButton = "Import model";
        s.modelFileFilter = "Model files (*.json *.pmx *.vrm *.model3.json)";
        s.aboutText = "PPet - Desktop Pet (C++ port)\nSupports Live2D / PMX / VRM models";
        s.modelTypeLive2D = "Live2D";
        s.modelTypePmx = "PMX";
        s.modelTypeVrm = "VRM";
        s.noModel = "No model loaded";
        s.trackingHint =
            "When enabled, the model's eyes and head follow the mouse across the entire screen";
    }

    return s;
}

}  // namespace ppet
