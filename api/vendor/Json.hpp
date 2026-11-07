// PPet - C++ 移植版
// api/vendor：nlohmann/json 的统一引用点。
//
// 产品代码请 include 本头而不是直接 <nlohmann/json.hpp>：
// 将来换 JSON 实现时只改这里，调用方零改动。
#pragma once

#include <nlohmann/json.hpp>

namespace ppet::api {
using Json = nlohmann::json;
}  // namespace ppet::api
