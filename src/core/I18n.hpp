// PPet - C++ 移植版
// 国际化：界面文案的中英文切换，替代原项目 tray.ts 中的 langs 常量。
#pragma once

#include <string>

#include "core/Config.hpp"

namespace ppet {

/// 界面文案集合。
struct Strings {
    // 托盘菜单
    std::string alwaysOnTop;
    std::string ignoreMouseEvents;
    std::string openAtLogin;
    std::string tools;
    std::string language;
    std::string settings;
    std::string prevModel;
    std::string nextModel;
    std::string reRender;
    std::string debug;
    std::string feedback;
    std::string about;
    std::string quit;

    // 设置界面
    std::string modelList;
    std::string importModel;
    std::string importOnlineModel;
    std::string onlineModelUrl;
    std::string onlineModelHint;
    std::string removeModel;
    std::string clearSettings;
    std::string renderSize;
    std::string fullscreenTracking;
    std::string trackingRadius;
    std::string maxTrackingAngle;
    std::string alwaysOnTopLabel;
    std::string ignoreMouseLabel;
    std::string showToolLabel;
    std::string windowSection;
    std::string settingsTitle;
    std::string noFontNote;

    // 提示信息
    std::string importFailed;
    std::string removeFailed;
    std::string modelLoadFailed;
    std::string selectModelFile;
    std::string importButton;
    std::string modelFileFilter;
    std::string aboutText;
    std::string modelTypeLive2D;
    std::string modelTypePmx;
    std::string modelTypeVrm;
    std::string noModel;
    std::string trackingHint;

    // 模型调试/诊断面板（DebugPanel）
    std::string debugPanel;
    std::string debugTitle;
    std::string debugTabOverview;
    std::string debugTabParams;
    std::string debugTabParts;
    std::string debugTabResources;
    std::string debugNoModel;
    std::string debugStatusOk;
    std::string debugStatusWarn;
    std::string debugStatusError;
    std::string debugCopy;
    std::string debugCopied;
    std::string debugLogHint;
    std::string debugAutoRefresh;
    std::string debugMissingNone;
    std::string debugMissingHint;
    std::string debugGroupMissing;
};

/// 按语言取文案。
Strings GetStrings(Language lang);

}  // namespace ppet
