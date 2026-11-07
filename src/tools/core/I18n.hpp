// PPet - C++ 移植版
// 国际化：界面文案的中英文切换，替代原项目 tray.ts 中的 langs 常量。
#pragma once

#include <string>

#include <src/tools/core/Config.hpp>

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
    std::string resetPosition;
    std::string moveWindow;
    std::string prevModel;
    std::string nextModel;
    std::string reRender;
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

    // 诊断面板扩展：骨架信息 / 图片渲染 / 详细错误
    std::string debugTabBones;
    std::string debugTabImages;
    std::string debugModelId;
    std::string debugNoBones;
    std::string debugTextureGroup;
    std::string debugRenderGroup;
    std::string debugNoPreview;
    std::string debugErrorGroup;
    std::string debugNoErrors;

    // 模型加载失败弹窗
    std::string errTitleFailed;
    std::string errTitleWarning;
    std::string errDetailGroup;
    std::string errAdvice;
    std::string errClose;
    std::string errLogHint;
    std::string errCopy;
    std::string errCopied;

    // 表情与语言功能（参考 VRM-VPet 的表情管理 / FULASKA 的 phrases+bubble+mouth）
    std::string sayPhrase;        ///< 托盘「说一句」
    std::string expressionMenu;   ///< 托盘「表情」子菜单
    std::string bubble;           ///< 「台词气泡」开关
    std::string debugTabExpression;  ///< 诊断面板「表情与说话」页签
    std::string expressionTitle;  ///< 表情标题
    std::string currentPhrase;    ///< 当前台词
    std::string lipSyncLabel;     ///< 口型
    std::string expressionStatusLabel;  ///< 表情状态
    std::string noExpressions;    ///< 该模型不支持表情

    // 表情预设名（托盘「表情」子菜单）。键与 ExpressionDirector 的预设 id 一致：
    // neutral / happy / relaxed / angry / sad / surprised。
    std::string expressionNeutral;
    std::string expressionHappy;
    std::string expressionRelaxed;
    std::string expressionAngry;
    std::string expressionSad;
    std::string expressionSurprised;
};

/// 按语言取文案。
Strings GetStrings(Language lang);

}  // namespace ppet
