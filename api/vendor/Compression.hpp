// PPet - C++ 移植版
// api/vendor：zlib 的统一引用点（ZIP 模型包解压）。
//
// ZIP 归档的中央目录解析在 src/tools/core/ZipArchive.* 自己实现，
// 只有 inflate 走 zlib；把 include 收敛到这里便于统一升级 vendored zlib。
#pragma once

#include <zlib/zlib.h>
