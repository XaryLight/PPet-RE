// PPet - C++ 移植版
// glTF 2.0 / GLB 容器解析器实现。
//
// 关于 GLB 容器：
//   布局为 magic("glTF") + version(2) + length + 若干 chunk，
//   每个 chunk 为 chunkLength + chunkType + chunkData。
//   chunkType 为 0x4E4F534A("JSON") 与 0x004E4942("BIN")。
//   JSON chunk 末尾用空格(0x20)补齐、最后补一个 0x00；
//   BIN chunk 末尾用 0x00 补齐。解析 JSON 前必须先去掉这些填充字节，
//   否则 nlohmann 会因为尾随的 0x00 而报错。
//
// 关于坐标与矩阵：
//   glTF 的 matrix 是列主序 16 个浮点数，平移在 [12..14]，
//   与 core/Math.hpp 中 Mat4（m[col * 4 + row]）的内存布局完全一致，
//   因此矩阵可以直接拷贝，不需要转置。
#include "model/vrm/GltfParser.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ppet::vrm {
namespace {

// ---------------------------------------------------------------------------
// 上限常量：防止恶意 / 损坏文件触发超大分配或超长循环。
// ---------------------------------------------------------------------------
constexpr std::size_t kMaxFileSize = 512ull * 1024 * 1024;   ///< 512 MB
constexpr std::size_t kMaxJsonSize = 256ull * 1024 * 1024;    ///< 256 MB
constexpr std::size_t kMaxAccessorCount = 10000000;
constexpr std::size_t kMaxBufferCount = 1024;
constexpr std::size_t kMaxNodeCount = 1000000;
constexpr std::size_t kMaxMeshCount = 100000;
constexpr std::size_t kMaxMaterialCount = 100000;
constexpr std::size_t kMaxTextureCount = 100000;
constexpr std::size_t kMaxImageCount = 100000;
constexpr std::size_t kMaxSkinCount = 100000;
constexpr std::size_t kMaxAnimationCount = 10000;
constexpr std::size_t kMaxPrimitiveCount = 100000;
constexpr std::size_t kMaxMorphTargetCount = 4096;
constexpr std::size_t kMaxJointCount = 100000;

/// GLB 文件头 magic（"glTF" 的小端表示）。
constexpr std::uint32_t kGlbMagic = 0x46546C67u;
/// GLB chunk 类型。
constexpr std::uint32_t kChunkJson = 0x4E4F534Au;
constexpr std::uint32_t kChunkBin = 0x004E4942u;

// ---------------------------------------------------------------------------
// JSON 取值辅助：全部做类型检查，缺省或类型不符时返回默认值。
// ---------------------------------------------------------------------------

bool JsonToFloat(const nlohmann::json& v, float& out) {
    if (!v.is_number()) return false;
    const double d = v.get<double>();
    // NaN / Inf 会污染后续所有矩阵运算，直接丢弃。
    if (!std::isfinite(d)) return false;
    out = static_cast<float>(d);
    return true;
}

bool JsonToInt(const nlohmann::json& v, int& out) {
    if (!v.is_number()) return false;
    const double d = v.get<double>();
    if (!std::isfinite(d)) return false;
    if (d > 2147483647.0) {
        out = 2147483647;
    } else if (d < -2147483648.0) {
        out = -2147483648;
    } else {
        out = static_cast<int>(d);
    }
    return true;
}

int GetIntOr(const nlohmann::json& obj, const char* key, int def) {
    if (!obj.is_object()) return def;
    const auto it = obj.find(key);
    if (it == obj.end()) return def;
    int v = def;
    if (!JsonToInt(*it, v)) return def;
    return v;
}

float GetFloatOr(const nlohmann::json& obj, const char* key, float def) {
    if (!obj.is_object()) return def;
    const auto it = obj.find(key);
    if (it == obj.end()) return def;
    float v = def;
    if (!JsonToFloat(*it, v)) return def;
    return v;
}

bool GetBoolOr(const nlohmann::json& obj, const char* key, bool def) {
    if (!obj.is_object()) return def;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

std::string GetStringOr(const nlohmann::json& obj, const char* key, const std::string& def) {
    if (!obj.is_object()) return def;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

/// 读取数组形式的 vec3。
bool GetVec3(const nlohmann::json& obj, const char* key, Vec3& out) {
    if (!obj.is_object()) return false;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->size() < 3) return false;
    return JsonToFloat((*it)[0], out.x) && JsonToFloat((*it)[1], out.y) &&
           JsonToFloat((*it)[2], out.z);
}

/// 读取数组形式的 vec4。
bool GetVec4(const nlohmann::json& obj, const char* key, Vec4& out) {
    if (!obj.is_object()) return false;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->size() < 4) return false;
    return JsonToFloat((*it)[0], out.x) && JsonToFloat((*it)[1], out.y) &&
           JsonToFloat((*it)[2], out.z) && JsonToFloat((*it)[3], out.w);
}

/// 读取数组形式的四元数（glTF 顺序 x,y,z,w）。
bool GetQuat(const nlohmann::json& obj, const char* key, Vec4& out) {
    return GetVec4(obj, key, out);
}

/// 读取 16 个浮点数组成的列主序矩阵。
bool GetMat4(const nlohmann::json& obj, const char* key, Mat4& out) {
    if (!obj.is_object()) return false;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->size() < 16) return false;
    for (int i = 0; i < 16; ++i) {
        float v = 0.0f;
        if (!JsonToFloat((*it)[static_cast<std::size_t>(i)], v)) return false;
        out.m[i] = v;
    }
    return true;
}

// ---------------------------------------------------------------------------
// base64 解码（data URI 用）
// ---------------------------------------------------------------------------

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/// 严格 base64 解码。
/// - 忽略空白字符（换行等）。
/// - 非法字符、填充位置错误、长度非法一律返回 false。
bool DecodeBase64(const std::string& in, std::vector<unsigned char>& out) {
    out.clear();
    // 先剥离空白，得到有效字符数。
    std::string packed;
    packed.reserve(in.size());
    for (const char c : in) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        packed.push_back(c);
    }
    if (packed.empty()) return true;  // 空内容是合法的（0 字节）
    // 长度必须是 4 的倍数；末尾允许 1~2 个 '=' 填充。
    std::size_t pad = 0;
    while (pad < 2 && packed.size() > pad && packed[packed.size() - 1 - pad] == '=') ++pad;
    if (packed.size() % 4 != 0) return false;
    if (pad > 0 && packed.size() < 4) return false;

    out.reserve(packed.size() / 4 * 3);
    for (std::size_t i = 0; i < packed.size(); i += 4) {
        int v[4];
        int valid = 4;
        for (int k = 0; k < 4; ++k) {
            const char c = packed[i + static_cast<std::size_t>(k)];
            if (c == '=') {
                // '=' 只允许出现在最后两组，且其后不能再有非填充字符。
                if (i + 4 != packed.size() || k < 2) return false;
                v[k] = 0;
                --valid;
                continue;
            }
            const int d = Base64Value(c);
            if (d < 0) return false;
            v[k] = d;
        }
        const std::uint32_t triple = (static_cast<std::uint32_t>(v[0]) << 18) |
                                     (static_cast<std::uint32_t>(v[1]) << 12) |
                                     (static_cast<std::uint32_t>(v[2]) << 6) |
                                     static_cast<std::uint32_t>(v[3]);
        out.push_back(static_cast<unsigned char>((triple >> 16) & 0xFFu));
        if (valid >= 3) out.push_back(static_cast<unsigned char>((triple >> 8) & 0xFFu));
        if (valid >= 4) out.push_back(static_cast<unsigned char>(triple & 0xFFu));
    }
    return true;
}

/// 解析 data URI。成功返回 true，data 写入 out，mimeType 写入 mime。
bool ParseDataUri(const std::string& uri, std::vector<unsigned char>& out,
                  std::string& mime, std::string& error) {
    if (uri.size() < 5 || uri.compare(0, 5, "data:") != 0) {
        error = "data URI 格式非法";
        return false;
    }
    const std::size_t comma = uri.find(',');
    if (comma == std::string::npos) {
        error = "data URI 缺少数据部分";
        return false;
    }
    const std::string header = uri.substr(5, comma - 5);
    const std::string payload = uri.substr(comma + 1);

    // header 形如 "text/plain;base64" 或 "image/png"。
    std::string meta = header;
    bool isBase64 = false;
    const std::size_t semi = header.find(';');
    if (semi != std::string::npos) {
        meta = header.substr(0, semi);
        const std::string params = header.substr(semi + 1);
        if (params.find("base64") != std::string::npos) isBase64 = true;
    }
    mime = meta;

    if (!isBase64) {
        // 非 base64 的 data URI 是百分号编码的原始文本，VRM 基本不用，这里按原文处理。
        out.assign(payload.begin(), payload.end());
        return true;
    }
    if (!DecodeBase64(payload, out)) {
        error = "data URI 的 base64 数据非法";
        return false;
    }
    return true;
}

/// 百分号解码（glTF 的外部资源 URI 是百分号编码的）。
std::string PercentDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

/// 取路径中的目录部分（不含末尾分隔符）。
std::string DirNameOf(const std::string& path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

/// 读取整个文件。失败返回 false。
bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out,
                   std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "无法打开文件：" + path;
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) {
        error = "无法读取文件大小：" + path;
        return false;
    }
    if (static_cast<std::uint64_t>(size) > kMaxFileSize) {
        error = "文件过大（超过 512 MB），已拒绝加载";
        return false;
    }
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(out.data()), size)) {
        error = "读取文件失败：" + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// accessor 底层读取
// ---------------------------------------------------------------------------

std::size_t ComponentSize(int componentType) {
    switch (componentType) {
        case kGltfByte:
        case kGltfUnsignedByte:
            return 1;
        case kGltfShort:
        case kGltfUnsignedShort:
            return 2;
        case kGltfUnsignedInt:
        case kGltfFloat:
            return 4;
        default:
            return 0;
    }
}

int ComponentCountOf(GltfType type) {
    switch (type) {
        case GltfType::Scalar: return 1;
        case GltfType::Vec2: return 2;
        case GltfType::Vec3: return 3;
        case GltfType::Vec4: return 4;
        case GltfType::Mat2: return 4;
        case GltfType::Mat3: return 9;
        case GltfType::Mat4: return 16;
        default: return 0;
    }
}

GltfType TypeFromString(const std::string& s) {
    if (s == "SCALAR") return GltfType::Scalar;
    if (s == "VEC2") return GltfType::Vec2;
    if (s == "VEC3") return GltfType::Vec3;
    if (s == "VEC4") return GltfType::Vec4;
    if (s == "MAT2") return GltfType::Mat2;
    if (s == "MAT3") return GltfType::Mat3;
    if (s == "MAT4") return GltfType::Mat4;
    return GltfType::Scalar;
}

/// 把一个分量读成浮点（按 glTF 规范处理 normalized 整数）。
float ReadComponentAsFloat(const unsigned char* p, int componentType, bool normalized) {
    switch (componentType) {
        case kGltfFloat: {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }
        case kGltfByte: {
            std::int8_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            if (!normalized) return static_cast<float>(v);
            return std::max(static_cast<float>(v) / 127.0f, -1.0f);
        }
        case kGltfUnsignedByte: {
            const std::uint8_t v = *p;
            if (!normalized) return static_cast<float>(v);
            return static_cast<float>(v) / 255.0f;
        }
        case kGltfShort: {
            std::int16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            if (!normalized) return static_cast<float>(v);
            return std::max(static_cast<float>(v) / 32767.0f, -1.0f);
        }
        case kGltfUnsignedShort: {
            std::uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            if (!normalized) return static_cast<float>(v);
            return static_cast<float>(v) / 65535.0f;
        }
        case kGltfUnsignedInt: {
            std::uint32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v);
        }
        default:
            return 0.0f;
    }
}

/// 把一个分量读成无符号整数。
std::uint32_t ReadComponentAsU32(const unsigned char* p, int componentType) {
    switch (componentType) {
        case kGltfByte: {
            std::int8_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<std::uint32_t>(v);
        }
        case kGltfUnsignedByte:
            return *p;
        case kGltfShort: {
            std::int16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<std::uint32_t>(v);
        }
        case kGltfUnsignedShort: {
            std::uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<uint32_t>(v);
        }
        case kGltfUnsignedInt: {
            std::uint32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }
        default:
            return 0;
    }
}

/// accessor 的实际数据布局。
struct AccessorView {
    const unsigned char* base = nullptr;  ///< 第一个元素的起始地址
    std::size_t stride = 0;                ///< 元素步长（字节）
    std::size_t count = 0;
    int componentType = 0;
    bool normalized = false;
    int components = 0;
};

/// 解析 accessor 并定位其数据。返回 false 时 error 中写入原因。
bool ResolveAccessor(const std::vector<GltfAccessor>& accessors,
                     const std::vector<GltfBufferView>& views,
                     const std::vector<GltfBuffer>& buffers, int accessorIndex,
                     AccessorView& view, std::string& error) {
    if (accessorIndex < 0 || static_cast<std::size_t>(accessorIndex) >= accessors.size()) {
        error = "accessor 索引越界";
        return false;
    }
    const GltfAccessor& acc = accessors[static_cast<std::size_t>(accessorIndex)];

    if (acc.count > kMaxAccessorCount) {
        error = "accessor 元素数量超出上限（文件可能已损坏）";
        return false;
    }
    const std::size_t compSize = ComponentSize(acc.componentType);
    if (compSize == 0) {
        error = "accessor 的 componentType 非法";
        return false;
    }
    const int comps = ComponentCountOf(acc.type);
    if (comps <= 0) {
        error = "accessor 的 type 非法";
        return false;
    }
    if (acc.count == 0) {
        // 空的 accessor 是合法的（某些导出器会写出 count=0）。
        view.base = nullptr;
        view.stride = 0;
        view.count = 0;
        view.componentType = acc.componentType;
        view.normalized = acc.normalized;
        view.components = comps;
        return true;
    }
    if (acc.bufferView < 0 || static_cast<std::size_t>(acc.bufferView) >= views.size()) {
        error = "accessor 的 bufferView 索引越界";
        return false;
    }
    const GltfBufferView& viewDesc = views[static_cast<std::size_t>(acc.bufferView)];
    if (viewDesc.buffer < 0 || static_cast<std::size_t>(viewDesc.buffer) >= buffers.size()) {
        error = "bufferView 的 buffer 索引越界";
        return false;
    }
    const GltfBuffer& buffer = buffers[static_cast<std::size_t>(viewDesc.buffer)];

    const std::size_t elementSize = compSize * static_cast<std::size_t>(comps);
    std::size_t stride = viewDesc.byteStride;
    if (stride == 0) stride = elementSize;
    if (stride < elementSize) {
        error = "bufferView 的 byteStride 小于元素长度";
        return false;
    }
    if (stride > 512) {
        error = "bufferView 的 byteStride 超出规范上限";
        return false;
    }

    // accessor 的数据必须完整落在 bufferView 之内。
    const std::size_t viewStart = viewDesc.byteOffset;
    if (viewStart > buffer.data.size()) {
        error = "bufferView 的起点超出 buffer 范围";
        return false;
    }
    if (viewDesc.byteLength > buffer.data.size() - viewStart) {
        error = "bufferView 的长度超出 buffer 范围";
        return false;
    }
    const std::size_t accStart = viewStart + acc.byteOffset;
    if (accStart > viewStart + viewDesc.byteLength) {
        error = "accessor 的起点超出 bufferView 范围";
        return false;
    }
    // 最后一个元素的起点必须在 bufferView 内。
    const std::size_t lastOffset = (acc.count - 1) * stride;
    if (lastOffset > viewStart + viewDesc.byteLength - accStart ||
        elementSize > viewStart + viewDesc.byteLength - accStart - lastOffset) {
        error = "accessor 的数据范围超出 bufferView";
        return false;
    }

    view.base = buffer.data.data() + accStart;
    view.stride = stride;
    view.count = acc.count;
    view.componentType = acc.componentType;
    view.normalized = acc.normalized;
    view.components = comps;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 四元数 -> 矩阵（glTF 的 rotation 是 xyzw 顺序的四元数）
// ---------------------------------------------------------------------------

Mat4 QuaternionToMat4(const Vec4& q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float len2 = x * x + y * y + z * z + w * w;
    if (len2 < kEpsilon) return Mat4::Identity();
    // 归一化：导出器给出的四元数理论上已归一，但恶意 / 损坏文件未必。
    const float inv = 1.0f / std::sqrt(len2);
    const float nx = x * inv, ny = y * inv, nz = z * inv, nw = w * inv;

    Mat4 r;
    r.At(0, 0) = 1.0f - 2.0f * (ny * ny + nz * nz);
    r.At(0, 1) = 2.0f * (nx * ny - nz * nw);
    r.At(0, 2) = 2.0f * (nx * nz + ny * nw);
    r.At(1, 0) = 2.0f * (nx * ny + nz * nw);
    r.At(1, 1) = 1.0f - 2.0f * (nx * nx + nz * nz);
    r.At(1, 2) = 2.0f * (ny * nz - nx * nw);
    r.At(2, 0) = 2.0f * (nx * nz - ny * nw);
    r.At(2, 1) = 2.0f * (ny * nz + nx * nw);
    r.At(2, 2) = 1.0f - 2.0f * (nx * nx + ny * ny);
    return r;
}

/// 把列主序矩阵近似分解成 TRS（忽略错切与非均匀缩放带来的旋转耦合）。
/// 只在节点使用 matrix 字段时使用，目的是让动画能按分量覆盖。
static void DecomposeMat4(const Mat4& m, Vec3& translation, Vec4& rotation, Vec3& scale) {
    translation = Vec3(m.At(0, 3), m.At(1, 3), m.At(2, 3));

    Vec3 axisX(m.At(0, 0), m.At(1, 0), m.At(2, 0));
    Vec3 axisY(m.At(0, 1), m.At(1, 1), m.At(2, 1));
    Vec3 axisZ(m.At(0, 2), m.At(1, 2), m.At(2, 2));
    scale = Vec3(Length(axisX), Length(axisY), Length(axisZ));
    if (scale.x < kEpsilon || scale.y < kEpsilon || scale.z < kEpsilon) {
        rotation = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        scale = Vec3(1.0f, 1.0f, 1.0f);
        return;
    }
    axisX = axisX / scale.x;
    axisY = axisY / scale.y;
    axisZ = axisZ / scale.z;

    // 用三个基向量构造旋转矩阵，再转成四元数。
    const float m00 = axisX.x, m01 = axisY.x, m02 = axisZ.x;
    const float m10 = axisX.y, m11 = axisY.y, m12 = axisZ.y;
    const float m20 = axisX.z, m21 = axisY.z, m22 = axisZ.z;
    const float trace = m00 + m11 + m22;
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        w = 0.25f * s;
        x = (m21 - m12) / s;
        y = (m02 - m20) / s;
        z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        w = (m21 - m12) / s;
        x = 0.25f * s;
        y = (m01 + m10) / s;
        z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        w = (m02 - m20) / s;
        x = (m01 + m10) / s;
        y = 0.25f * s;
        z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        w = (m10 - m01) / s;
        x = (m02 + m20) / s;
        y = (m12 + m21) / s;
        z = 0.25f * s;
    }
    rotation = Vec4(x, y, z, w);
}

// ---------------------------------------------------------------------------
// GltfDocument：accessor 读取
// ---------------------------------------------------------------------------

bool GltfDocument::ReadAccessorFloats(int accessorIndex, std::vector<float>& out,
                                       std::string& error) const {
    AccessorView view;
    if (!ResolveAccessor(accessors_, bufferViews_, buffers_, accessorIndex, view, error)) {
        return false;
    }
    out.clear();
    if (view.count == 0) return true;
    const std::size_t comps = static_cast<std::size_t>(view.components);
    out.resize(view.count * comps);
    const std::size_t compSize = ComponentSize(view.componentType);
    for (std::size_t i = 0; i < view.count; ++i) {
        const unsigned char* p = view.base + i * view.stride;
        for (std::size_t k = 0; k < comps; ++k) {
            out[i * comps + k] = ReadComponentAsFloat(p + k * compSize, view.componentType,
                                                      view.normalized);
        }
    }
    return true;
}

bool GltfDocument::ReadAccessorVec3(int accessorIndex, std::vector<Vec3>& out,
                                     std::string& error) const {
    std::vector<float> raw;
    if (!ReadAccessorFloats(accessorIndex, raw, error)) return false;
    out.clear();
    const std::size_t count = raw.size() / 3;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(Vec3(raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]));
    }
    return true;
}

bool GltfDocument::ReadAccessorVec4(int accessorIndex, std::vector<Vec4>& out,
                                     std::string& error) const {
    std::vector<float> raw;
    if (!ReadAccessorFloats(accessorIndex, raw, error)) return false;
    out.clear();
    const std::size_t count = raw.size() / 4;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(Vec4(raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], raw[i * 4 + 3]));
    }
    return true;
}

bool GltfDocument::ReadAccessorU32(int accessorIndex, std::vector<std::uint32_t>& out,
                                    std::string& error) const {
    AccessorView view;
    if (!ResolveAccessor(accessors_, bufferViews_, buffers_, accessorIndex, view, error)) {
        return false;
    }
    out.clear();
    if (view.count == 0) return true;
    const std::size_t comps = static_cast<std::size_t>(view.components);
    out.resize(view.count * comps);
    const std::size_t compSize = ComponentSize(view.componentType);
    for (std::size_t i = 0; i < view.count; ++i) {
        const unsigned char* p = view.base + i * view.stride;
        for (std::size_t k = 0; k < comps; ++k) {
            out[i * comps + k] = ReadComponentAsU32(p + k * compSize, view.componentType);
        }
    }
    return true;
}

bool GltfDocument::ReadAccessorMat4(int accessorIndex, std::vector<Mat4>& out,
                                     std::string& error) const {
    AccessorView view;
    if (!ResolveAccessor(accessors_, bufferViews_, buffers_, accessorIndex, view, error)) {
        return false;
    }
    out.clear();
    if (view.count == 0) return true;
    if (view.components != 16) {
        error = "逆绑定矩阵 accessor 的类型不是 MAT4";
        return false;
    }
    out.resize(view.count);
    const std::size_t compSize = ComponentSize(view.componentType);
    for (std::size_t i = 0; i < view.count; ++i) {
        const unsigned char* p = view.base + i * view.stride;
        for (std::size_t k = 0; k < 16; ++k) {
            out[i].m[k] = ReadComponentAsFloat(p + k * compSize, view.componentType,
                                                view.normalized);
        }
    }
    return true;
}

bool GltfDocument::GetImageBytes(int imageIndex, std::vector<unsigned char>& out,
                                 std::string& mimeType, std::string& error) const {
    out.clear();
    mimeType.clear();
    if (imageIndex < 0 || static_cast<std::size_t>(imageIndex) >= images_.size()) {
        error = "图片索引越界";
        return false;
    }
    const GltfImage& image = images_[static_cast<std::size_t>(imageIndex)];
    mimeType = image.mimeType;

    if (image.bufferView >= 0) {
        if (static_cast<std::size_t>(image.bufferView) >= bufferViews_.size()) {
            error = "图片的 bufferView 索引越界";
            return false;
        }
        const GltfBufferView& view = bufferViews_[static_cast<std::size_t>(image.bufferView)];
        if (view.buffer < 0 || static_cast<std::size_t>(view.buffer) >= buffers_.size()) {
            error = "图片的 buffer 索引越界";
            return false;
        }
        const GltfBuffer& buffer = buffers_[static_cast<std::size_t>(view.buffer)];
        if (view.byteOffset > buffer.data.size() ||
            view.byteLength > buffer.data.size() - view.byteOffset) {
            error = "图片数据超出 buffer 范围";
            return false;
        }
        out.assign(buffer.data.begin() + static_cast<std::ptrdiff_t>(view.byteOffset),
                   buffer.data.begin() + static_cast<std::ptrdiff_t>(view.byteOffset +
                                                                    view.byteLength));
        return true;
    }

    if (image.uri.empty()) {
        error = "图片既没有 uri 也没有 bufferView";
        return false;
    }
    if (image.uri.compare(0, 5, "data:") == 0) {
        std::string mime;
        if (!ParseDataUri(image.uri, out, mime, error)) return false;
        if (mimeType.empty()) mimeType = mime;
        return true;
    }
    // 外部文件：相对模型文件所在目录。
    const std::string rel = PercentDecode(image.uri);
    const std::string full = rel.empty() || rel[0] == '/' ? rel : baseDir_ + "/" + rel;
    if (!ReadWholeFile(full, out, error)) return false;
    if (mimeType.empty()) {
        if (full.size() >= 4 && full.compare(full.size() - 4, 4, ".png") == 0) {
            mimeType = "image/png";
        } else if (full.size() >= 4 && full.compare(full.size() - 4, 4, ".jpg") == 0) {
            mimeType = "image/jpeg";
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

/// 内部构建器：GltfDocument 的私有数据只有它可以写入。
struct GltfParserAccess {

/// 解析 buffers（含 GLB 的 BIN chunk 与外部 / data URI 资源）。
static bool ParseBuffers(const nlohmann::json& root, const std::string& baseDir,
                  const std::vector<unsigned char>& glbBin, bool hasGlbBin,
                  GltfDocument& doc, std::string& error) {
    const auto it = root.find("buffers");
    if (it == root.end() || !it->is_array()) {
        // 没有 buffers 段：GLB 里仍然可能有一个 BIN chunk（规范上 buffer 0 即为它）。
        if (hasGlbBin) {
            GltfBuffer buffer;
            buffer.data = glbBin;
            doc.buffers_.push_back(std::move(buffer));
        }
        return true;
    }
    if (it->size() > kMaxBufferCount) {
        error = "buffer 数量超出上限";
        return false;
    }
    for (std::size_t i = 0; i < it->size(); ++i) {
        const nlohmann::json& item = (*it)[i];
        GltfBuffer buffer;
        buffer.uri = GetStringOr(item, "uri", std::string());

        if (buffer.uri.empty()) {
            // 没有 uri 的 buffer 只能是 GLB 的第 0 号 buffer（BIN chunk）。
            // 少数导出器会写出更多没有 uri 的 buffer，这里按「空 buffer」处理，
            // 后续访问它的 accessor 会因为越界而读不出数据，不会崩溃。
            if (i == 0 && hasGlbBin) {
                buffer.data = glbBin;
            } else {
                std::ostringstream oss;
                oss << "buffer " << i << " 缺少 uri 且不是 GLB 的 BIN 块，按空 buffer 处理";
                doc.warnings_.push_back(oss.str());
            }
        } else if (buffer.uri.compare(0, 5, "data:") == 0) {
            std::string mime;
            if (!ParseDataUri(buffer.uri, buffer.data, mime, error)) {
                std::ostringstream oss;
                oss << "buffer " << i << "：" << error;
                error = oss.str();
                return false;
            }
        } else {
            const std::string rel = PercentDecode(buffer.uri);
            const std::string full = rel.empty() || rel[0] == '/' ? rel : baseDir + "/" + rel;
            if (!ReadWholeFile(full, buffer.data, error)) {
                std::ostringstream oss;
                oss << "buffer " << i << "：" << error;
                error = oss.str();
                return false;
            }
        }
        doc.buffers_.push_back(std::move(buffer));
    }
    return true;
}

/// 解析 bufferViews。
static bool ParseBufferViews(const nlohmann::json& root, GltfDocument& doc, std::string& error) {
    const auto it = root.find("bufferViews");
    if (it == root.end() || !it->is_array()) return true;
    if (it->size() > kMaxBufferCount * 16) {
        error = "bufferView 数量超出上限";
        return false;
    }
    doc.bufferViews_.reserve(it->size());
    for (std::size_t i = 0; i < it->size(); ++i) {
        const nlohmann::json& item = (*it)[i];
        GltfBufferView view;
        view.buffer = GetIntOr(item, "buffer", -1);
        const auto off = item.find("byteOffset");
        if (off != item.end() && off->is_number()) {
            const double d = off->get<double>();
            if (d < 0 || d > 4294967295.0) {
                error = "bufferView 的 byteOffset 非法";
                return false;
            }
            view.byteOffset = static_cast<std::size_t>(d);
        }
        const auto len = item.find("byteLength");
        if (len != item.end() && len->is_number()) {
            const double d = len->get<double>();
            if (d < 0 || d > 4294967295.0) {
                error = "bufferView 的 byteLength 非法";
                return false;
            }
            view.byteLength = static_cast<std::size_t>(d);
        }
        const auto stride = item.find("byteStride");
        if (stride != item.end() && stride->is_number()) {
            const double d = stride->get<double>();
            if (d < 0 || d > 512) {
                error = "bufferView 的 byteStride 非法";
                return false;
            }
            view.byteStride = static_cast<std::size_t>(d);
        }
        doc.bufferViews_.push_back(view);
    }
    return true;
}

/// 解析 accessors。
static bool ParseAccessors(const nlohmann::json& root, GltfDocument& doc, std::string& error) {
    const auto it = root.find("accessors");
    if (it == root.end() || !it->is_array()) return true;
    if (it->size() > kMaxAccessorCount) {
        error = "accessor 数量超出上限";
        return false;
    }
    doc.accessors_.reserve(it->size());
    for (std::size_t i = 0; i < it->size(); ++i) {
        const nlohmann::json& item = (*it)[i];
        GltfAccessor acc;
        acc.bufferView = GetIntOr(item, "bufferView", -1);
        const auto off = item.find("byteOffset");
        if (off != item.end() && off->is_number()) {
            const double d = off->get<double>();
            if (d < 0 || d > 4294967295.0) {
                error = "accessor 的 byteOffset 非法";
                return false;
            }
            acc.byteOffset = static_cast<std::size_t>(d);
        }
        acc.componentType = GetIntOr(item, "componentType", kGltfFloat);
        acc.normalized = GetBoolOr(item, "normalized", false);
        const auto count = item.find("count");
        if (count != item.end() && count->is_number()) {
            const double d = count->get<double>();
            if (d < 0 || d > static_cast<double>(kMaxAccessorCount)) {
                error = "accessor 的 count 非法或超出上限";
                return false;
            }
            acc.count = static_cast<std::size_t>(d);
        }
        acc.type = TypeFromString(GetStringOr(item, "type", "SCALAR"));
        doc.accessors_.push_back(acc);
    }
    return true;
}

/// 读取图元属性里的 accessor 索引（缺失时返回 -1）。
static int GetAttributeAccessor(const nlohmann::json& primitive, const char* name) {
    const auto attrs = primitive.find("attributes");
    if (attrs == primitive.end() || !attrs->is_object()) return -1;
    const auto it = attrs->find(name);
    if (it == attrs->end() || !it->is_number()) return -1;
    int v = -1;
    if (!JsonToInt(*it, v)) return -1;
    return v;
}

/// 读取 morph target 上的 accessor 索引（缺失时返回 -1）。
///
/// 注意：morph target 的结构与图元属性不同——accessor 索引直接挂在 target 对象上，
/// 不在 "attributes" 子对象里（glTF 2.0 规范 3.7.2.1）。
/// 例如 "targets": [ { "POSITION": 13, "NORMAL": 14 } ]。
static int GetTargetAccessor(const nlohmann::json& target, const char* name) {
    if (!target.is_object()) return -1;
    const auto it = target.find(name);
    if (it == target.end() || !it->is_number()) return -1;
    int v = -1;
    if (!JsonToInt(*it, v)) return -1;
    return v;
}

/// 解析 meshes（含 morph targets）。
static bool ParseMeshes(const nlohmann::json& root, GltfDocument& doc, std::string& error) {
    const auto it = root.find("meshes");
    if (it == root.end() || !it->is_array()) return true;
    if (it->size() > kMaxMeshCount) {
        error = "mesh 数量超出上限";
        return false;
    }
    doc.meshes_.reserve(it->size());
    for (std::size_t i = 0; i < it->size(); ++i) {
        const nlohmann::json& item = (*it)[i];
        GltfMesh mesh;
        mesh.name = GetStringOr(item, "name", std::string());

        const auto weights = item.find("weights");
        if (weights != item.end() && weights->is_array()) {
            mesh.weights.reserve(weights->size());
            for (const auto& w : *weights) {
                float v = 0.0f;
                if (JsonToFloat(w, v)) mesh.weights.push_back(v);
            }
        }
        // VRM 约定：morph 名字放在 mesh.extras.targetNames 里。
        const auto extras = item.find("extras");
        if (extras != item.end() && extras->is_object()) {
            const auto names = extras->find("targetNames");
            if (names != extras->end() && names->is_array()) {
                for (const auto& n : *names) {
                    if (n.is_string()) mesh.targetNames.push_back(n.get<std::string>());
                }
            }
        }

        const auto prims = item.find("primitives");
        if (prims != item.end() && prims->is_array()) {
            if (prims->size() > kMaxPrimitiveCount) {
                error = "primitive 数量超出上限";
                return false;
            }
            for (std::size_t p = 0; p < prims->size(); ++p) {
                const nlohmann::json& prim = (*prims)[p];
                GltfPrimitive primitive;
                primitive.mode = GetIntOr(prim, "mode", 4);
                primitive.material = GetIntOr(prim, "material", -1);
                primitive.indicesAccessor = GetIntOr(prim, "indices", -1);
                primitive.positionAccessor = GetAttributeAccessor(prim, "POSITION");
                primitive.normalAccessor = GetAttributeAccessor(prim, "NORMAL");
                primitive.texcoordAccessor = GetAttributeAccessor(prim, "TEXCOORD_0");
                primitive.tangentAccessor = GetAttributeAccessor(prim, "TANGENT");
                primitive.colorAccessor = GetAttributeAccessor(prim, "COLOR_0");
                primitive.jointsAccessor = GetAttributeAccessor(prim, "JOINTS_0");
                primitive.weightsAccessor = GetAttributeAccessor(prim, "WEIGHTS_0");

                const auto targets = prim.find("targets");
                if (targets != prim.end() && targets->is_array()) {
                    if (targets->size() > kMaxMorphTargetCount) {
                        error = "morph target 数量超出上限";
                        return false;
                    }
                    for (std::size_t t = 0; t < targets->size(); ++t) {
                        const nlohmann::json& tgt = (*targets)[t];
                        GltfPrimitive::MorphTarget target;
                        target.position = GetTargetAccessor(tgt, "POSITION");
                        target.normal = GetTargetAccessor(tgt, "NORMAL");
                        target.tangent = GetTargetAccessor(tgt, "TANGENT");
                        primitive.targets.push_back(target);
                    }
                }
                mesh.primitives.push_back(std::move(primitive));
            }
        }
        doc.meshes_.push_back(std::move(mesh));
    }
    return true;
}

/// 解析 nodes。
static bool ParseNodes(const nlohmann::json& root, GltfDocument& doc, std::string& error) {
    const auto it = root.find("nodes");
    if (it == root.end() || !it->is_array()) return true;
    if (it->size() > kMaxNodeCount) {
        error = "node 数量超出上限";
        return false;
    }
    doc.nodes_.reserve(it->size());
    for (std::size_t i = 0; i < it->size(); ++i) {
        const nlohmann::json& item = (*it)[i];
        GltfNode node;
        node.name = GetStringOr(item, "name", std::string());
        node.mesh = GetIntOr(item, "mesh", -1);
        node.skin = GetIntOr(item, "skin", -1);

        const auto children = item.find("children");
        if (children != item.end() && children->is_array()) {
            node.children.reserve(children->size());
            for (const auto& c : *children) {
                int v = -1;
                if (JsonToInt(c, v)) node.children.push_back(v);
            }
        }
        const auto weights = item.find("weights");
        if (weights != item.end() && weights->is_array()) {
            for (const auto& w : *weights) {
                float v = 0.0f;
                if (JsonToFloat(w, v)) node.weights.push_back(v);
            }
        }

        Mat4 matrix;
        if (GetMat4(item, "matrix", matrix)) {
            node.local = matrix;
            node.hasMatrix = true;
            // matrix 形式没有直接的 TRS，这里做一次近似分解（忽略错切），
            // 目的只是让动画采样能按分量覆盖。
            DecomposeMat4(matrix, node.translation, node.rotation, node.scale);
        } else {
            GetVec3(item, "translation", node.translation);
            GetVec3(item, "scale", node.scale);
            GetQuat(item, "rotation", node.rotation);
            node.local = Mat4::Translation(node.translation) *
                         QuaternionToMat4(node.rotation) * Mat4::Scale(node.scale);
        }
        doc.nodes_.push_back(std::move(node));
    }
    return true;
}

/// 解析 skins。
static bool ParseSkins(const nlohmann::json& root, GltfDocument& doc, std::string& error) {
    const auto it = root.find("skins");
    if (it == root.end() || !it->is_array()) return true;
    if (it->size() > kMaxSkinCount) {
        error = "skin 数量超出上限";
        return false;
    }
    doc.skins_.reserve(it->size());
    for (std::size_t i = 0; i < it->size(); ++i) {
        const nlohmann::json& item = (*it)[i];
        GltfSkin skin;
        skin.name = GetStringOr(item, "name", std::string());
        skin.inverseBindMatricesAccessor = GetIntOr(item, "inverseBindMatrices", -1);
        skin.skeleton = GetIntOr(item, "skeleton", -1);
        const auto joints = item.find("joints");
        if (joints != item.end() && joints->is_array()) {
            if (joints->size() > kMaxJointCount) {
                error = "skin 的关节数量超出上限";
                return false;
            }
            for (const auto& j : *joints) {
                int v = -1;
                if (JsonToInt(j, v)) skin.joints.push_back(v);
            }
        }
        doc.skins_.push_back(std::move(skin));
    }
    return true;
}

/// 读取纹理引用（{"index": 3, "texCoord": 0} 形式）。
static int GetTextureRef(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return -1;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_object()) return -1;
    return GetIntOr(*it, "index", -1);
}

/// 解析 materials（含 VRMC_materials_mtoon）。
static bool ParseMaterials(const nlohmann::json& root, GltfDocument& doc, std::string& error) {
    const auto it = root.find("materials");
    if (it == root.end() || !it->is_array()) return true;
    if (it->size() > kMaxMaterialCount) {
        error = "material 数量超出上限";
        return false;
    }
    doc.materials_.reserve(it->size());
    for (std::size_t i = 0; i < it->size(); ++i) {
        const nlohmann::json& item = (*it)[i];
        GltfMaterial mat;
        mat.name = GetStringOr(item, "name", std::string());
        mat.alphaMode = GetStringOr(item, "alphaMode", "OPAQUE");
        mat.alphaCutoff = GetFloatOr(item, "alphaCutoff", 0.5f);
        mat.doubleSided = GetBoolOr(item, "doubleSided", false);

        const auto pbr = item.find("pbrMetallicRoughness");
        if (pbr != item.end() && pbr->is_object()) {
            Vec4 base(1.0f, 1.0f, 1.0f, 1.0f);
            GetVec4(*pbr, "baseColorFactor", base);
            mat.baseColorFactor = base;
            mat.baseColorTexture = GetTextureRef(*pbr, "baseColorTexture");
            mat.metallicFactor = GetFloatOr(*pbr, "metallicFactor", 1.0f);
            mat.roughnessFactor = GetFloatOr(*pbr, "roughnessFactor", 1.0f);
        }
        GetVec3(item, "emissiveFactor", mat.emissiveFactor);
        mat.emissiveTexture = GetTextureRef(item, "emissiveTexture");
        mat.normalTexture = GetTextureRef(item, "normalTexture");
        mat.occlusionTexture = GetTextureRef(item, "occlusionTexture");

        const auto ext = item.find("extensions");
        if (ext != item.end() && ext->is_object()) {
            if (ext->contains("KHR_materials_unlit")) {
                mat.unlit = true;
            }
            const auto mtoon = ext->find("VRMC_materials_mtoon");
            if (mtoon != ext->end() && mtoon->is_object()) {
                mat.hasMtoon = true;
                GetVec3(*mtoon, "shadeColorFactor", mat.shadeColorFactor);
                mat.shadeMultiplyTexture = GetTextureRef(*mtoon, "shadeMultiplyTexture");
                mat.shadeColorTexture = GetTextureRef(*mtoon, "shadeColorTexture");
                GetVec3(*mtoon, "rimColorFactor", mat.rimColorFactor);
                mat.rimMultiplyTexture = GetTextureRef(*mtoon, "rimMultiplyTexture");
mat.rimColorTexture = GetTextureRef(*mtoon, "rimColorTexture");
    mat.rimLightingMixFactor = GetFloatOr(*mtoon, "rimLightingMixFactor", 0.0f);
    mat.outlineWidthMode = GetStringOr(*mtoon, "outlineWidthMode", "none");
                mat.outlineWidthFactor = GetFloatOr(*mtoon, "outlineWidthFactor", 0.0f);
                GetVec3(*mtoon, "outlineColorFactor", mat.outlineColorFactor);
                mat.outlineLightingMixFactor = GetFloatOr(*mtoon, "outlineLightingMixFactor", 1.0f);
                mat.lightColorAttenuation = GetFloatOr(*mtoon, "lightColorAttenuation", 0.5f);
                mat.indirectLightIntensity = GetFloatOr(*mtoon, "indirectLightIntensity", 0.1f);
                mat.reflectanceFactor = GetFloatOr(*mtoon, "reflectanceFactor", 0.5f);
                mat.reflectanceTexture = GetTextureRef(*mtoon, "reflectanceTexture");
                mat.clearCoatFactor = GetFloatOr(*mtoon, "clearCoatFactor", 0.0f);
                mat.clearCoatRoughnessFactor =
                    GetFloatOr(*mtoon, "clearCoatRoughnessFactor", 0.0f);
                GetVec3(*mtoon, "sheenColorFactor", mat.sheenColorFactor);
                mat.sheenRoughnessFactor = GetFloatOr(*mtoon, "sheenRoughnessFactor", 0.5f);
                mat.shadingShiftFactor = GetFloatOr(*mtoon, "shadingShiftFactor", 0.0f);
                mat.shadingToonyFactor = GetFloatOr(*mtoon, "shadingToonyFactor", 0.9f);
                mat.parametricRimFresnelPowerFactor =
                    GetFloatOr(*mtoon, "parametricRimFresnelPowerFactor", 5.0f);
            }
        }
        doc.materials_.push_back(std::move(mat));
    }
    return true;
}

/// 解析 images / samplers / textures。
static bool ParseTextures(const nlohmann::json& root, GltfDocument& doc, std::string& error) {
    const auto images = root.find("images");
    if (images != root.end() && images->is_array()) {
        if (images->size() > kMaxImageCount) {
            error = "image 数量超出上限";
            return false;
        }
        for (std::size_t i = 0; i < images->size(); ++i) {
            const nlohmann::json& item = (*images)[i];
            GltfImage image;
            image.name = GetStringOr(item, "name", std::string());
            image.mimeType = GetStringOr(item, "mimeType", std::string());
            image.uri = GetStringOr(item, "uri", std::string());
            image.bufferView = GetIntOr(item, "bufferView", -1);
            doc.images_.push_back(std::move(image));
        }
    }

    const auto samplers = root.find("samplers");
    if (samplers != root.end() && samplers->is_array()) {
        for (std::size_t i = 0; i < samplers->size(); ++i) {
            const nlohmann::json& item = (*samplers)[i];
            GltfSampler sampler;
            sampler.magFilter = GetIntOr(item, "magFilter", sampler.magFilter);
            sampler.minFilter = GetIntOr(item, "minFilter", sampler.minFilter);
            sampler.wrapS = GetIntOr(item, "wrapS", sampler.wrapS);
            sampler.wrapT = GetIntOr(item, "wrapT", sampler.wrapT);
            doc.samplers_.push_back(sampler);
        }
    }

    const auto textures = root.find("textures");
    if (textures != root.end() && textures->is_array()) {
        if (textures->size() > kMaxTextureCount) {
            error = "texture 数量超出上限";
            return false;
        }
        for (std::size_t i = 0; i < textures->size(); ++i) {
            const nlohmann::json& item = (*textures)[i];
            GltfTexture texture;
            texture.source = GetIntOr(item, "source", -1);
            texture.sampler = GetIntOr(item, "sampler", -1);
            const auto ext = item.find("extensions");
            if (ext != item.end() && ext->is_object()) {
                const auto transform = ext->find("KHR_texture_transform");
                if (transform != ext->end() && transform->is_object()) {
                    texture.hasTransform = true;
                    const auto off = transform->find("offset");
                    if (off != transform->end() && off->is_array() && off->size() >= 2) {
                        float x = 0.0f, y = 0.0f;
                        if (JsonToFloat((*off)[0], x) && JsonToFloat((*off)[1], y)) {
                            texture.uvOffset = Vec2(x, y);
                        }
                    }
                    const auto scale = transform->find("scale");
                    if (scale != transform->end() && scale->is_array() && scale->size() >= 2) {
                        float x = 1.0f, y = 1.0f;
                        if (JsonToFloat((*scale)[0], x) && JsonToFloat((*scale)[1], y)) {
                            texture.uvScale = Vec2(x, y);
                        }
                    }
                    texture.uvRotation = GetFloatOr(*transform, "rotation", 0.0f);
                }
            }
            doc.textures_.push_back(std::move(texture));
        }
    }
    return true;
}

/// 解析 animations。
static bool ParseAnimations(const nlohmann::json& root, GltfDocument& doc, std::string& error) {
    const auto it = root.find("animations");
    if (it == root.end() || !it->is_array()) return true;
    if (it->size() > kMaxAnimationCount) {
        error = "animation 数量超出上限";
        return false;
    }
    doc.animations_.reserve(it->size());
    for (std::size_t i = 0; i < it->size(); ++i) {
        const nlohmann::json& item = (*it)[i];
        GltfAnimation anim;
        anim.name = GetStringOr(item, "name", std::string());

        const auto samplers = item.find("samplers");
        if (samplers != item.end() && samplers->is_array()) {
            for (std::size_t s = 0; s < samplers->size(); ++s) {
                const nlohmann::json& sitem = (*samplers)[s];
                GltfAnimationSampler sampler;
                sampler.input = GetIntOr(sitem, "input", -1);
                sampler.output = GetIntOr(sitem, "output", -1);
                sampler.interpolation = GetStringOr(sitem, "interpolation", "LINEAR");
                anim.samplers.push_back(std::move(sampler));
            }
        }
        const auto channels = item.find("channels");
        if (channels != item.end() && channels->is_array()) {
            for (std::size_t c = 0; c < channels->size(); ++c) {
                const nlohmann::json& citem = (*channels)[c];
                GltfAnimationChannel channel;
                channel.sampler = GetIntOr(citem, "sampler", -1);
                const auto target = citem.find("target");
                if (target != citem.end() && target->is_object()) {
                    channel.node = GetIntOr(*target, "node", -1);
                    channel.path = GetStringOr(*target, "path", std::string());
                }
                anim.channels.push_back(std::move(channel));
            }
        }
        doc.animations_.push_back(std::move(anim));
    }
    return true;
}

/// 解析场景：确定当前场景的根节点。
///
/// 规范要求 scene 指向 scenes 里的一个条目；如果 scenes 缺失，
/// 则把所有「没有被任何节点当作子节点」的节点当作根节点（等价于规范里的隐式场景）。
/// 解析 scenes。
///
/// 场景里的节点索引必须落在 nodes 数组范围内，校验放在 Validate 里统一做，
/// 这里先原样保留。
static void ParseScenes(const nlohmann::json& root, GltfDocument& doc) {
    const auto scenes = root.find("scenes");
    int sceneIndex = GetIntOr(root, "scene", 0);
    if (scenes != root.end() && scenes->is_array() && !scenes->empty()) {
        if (sceneIndex < 0 || static_cast<std::size_t>(sceneIndex) >= scenes->size()) {
            sceneIndex = 0;
        }
        const nlohmann::json& scene = (*scenes)[static_cast<std::size_t>(sceneIndex)];
        const auto nodes = scene.find("nodes");
        if (nodes != scene.end() && nodes->is_array()) {
            for (const auto& n : *nodes) {
                int v = -1;
                if (JsonToInt(n, v) && v >= 0 &&
                    static_cast<std::size_t>(v) < doc.nodes_.size()) {
                    doc.sceneRoots_.push_back(v);
                }
            }
        }
        return;
    }
    // 没有 scenes：把所有父节点不存在的节点当作根节点。
    std::vector<bool> hasParent(doc.nodes_.size(), false);
    for (const GltfNode& node : doc.nodes_) {
        for (const int child : node.children) {
            if (child >= 0 && static_cast<std::size_t>(child) < hasParent.size()) {
                hasParent[static_cast<std::size_t>(child)] = true;
            }
        }
    }
    for (std::size_t i = 0; i < doc.nodes_.size(); ++i) {
        if (!hasParent[i]) doc.sceneRoots_.push_back(static_cast<int>(i));
    }
}

/// 构造「xxx 的索引 N 越界（共 M 项）」形式的中文错误信息。
static std::string IndexOutOfRange(const char* what, int index, std::size_t count) {
    std::ostringstream oss;
    oss << what << " 的索引 " << index << " 越界（共 " << count << " 项）";
    return oss.str();
}

/// 校验整个文档的索引引用是否合法，并检测节点树里的环。
///
/// glTF 规范要求所有索引（accessor / material / texture / node / skin joint）
/// 必须指向确实存在的元素。越界引用说明文件已损坏或被恶意篡改，
/// 继续解析只会得到毫无意义的几何数据，因此直接判定为解析失败。
///
/// 注意：这里只校验 glTF 核心结构。VRM 扩展里的软引用
/// （humanoid 骨骼节点、springBone 碰撞体、表情绑定的 morph 下标等）
/// 在 VrmModel 层按「丢弃」处理——那些字段即使缺失也只影响部分功能，
/// 不该让整个模型加载失败。
static bool Validate(const GltfDocument& doc, std::string& error) {
    const std::size_t accessorCount = doc.accessors_.size();
    const std::size_t bufferViewCount = doc.bufferViews_.size();
    const std::size_t bufferCount = doc.buffers_.size();
    const std::size_t nodeCount = doc.nodes_.size();
    const std::size_t meshCount = doc.meshes_.size();
    const std::size_t materialCount = doc.materials_.size();
    const std::size_t textureCount = doc.textures_.size();
    const std::size_t imageCount = doc.images_.size();
    const std::size_t samplerCount = doc.samplers_.size();
    const std::size_t skinCount = doc.skins_.size();

    // --- accessor -> bufferView ---
    for (std::size_t i = 0; i < accessorCount; ++i) {
        const int view = doc.accessors_[i].bufferView;
        if (view >= 0 && static_cast<std::size_t>(view) >= bufferViewCount) {
            error = IndexOutOfRange("accessor 的 bufferView", view, bufferViewCount);
            return false;
        }
    }

    // --- bufferView -> buffer ---
    for (std::size_t i = 0; i < bufferViewCount; ++i) {
        const int buffer = doc.bufferViews_[i].buffer;
        if (buffer < 0 || static_cast<std::size_t>(buffer) >= bufferCount) {
            error = IndexOutOfRange("bufferView 的 buffer", buffer, bufferCount);
            return false;
        }
    }

    // --- mesh / primitive ---
    for (std::size_t i = 0; i < meshCount; ++i) {
        const GltfMesh& mesh = doc.meshes_[i];
        for (const GltfPrimitive& primitive : mesh.primitives) {
            const int accessors[] = {
                primitive.positionAccessor, primitive.normalAccessor,
                primitive.texcoordAccessor, primitive.tangentAccessor,
                primitive.colorAccessor, primitive.jointsAccessor,
                primitive.weightsAccessor, primitive.indicesAccessor,
            };
            for (const int accessor : accessors) {
                if (accessor >= 0 && static_cast<std::size_t>(accessor) >= accessorCount) {
                    error = IndexOutOfRange("primitive 的 accessor", accessor, accessorCount);
                    return false;
                }
            }
            for (const GltfPrimitive::MorphTarget& target : primitive.targets) {
                const int targetAccessors[] = {target.position, target.normal, target.tangent};
                for (const int accessor : targetAccessors) {
                    if (accessor >= 0 && static_cast<std::size_t>(accessor) >= accessorCount) {
                        error = IndexOutOfRange("morph target 的 accessor", accessor,
                                                accessorCount);
                        return false;
                    }
                }
            }
            if (primitive.material >= 0 &&
                static_cast<std::size_t>(primitive.material) >= materialCount) {
                error = IndexOutOfRange("primitive 的 material", primitive.material,
                                        materialCount);
                return false;
            }
        }
    }

    // --- node -> mesh / skin / children ---
    for (std::size_t i = 0; i < nodeCount; ++i) {
        const GltfNode& node = doc.nodes_[i];
        if (node.mesh >= 0 && static_cast<std::size_t>(node.mesh) >= meshCount) {
            error = IndexOutOfRange("node 的 mesh", node.mesh, meshCount);
            return false;
        }
        if (node.skin >= 0 && static_cast<std::size_t>(node.skin) >= skinCount) {
            error = IndexOutOfRange("node 的 skin", node.skin, skinCount);
            return false;
        }
        for (const int child : node.children) {
            if (child < 0 || static_cast<std::size_t>(child) >= nodeCount) {
                error = IndexOutOfRange("node 的 children", child, nodeCount);
                return false;
            }
        }
    }

    // --- skin -> joints / accessor / skeleton ---
    for (std::size_t i = 0; i < skinCount; ++i) {
        const GltfSkin& skin = doc.skins_[i];
        for (const int joint : skin.joints) {
            if (joint < 0 || static_cast<std::size_t>(joint) >= nodeCount) {
                error = IndexOutOfRange("skin 的 joints", joint, nodeCount);
                return false;
            }
        }
        if (skin.inverseBindMatricesAccessor >= 0 &&
            static_cast<std::size_t>(skin.inverseBindMatricesAccessor) >= accessorCount) {
            error = IndexOutOfRange("skin 的 inverseBindMatrices",
                                    skin.inverseBindMatricesAccessor, accessorCount);
            return false;
        }
        if (skin.skeleton >= 0 && static_cast<std::size_t>(skin.skeleton) >= nodeCount) {
            error = IndexOutOfRange("skin 的 skeleton", skin.skeleton, nodeCount);
            return false;
        }
    }

    // --- material -> texture ---
    for (const GltfMaterial& material : doc.materials_) {
        const int textures[] = {
            material.baseColorTexture, material.emissiveTexture, material.normalTexture,
            material.occlusionTexture, material.shadeMultiplyTexture,
            material.shadeColorTexture, material.rimMultiplyTexture,
            material.rimColorTexture, material.reflectanceTexture,
        };
        for (const int texture : textures) {
            if (texture >= 0 && static_cast<std::size_t>(texture) >= textureCount) {
                error = IndexOutOfRange("material 的 texture", texture, textureCount);
                return false;
            }
        }
    }

    // --- texture -> image / sampler ---
    for (const GltfTexture& texture : doc.textures_) {
        if (texture.source >= 0 && static_cast<std::size_t>(texture.source) >= imageCount) {
            error = IndexOutOfRange("texture 的 source", texture.source, imageCount);
            return false;
        }
        if (texture.sampler >= 0 && static_cast<std::size_t>(texture.sampler) >= samplerCount) {
            error = IndexOutOfRange("texture 的 sampler", texture.sampler, samplerCount);
            return false;
        }
    }

    // --- image -> bufferView ---
    for (const GltfImage& image : doc.images_) {
        if (image.bufferView >= 0 &&
            static_cast<std::size_t>(image.bufferView) >= bufferViewCount) {
            error = IndexOutOfRange("image 的 bufferView", image.bufferView, bufferViewCount);
            return false;
        }
    }

    // --- animation ---
    for (const GltfAnimation& animation : doc.animations_) {
        for (const GltfAnimationSampler& sampler : animation.samplers) {
            if (sampler.input >= 0 &&
                static_cast<std::size_t>(sampler.input) >= accessorCount) {
                error = IndexOutOfRange("animation sampler 的 input", sampler.input,
                                        accessorCount);
                return false;
            }
            if (sampler.output >= 0 &&
                static_cast<std::size_t>(sampler.output) >= accessorCount) {
                error = IndexOutOfRange("animation sampler 的 output", sampler.output,
                                        accessorCount);
                return false;
            }
        }
        for (const GltfAnimationChannel& channel : animation.channels) {
            if (channel.sampler >= 0 &&
                static_cast<std::size_t>(channel.sampler) >= animation.samplers.size()) {
                error = IndexOutOfRange("animation channel 的 sampler", channel.sampler,
                                        animation.samplers.size());
                return false;
            }
            if (channel.node >= 0 && static_cast<std::size_t>(channel.node) >= nodeCount) {
                error = IndexOutOfRange("animation channel 的 node", channel.node, nodeCount);
                return false;
            }
        }
    }

    // --- scene -> node ---
    //
    // 这里直接读原始 JSON，而不是用 ParseScenes 过滤后的结果：
    // ParseScenes 会把越界的场景节点悄悄丢掉，那样就永远查不出问题文件了。
    {
        const auto scenes = doc.json_.find("scenes");
        const auto sceneIt = doc.json_.find("scene");
        if (sceneIt != doc.json_.end() && sceneIt->is_number()) {
            int sceneIndex = 0;
            if (JsonToInt(*sceneIt, sceneIndex)) {
                std::size_t sceneCount = 0;
                if (scenes != doc.json_.end() && scenes->is_array()) {
                    sceneCount = scenes->size();
                }
                if (sceneIndex < 0 || static_cast<std::size_t>(sceneIndex) >= sceneCount) {
                    error = IndexOutOfRange("glTF 的 scene", sceneIndex, sceneCount);
                    return false;
                }
            }
        }
        if (scenes != doc.json_.end() && scenes->is_array()) {
            for (const auto& scene : *scenes) {
                if (!scene.is_object()) continue;
                const auto nodes = scene.find("nodes");
                if (nodes == scene.end() || !nodes->is_array()) continue;
                for (const auto& n : *nodes) {
                    int v = -1;
                    if (!JsonToInt(n, v)) continue;
                    if (v < 0 || static_cast<std::size_t>(v) >= nodeCount) {
                        error = IndexOutOfRange("scene 的节点", v, nodeCount);
                        return false;
                    }
                }
            }
        }
    }
    for (const int root : doc.sceneRoots_) {
        if (root < 0 || static_cast<std::size_t>(root) >= nodeCount) {
            error = IndexOutOfRange("scene 的节点", root, nodeCount);
            return false;
        }
    }

    // --- 节点树成环检测 ---
    //
    // glTF 的节点层级必须是一棵树。恶意文件可能让 A 的孩子是 B、B 的孩子是 A，
    // 遍历时就会无限循环。用「三色标记」做迭代式 DFS 检测回边：
    // 0 = 未访问，1 = 在当前路径上，2 = 已完成。
    // 命中「在当前路径上」的节点说明存在回边（环）。
    std::vector<char> color(nodeCount, 0);
    std::vector<std::pair<int, std::size_t>> stack;  // (节点, 下一个要访问的孩子序号)
    for (std::size_t start = 0; start < nodeCount; ++start) {
        if (color[start] != 0) continue;
        stack.clear();
        stack.emplace_back(static_cast<int>(start), 0);
        color[start] = 1;
        while (!stack.empty()) {
            const int node = stack.back().first;
            std::size_t& nextChild = stack.back().second;
            const std::vector<int>& children = doc.nodes_[static_cast<std::size_t>(node)].children;
            if (nextChild < children.size()) {
                const int child = children[nextChild++];
                const std::size_t idx = static_cast<std::size_t>(child);
                if (color[idx] == 1) {
                    error = "节点树存在循环引用";
                    return false;
                }
                if (color[idx] == 0) {
                    color[idx] = 1;
                    stack.emplace_back(child, 0);
                }
                continue;
            }
            color[static_cast<std::size_t>(node)] = 2;
            stack.pop_back();
        }
    }

    return true;
}

/// 解析 GLB 容器，返回 JSON 文本与 BIN 数据。
static bool ParseGlb(const std::vector<unsigned char>& file, std::string& jsonText,
              std::vector<unsigned char>& bin, bool& hasBin, std::string& error) {
    if (file.size() < 12) {
        error = "GLB 文件太短（头部至少 12 字节）";
        return false;
    }
    std::uint32_t magic = 0, version = 0, length = 0;
    std::memcpy(&magic, file.data(), 4);
    std::memcpy(&version, file.data() + 4, 4);
    std::memcpy(&length, file.data() + 8, 4);
    if (magic != kGlbMagic) {
        error = "不是有效的 GLB 文件（文件头标识应为 \"glTF\"）";
        return false;
    }
    if (version != 2) {
        std::ostringstream oss;
        oss << "GLB 版本号不支持（" << version << "，仅支持 2）";
        error = oss.str();
        return false;
    }
    if (length < 12 || static_cast<std::size_t>(length) > file.size()) {
        error = "GLB 的 length 字段与实际文件大小不符（文件可能被截断）";
        return false;
    }

    std::size_t pos = 12;
    bool gotJson = false;
    while (pos + 8 <= length) {
        std::uint32_t chunkLength = 0, chunkType = 0;
        std::memcpy(&chunkLength, file.data() + pos, 4);
        std::memcpy(&chunkType, file.data() + pos + 4, 4);
        pos += 8;
        if (chunkLength > length - pos) {
            error = "GLB 的 chunk 长度超出文件范围（文件可能被截断）";
            return false;
        }
        if (chunkType == kChunkJson) {
            if (gotJson) {
                error = "GLB 包含多个 JSON chunk";
                return false;
            }
            jsonText.assign(reinterpret_cast<const char*>(file.data() + pos), chunkLength);
            gotJson = true;
        } else if (chunkType == kChunkBin) {
            bin.assign(file.begin() + static_cast<std::ptrdiff_t>(pos),
                       file.begin() + static_cast<std::ptrdiff_t>(pos + chunkLength));
            hasBin = true;
        }
        // chunk 需要 4 字节对齐（chunkLength 本身已含填充字节）。
        pos += chunkLength;
    }
    if (!gotJson) {
        error = "GLB 缺少 JSON chunk";
        return false;
    }
    return true;
}

/// 去掉 JSON chunk 末尾的填充字节（空格 0x20 与结尾的 0x00）。
static std::string TrimJsonPadding(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0) {
        const char c = text[end - 1];
        if (c == ' ' || c == '\0' || c == '\n' || c == '\r' || c == '\t') {
            --end;
        } else {
            break;
        }
    }
    return text.substr(0, end);
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

static bool Load(const std::string& path, GltfDocument& out, std::string& error) {
    out = GltfDocument();
    error.clear();

    std::vector<unsigned char> file;
    if (!ReadWholeFile(path, file, error)) return false;
    if (file.empty()) {
        error = "文件为空：" + path;
        return false;
    }

    out.baseDir_ = DirNameOf(path);

    std::string jsonText;
    std::vector<unsigned char> glbBin;
    bool hasGlbBin = false;
    const bool isGlb = file.size() >= 4 && file[0] == 'g' && file[1] == 'l' && file[2] == 'T' &&
                       file[3] == 'F';
    if (isGlb) {
        if (!ParseGlb(file, jsonText, glbBin, hasGlbBin, error)) return false;
        jsonText = TrimJsonPadding(jsonText);
    } else {
        jsonText.assign(reinterpret_cast<const char*>(file.data()), file.size());
    }
    if (jsonText.size() > kMaxJsonSize) {
        error = "JSON 数据过大，已拒绝加载";
        return false;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(jsonText);
    } catch (const nlohmann::json::exception& e) {
        // 这里必须捕获 json::exception 的基类而不是只捕获 parse_error：
        // nlohmann 在遇到「数值溢出」（例如 -4E371138828673793 这种
        // 超出 double 表示范围的字面量）时抛的是 out_of_range，
        // 它不是 parse_error 的子类。只捕获 parse_error 会让畸形文件
        // 直接触发 std::terminate 终止进程。
        error = std::string("JSON 解析失败：") + e.what();
        return false;
    }
    if (!root.is_object()) {
        error = "glTF 的根节点不是 JSON 对象";
        return false;
    }
    out.json_ = std::move(root);

    // 解析过程中 nlohmann 在遇到类型不符时可能抛异常。
    // 这里统一兜底成「解析失败」，保证来自互联网的畸形文件不会让程序崩溃。
    try {
        if (!ParseBuffers(out.json_, out.baseDir_, glbBin, hasGlbBin, out, error)) {
            return false;
        }
        if (!ParseBufferViews(out.json_, out, error)) return false;
        if (!ParseAccessors(out.json_, out, error)) return false;
        if (!ParseMeshes(out.json_, out, error)) return false;
        if (!ParseNodes(out.json_, out, error)) return false;
        if (!ParseSkins(out.json_, out, error)) return false;
        if (!ParseMaterials(out.json_, out, error)) return false;
        if (!ParseTextures(out.json_, out, error)) return false;
        if (!ParseAnimations(out.json_, out, error)) return false;
        ParseScenes(out.json_, out);
        // 全部结构解析完后统一做索引范围校验与成环检测。
        // 这一步放在最后是因为各数组的长度此时才全部确定。
        if (!Validate(out, error)) return false;
    } catch (const nlohmann::json::exception& e) {
        error = std::string("glTF 数据结构异常：") + e.what();
        return false;
    }
    return true;
}

};  // struct GltfParserAccess

bool LoadGltf(const std::string& path, GltfDocument& out, std::string& error) {
    return GltfParserAccess::Load(path, out, error);
}

}  // namespace ppet::vrm
