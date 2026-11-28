// PPet - C++ 移植版
// VRM MToon 描边宽度单位换算（纯逻辑）。
//
// 【背景】VRM 0.x 与 1.0 对「描边宽度」的单位定义不同，混用会导致描边粗到
//   把模型本体整圈遮住（表现为模型外围一大圈深色轮廓）。
//   · VRM 0.x（extensions.VRM.materialProperties，UniVRM/MToon）：
//       world 模式下 _OutlineWidth 是美术面板数值，MToon shader 实际外扩
//       = _OutlineWidth * 0.1 米。
//   · VRM 1.0（VRMC_materials_mtoon）：
//       outlineWidthFactor 直接以「米」为单位，无额外系数。
//   若把 0.x 的 0.08 当 0.08 米用，1.67m 模型的描边会占 4.8%，直接遮住模型；
//   正确换算后是 0.008m（0.48%），才是正常动漫描边。
//
// 【为什么是纯逻辑】只做数值换算，不碰 GL / 文件 IO，可在无图形环境下单测；
//   解析层（GltfParser）调用本函数，保证 0.x 与 1.0 共用同一份换算规则。
#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>

namespace ppet::vrm {

/// VRM 0.x world 描边的换算系数。UniVRM/MToon 的规范值是 0.1，
/// 但桌宠窗口里模型占屏比例大、描边会被放大，用户反馈 0.1 换算后（0.08m，
/// 约占 1.67m 模型 0.48%）仍偏粗，故 PPet 取 0.05（约 0.24%），观感更精致。
/// 需要更粗时调大此值即可（0.1 = 规范值）。
constexpr float kVrm0OutlineScale = 0.05f;

/// 把 VRM 0.x 的 world 模式 _OutlineWidth 换算成世界单位（米）。
/// VRM 1.0 的 world 值本身就是米，原样返回。
///
/// @param factor      解析出的 _OutlineWidth / outlineWidthFactor 原始值。
/// @param widthMode   归一化后的模式字符串："worldCoordinates" / "screenCoordinates" / 其它。
/// @param isVrm0      是否 VRM 0.x（true 才应用换算系数）。
/// @return 以米为单位的描边宽度；非有限或负值返回 0。
inline float ResolveOutlineWidthMeters(float factor, std::string_view widthMode, bool isVrm0) {
    if (!std::isfinite(factor) || factor <= 0.0f) return 0.0f;
    // screenCoordinates 在 VrmModel 里按模型高度折算，解析层只保留原始比例，
    // 这里不重复换算（保持与旧行为一致）。
    if (widthMode == "screenCoordinates") return factor;
    if (widthMode != "worldCoordinates") return factor;
    // worldCoordinates：VRM 0.x 需要缩放换算到米；VRM 1.0 已是米。
    return isVrm0 ? factor * kVrm0OutlineScale : factor;
}

}  // namespace ppet::vrm
