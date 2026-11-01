// PPet - C++ 移植版
// PMX 2.0 / 2.1 二进制解析器实现。
//
// 关于「骨骼 append transform」的读取顺序（任务书特别要求写清依据）：
// 任务书给出的顺序（"(flags & 0x0200)==0 且 (...): 读 byte appendLocalAccess，
// 读 int32 externalKey"）来自 PMD / PMX-Editor 的插件格式，与 MMD 实际使用的
// .pmx 文件格式不同。真实 .pmx 的骨骼可选段顺序由两个独立来源交叉确认：
//   - felixjones 的 PMX 2.1 规范 "Bone Data" 表：
//       Tail position -> Inherit bone -> Fixed axis -> Local co-ordinate
//                     -> External parent -> IK
//   - mmd-parser v1.0.0（three.js 生产环境使用）parseBone() 的实际读取顺序：
//       connectIndex|offsetPosition -> grant -> fixAxis -> localX/localZ
//                              -> key(0x2000) -> ik(0x20)
// 两者一致，且能在真实样本 miku.pmx 上完整解析到底，故按此实现。
// 特别注意：externalKey（0x2000）的读取位置在 IK 段「之前」，与任务书描述相反。
//
// 健壮性约定：
//   - 所有读取都经过 ByteReader，越界立即失败并返回中文错误，不做越界访问。
//   - count 字段既做绝对上限检查，也与「剩余字节数 / 最小单元字节数」交叉校验，
//     防止恶意文件用超大 count 触发巨额内存分配。
//   - 所有索引读入后立即做范围校验：越界的引用被丢弃（置 -1）或丢弃整个三角形，
//     绝不用于数组下标访问。
#include "model/pmx/PmxParser.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace ppet::pmx {
namespace {

// ---------------------------------------------------------------------------
// 上限常量：防止恶意 / 损坏文件触发超大分配或超长循环。
// ---------------------------------------------------------------------------
constexpr std::uint32_t kMaxVertices = 10000000;
constexpr std::uint32_t kMaxFaces = 10000000;
constexpr std::uint32_t kMaxTextures = 100000;
constexpr std::uint32_t kMaxMaterials = 1000000;
constexpr std::uint32_t kMaxBones = 1000000;
constexpr std::uint32_t kMaxMorphs = 1000000;
constexpr std::uint32_t kMaxMorphElements = 10000000;
constexpr std::uint32_t kMaxDisplayFrames = 1000000;
constexpr std::uint32_t kMaxRigidBodies = 1000000;
constexpr std::uint32_t kMaxJoints = 1000000;
constexpr std::uint32_t kMaxIkLinks = 1000000;
/// 单个文本字段最大字节数（16 MB 足够容纳任何合法的名字 / 注释）。
constexpr std::int32_t kMaxTextBytes = 16 * 1024 * 1024;
constexpr std::uint8_t kMaxAdditionalUv = 4;

/// 顶点索引无效标记。
constexpr std::uint32_t kInvalidVertexIndex = 0xFFFFFFFFu;

/// 索引宽度合法值。
bool IsValidIndexSize(std::uint8_t s) { return s == 1 || s == 2 || s == 4; }

// ---------------------------------------------------------------------------
// ByteReader：小端二进制读取器，所有读取都做边界检查。
// ---------------------------------------------------------------------------
class ByteReader {
public:
    ByteReader(const unsigned char* data, std::size_t size) : data_(data), size_(size) {}

    std::size_t Remaining() const { return size_ - pos_; }
    std::size_t Position() const { return pos_; }
    bool Failed() const { return failed_; }

    /// 记录错误（只保留第一条）。
    bool Fail(const std::string& message) {
        if (!failed_) {
            failed_ = true;
            error_ = message;
        }
        return false;
    }

    const std::string& Error() const { return error_; }

    bool ReadBytes(void* dst, std::size_t n) {
        if (failed_) return false;
        if (n > Remaining()) {
            std::ostringstream oss;
            oss << "文件意外结束：需要 " << n << " 字节，仅剩 " << Remaining() << " 字节";
            return Fail(oss.str());
        }
        std::memcpy(dst, data_ + pos_, n);
        pos_ += n;
        return true;
    }

    bool ReadU8(std::uint8_t& v) { return ReadBytes(&v, 1); }

    bool ReadU16(std::uint16_t& v) {
        unsigned char b[2];
        if (!ReadBytes(b, 2)) return false;
        v = static_cast<std::uint16_t>(b[0] | (static_cast<std::uint16_t>(b[1]) << 8));
        return true;
    }

    bool ReadU32(std::uint32_t& v) {
        unsigned char b[4];
        if (!ReadBytes(b, 4)) return false;
        v = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
            (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        return true;
    }

    bool ReadI32(std::int32_t& v) {
        std::uint32_t u = 0;
        if (!ReadU32(u)) return false;
        v = static_cast<std::int32_t>(u);
        return true;
    }

    bool ReadF32(float& v) {
        std::uint32_t u = 0;
        if (!ReadU32(u)) return false;
        std::memcpy(&v, &u, sizeof(v));
        return true;
    }

    bool ReadVec2(float* dst) { return ReadFloats(dst, 2); }
    bool ReadVec3(float* dst) { return ReadFloats(dst, 3); }
    bool ReadVec4(float* dst) { return ReadFloats(dst, 4); }

    bool ReadFloats(float* dst, int n) {
        for (int i = 0; i < n; ++i) {
            if (!ReadF32(dst[i])) return false;
        }
        return true;
    }

    /// 读取有符号索引（骨骼 / 贴图 / 材质 / morph / 刚体索引，-1 表示空）。
    bool ReadSignedIndex(std::uint8_t indexSize, int& out) {
        out = -1;
        switch (indexSize) {
            case 1: {
                std::uint8_t v = 0;
                if (!ReadU8(v)) return false;
                out = static_cast<std::int8_t>(v);
                return true;
            }
            case 2: {
                std::uint16_t v = 0;
                if (!ReadU16(v)) return false;
                out = static_cast<std::int16_t>(v);
                return true;
            }
            case 4: {
                std::int32_t v = 0;
                if (!ReadI32(v)) return false;
                out = v;
                return true;
            }
            default:
                return Fail("索引宽度非法（只允许 1 / 2 / 4 字节）");
        }
    }

    /// 读取无符号索引（顶点索引，PMX 中顶点索引不区分正负）。
    bool ReadUnsignedIndex(std::uint8_t indexSize, std::uint32_t& out) {
        out = 0;
        switch (indexSize) {
            case 1: {
                std::uint8_t v = 0;
                if (!ReadU8(v)) return false;
                out = v;
                return true;
            }
            case 2: {
                std::uint16_t v = 0;
                if (!ReadU16(v)) return false;
                out = v;
                return true;
            }
            case 4: {
                std::uint32_t v = 0;
                if (!ReadU32(v)) return false;
                out = v;
                return true;
            }
            default:
                return Fail("索引宽度非法（只允许 1 / 2 / 4 字节）");
        }
    }

private:
    const unsigned char* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pos_ = 0;
    bool failed_ = false;
    std::string error_;
};

// ---------------------------------------------------------------------------
// 文本解码
// ---------------------------------------------------------------------------

/// 把一个 Unicode 码点编码为 UTF-8，追加到 out。
void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// UTF-16LE -> UTF-8。处理代理对，孤立代理项替换为 U+FFFD。
std::string Utf16LeToUtf8(const unsigned char* bytes, std::size_t byteLen) {
    std::string out;
    out.reserve(byteLen);
    const std::size_t units = byteLen / 2;
    for (std::size_t i = 0; i < units; ++i) {
        std::uint32_t unit = static_cast<std::uint32_t>(bytes[i * 2]) |
                             (static_cast<std::uint32_t>(bytes[i * 2 + 1]) << 8);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            // 高代理：尝试与下一个低代理配对。
            if (i + 1 < units) {
                std::uint32_t low = static_cast<std::uint32_t>(bytes[(i + 1) * 2]) |
                                    (static_cast<std::uint32_t>(bytes[(i + 1) * 2 + 1]) << 8);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    const std::uint32_t cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    AppendUtf8(out, cp);
                    ++i;
                    continue;
                }
            }
            AppendUtf8(out, 0xFFFD);
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            AppendUtf8(out, 0xFFFD);  // 孤立低代理
            continue;
        }
        AppendUtf8(out, unit);
    }
    return out;
}

/// UTF-8 -> UTF-8（校验并把非法序列替换为 U+FFFD）。
std::string SanitizeUtf8(const unsigned char* bytes, std::size_t len) {
    std::string out;
    out.reserve(len);
    std::size_t i = 0;
    while (i < len) {
        const unsigned char c = bytes[i];
        int extra = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            AppendUtf8(out, 0xFFFD);
            ++i;
            continue;
        }
        if (i + static_cast<std::size_t>(extra) >= len) {
            AppendUtf8(out, 0xFFFD);
            break;
        }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc = bytes[i + static_cast<std::size_t>(k)];
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        // 拒绝过长编码与代理区码点。
        if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            AppendUtf8(out, 0xFFFD);
            ++i;
            continue;
        }
        out.append(reinterpret_cast<const char*>(bytes + i), static_cast<std::size_t>(extra) + 1);
        i += static_cast<std::size_t>(extra) + 1;
    }
    return out;
}

/// 读取一个文本字段：int32 字节长度 + 原始字节。
/// PMX 的长度是「字节数」而非字符数（UTF-16LE 时为 2 * 字符数）。
bool ReadText(ByteReader& r, bool utf16, std::string& out) {
    std::int32_t len = 0;
    if (!r.ReadI32(len)) return false;
    if (len < 0) {
        return r.Fail("文本长度非法（为负数）");
    }
    if (len > kMaxTextBytes) {
        return r.Fail("文本长度超出允许范围");
    }
    if (static_cast<std::size_t>(len) > r.Remaining()) {
        std::ostringstream oss;
        oss << "文件意外结束：文本需要 " << len << " 字节，仅剩 " << r.Remaining() << " 字节";
        return r.Fail(oss.str());
    }
    std::string buffer;
    buffer.resize(static_cast<std::size_t>(len));
    if (len > 0 && !r.ReadBytes(&buffer[0], static_cast<std::size_t>(len))) {
        return false;
    }
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(buffer.data());
    out = utf16 ? Utf16LeToUtf8(raw, static_cast<std::size_t>(len))
                 : SanitizeUtf8(raw, static_cast<std::size_t>(len));
    return true;
}

// ---------------------------------------------------------------------------
// count 校验
// ---------------------------------------------------------------------------

/// 校验元素个数：既不能超过硬上限，也不能超过「剩余字节数 / 最小单元字节数」。
bool CheckCount(ByteReader& r, std::int32_t count, std::size_t minElemBytes,
                std::uint32_t maxCount, const char* what) {
    if (count < 0) {
        std::ostringstream oss;
        oss << what << " 数量非法（为负数）";
        return r.Fail(oss.str());
    }
    const std::uint32_t c = static_cast<std::uint32_t>(count);
    if (c > maxCount) {
        std::ostringstream oss;
        oss << what << " 数量 " << c << " 超出上限 " << maxCount << "（文件可能已损坏）";
        return r.Fail(oss.str());
    }
    if (minElemBytes > 0 && c > r.Remaining() / minElemBytes) {
        std::ostringstream oss;
        oss << what << " 数量 " << c << " 与文件剩余大小不符（文件可能已损坏）";
        return r.Fail(oss.str());
    }
    return true;
}

// ---------------------------------------------------------------------------
// 各段解析
// ---------------------------------------------------------------------------

struct PmxGlobals {
    std::uint8_t encoding = 0;
    std::uint8_t additionalUv = 0;
    std::uint8_t vertexIndexSize = 1;
    std::uint8_t textureIndexSize = 1;
    std::uint8_t materialIndexSize = 1;
    std::uint8_t boneIndexSize = 1;
    std::uint8_t morphIndexSize = 1;
    std::uint8_t rigidBodyIndexSize = 1;
};

bool ParseHeader(ByteReader& r, PmxDocument& doc, PmxGlobals& g, std::string& error) {
    char magic[4] = {0, 0, 0, 0};
    if (!r.ReadBytes(magic, 4)) {
        error = r.Error();
        return false;
    }
    if (std::memcmp(magic, "PMX ", 4) != 0) {
        error = "不是有效的 PMX 文件（文件头标识应为 \"PMX \"）";
        return false;
    }
    if (!r.ReadF32(doc.version)) {
        error = r.Error();
        return false;
    }
    // 兼容 2.0 / 2.1；其它版本理论上也可解析，但这里只做告警不阻断。
    if (doc.version < 1.0f || doc.version > 3.0f) {
        std::ostringstream oss;
        oss << "PMX 版本号异常（" << doc.version << "），继续尝试解析";
        error = oss.str();
    }

    std::uint8_t globalsCount = 0;
    if (!r.ReadU8(globalsCount)) {
        error = r.Error();
        return false;
    }
    if (globalsCount < 8) {
        error = "PMX 文件头缺少必要的全局设置字节（至少需要 8 个）";
        return false;
    }
    std::vector<std::uint8_t> globals(globalsCount);
    if (!r.ReadBytes(globals.data(), globalsCount)) {
        error = r.Error();
        return false;
    }
    g.encoding = globals[0];
    g.additionalUv = globals[1];
    g.vertexIndexSize = globals[2];
    g.textureIndexSize = globals[3];
    g.materialIndexSize = globals[4];
    g.boneIndexSize = globals[5];
    g.morphIndexSize = globals[6];
    g.rigidBodyIndexSize = globals[7];

    if (g.encoding > 1) {
        error = "PMX 文本编码标记非法（只支持 UTF-16LE / UTF-8）";
        return false;
    }
    if (g.additionalUv > kMaxAdditionalUv) {
        error = "PMX 附加 UV 数量超出范围（最多 4 组）";
        return false;
    }
    if (!IsValidIndexSize(g.vertexIndexSize) || !IsValidIndexSize(g.textureIndexSize) ||
        !IsValidIndexSize(g.materialIndexSize) || !IsValidIndexSize(g.boneIndexSize) ||
        !IsValidIndexSize(g.morphIndexSize) || !IsValidIndexSize(g.rigidBodyIndexSize)) {
        error = "PMX 索引宽度非法（只允许 1 / 2 / 4 字节）";
        return false;
    }
    doc.utf16 = (g.encoding == 0);

    if (!ReadText(r, doc.utf16, doc.name) || !ReadText(r, doc.utf16, doc.nameEn) ||
        !ReadText(r, doc.utf16, doc.comment) || !ReadText(r, doc.utf16, doc.commentEn)) {
        error = r.Error();
        return false;
    }
    return true;
}

bool ParseVertices(ByteReader& r, const PmxGlobals& g, PmxDocument& doc, std::string& error) {
    std::int32_t count = 0;
    if (!r.ReadI32(count)) {
        error = r.Error();
        return false;
    }
    // 顶点最小字节数：position(12) + normal(12) + uv(8) + 权重类型(1) + 边缘缩放(4) = 37。
    if (!CheckCount(r, count, 37, kMaxVertices, "顶点")) {
        error = r.Error();
        return false;
    }
    doc.vertices.resize(static_cast<std::size_t>(count));

    for (std::int32_t vi = 0; vi < count; ++vi) {
        PmxVertex& v = doc.vertices[static_cast<std::size_t>(vi)];
        if (!r.ReadVec3(v.position.data()) || !r.ReadVec3(v.normal.data()) ||
            !r.ReadVec2(v.uv.data())) {
            error = r.Error();
            return false;
        }
        if (g.additionalUv > 0) {
            v.additionalUvs.resize(g.additionalUv);
            for (std::uint8_t k = 0; k < g.additionalUv; ++k) {
                if (!r.ReadVec4(v.additionalUvs[k].data())) {
                    error = r.Error();
                    return false;
                }
            }
        }

        std::uint8_t weightType = 0;
        if (!r.ReadU8(weightType)) {
            error = r.Error();
            return false;
        }
        v.weightType = static_cast<PmxWeightType>(weightType);

        switch (weightType) {
            case 0: {  // BDEF1
                int idx = -1;
                if (!r.ReadSignedIndex(g.boneIndexSize, idx)) {
                    error = r.Error();
                    return false;
                }
                v.boneIndices[0] = idx;
                v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
                break;
            }
            case 1: {  // BDEF2
                int i0 = -1, i1 = -1;
                float w0 = 0.0f;
                if (!r.ReadSignedIndex(g.boneIndexSize, i0) ||
                    !r.ReadSignedIndex(g.boneIndexSize, i1) || !r.ReadF32(w0)) {
                    error = r.Error();
                    return false;
                }
                v.boneIndices[0] = i0;
                v.boneIndices[1] = i1;
                v.weights[0] = w0;
                v.weights[1] = 1.0f - w0;
                break;
            }
            case 2: {  // BDEF4
                for (int k = 0; k < 4; ++k) {
                    if (!r.ReadSignedIndex(g.boneIndexSize, v.boneIndices[static_cast<std::size_t>(k)])) {
                        error = r.Error();
                        return false;
                    }
                }
                for (int k = 0; k < 4; ++k) {
                    if (!r.ReadF32(v.weights[static_cast<std::size_t>(k)])) {
                        error = r.Error();
                        return false;
                    }
                }
                break;
            }
            case 3: {  // SDEF
                int i0 = -1, i1 = -1;
                float w0 = 0.0f;
                if (!r.ReadSignedIndex(g.boneIndexSize, i0) ||
                    !r.ReadSignedIndex(g.boneIndexSize, i1) || !r.ReadF32(w0) ||
                    !r.ReadVec3(v.sdefC.data()) || !r.ReadVec3(v.sdefR0.data()) ||
                    !r.ReadVec3(v.sdefR1.data())) {
                    error = r.Error();
                    return false;
                }
                v.boneIndices[0] = i0;
                v.boneIndices[1] = i1;
                v.weights[0] = w0;
                v.weights[1] = 1.0f - w0;
                break;
            }
            case 4: {  // QDEF（PMX 2.1）
                for (int k = 0; k < 4; ++k) {
                    if (!r.ReadSignedIndex(g.boneIndexSize, v.boneIndices[static_cast<std::size_t>(k)])) {
                        error = r.Error();
                        return false;
                    }
                }
                for (int k = 0; k < 4; ++k) {
                    if (!r.ReadF32(v.weights[static_cast<std::size_t>(k)])) {
                        error = r.Error();
                        return false;
                    }
                }
                break;
            }
            default: {
                std::ostringstream oss;
                oss << "顶点权重类型非法（" << static_cast<int>(weightType) << "）";
                error = oss.str();
                return false;
            }
        }

        if (!r.ReadF32(v.edgeScale)) {
            error = r.Error();
            return false;
        }
    }
    return true;
}

bool ParseFaces(ByteReader& r, const PmxGlobals& g, PmxDocument& doc, std::string& error) {
    std::int32_t indexCount = 0;
    if (!r.ReadI32(indexCount)) {
        error = r.Error();
        return false;
    }
    if (indexCount < 0) {
        error = "面索引数量非法（为负数）";
        return false;
    }
    if (static_cast<std::uint32_t>(indexCount) > kMaxFaces) {
        error = "面索引数量超出上限（文件可能已损坏）";
        return false;
    }
    if (indexCount % 3 != 0) {
        error = "面索引数量不是 3 的倍数（文件可能已损坏）";
        return false;
    }
    const std::size_t triangleCount = static_cast<std::size_t>(indexCount / 3);
    if (triangleCount > r.Remaining() / 3) {
        error = "面索引数量与文件剩余大小不符（文件可能已损坏）";
        return false;
    }
    doc.faces.reserve(triangleCount);

    const std::size_t vertexCount = doc.vertices.size();
    for (std::size_t t = 0; t < triangleCount; ++t) {
        std::array<std::uint32_t, 3> tri{};
        bool valid = true;
        for (int k = 0; k < 3; ++k) {
            std::uint32_t idx = 0;
            if (!r.ReadUnsignedIndex(g.vertexIndexSize, idx)) {
                error = r.Error();
                return false;
            }
            // 顶点索引越界的三角形标记为无效并原位保留。
            // 不能直接丢弃：材质的面数区间是按「文件中的三角形顺序」划分的，
            // 丢弃会让后续所有材质的区间整体错位。
            if (idx >= vertexCount) {
                valid = false;
                idx = kInvalidVertexIndex;
            }
            tri[static_cast<std::size_t>(k)] = idx;
        }
        if (!valid) {
            ++doc.droppedFaces;
            tri = {kInvalidVertexIndex, kInvalidVertexIndex, kInvalidVertexIndex};
        }
        doc.faces.push_back(tri);
    }
    return true;
}

bool ParseTextures(ByteReader& r, PmxDocument& doc, std::string& error) {
    std::int32_t count = 0;
    if (!r.ReadI32(count)) {
        error = r.Error();
        return false;
    }
    // 贴图路径至少有 4 字节长度前缀。
    if (!CheckCount(r, count, 4, kMaxTextures, "贴图")) {
        error = r.Error();
        return false;
    }
    doc.textures.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        std::string path;
        if (!ReadText(r, doc.utf16, path)) {
            error = r.Error();
            return false;
        }
        doc.textures.push_back(std::move(path));
    }
    return true;
}

bool ParseMaterials(ByteReader& r, const PmxGlobals& g, PmxDocument& doc, std::string& error) {
    std::int32_t count = 0;
    if (!r.ReadI32(count)) {
        error = r.Error();
        return false;
    }
    // 材质最小字节数（两个名字与注释各 4 字节长度前缀，索引按 1 字节算）：
    // 4+4 + 4 + 16(diffuse) + 12(specular) + 4(shininess) + 12(ambient) + 1(flags)
    //   + 16(edgeColor) + 4(edgeSize) + 1(tex) + 1(sphere) + 1(sphereMode)
    //   + 1(toonFlag) + 1(toonValue) + 4(comment) + 4(faceCount)
    if (!CheckCount(r, count, 86, kMaxMaterials, "材质")) {
        error = r.Error();
        return false;
    }
    doc.materials.reserve(static_cast<std::size_t>(count));

    const std::int32_t textureCount = static_cast<std::int32_t>(doc.textures.size());
    for (std::int32_t i = 0; i < count; ++i) {
        PmxMaterial m;
        if (!ReadText(r, doc.utf16, m.name) || !ReadText(r, doc.utf16, m.nameEn)) {
            error = r.Error();
            return false;
        }
        if (!r.ReadVec4(m.diffuse.data()) || !r.ReadVec3(m.specular.data()) ||
            !r.ReadF32(m.shininess) || !r.ReadVec3(m.ambient.data()) || !r.ReadU8(m.flags) ||
            !r.ReadVec4(m.edgeColor.data()) || !r.ReadF32(m.edgeSize)) {
            error = r.Error();
            return false;
        }
        int textureIndex = -1;
        int sphereIndex = -1;
        if (!r.ReadSignedIndex(g.textureIndexSize, textureIndex) ||
            !r.ReadSignedIndex(g.textureIndexSize, sphereIndex) || !r.ReadU8(m.sphereMode) ||
            !r.ReadU8(m.toonFlag)) {
            error = r.Error();
            return false;
        }
        if (m.toonFlag == 0) {
            int toonTex = -1;
            if (!r.ReadSignedIndex(g.textureIndexSize, toonTex)) {
                error = r.Error();
                return false;
            }
            // 独立 toon 贴图：走贴图索引表，需要范围校验。
            m.toonIndex = (toonTex >= 0 && toonTex < textureCount) ? toonTex : -1;
        } else if (m.toonFlag == 1) {
            std::int8_t sharedToon = 0;
            if (!r.ReadBytes(&sharedToon, 1)) {
                error = r.Error();
                return false;
            }
            // 内置共享 toon 只允许 0~9。
            m.toonIndex = (sharedToon >= 0 && sharedToon <= 9) ? sharedToon : -1;
        } else {
            error = "材质的 toon 引用方式非法（只支持 0 / 1）";
            return false;
        }
        if (!ReadText(r, doc.utf16, m.comment)) {
            error = r.Error();
            return false;
        }
        std::int32_t faceCount = 0;
        if (!r.ReadI32(faceCount)) {
            error = r.Error();
            return false;
        }
        if (faceCount < 0) {
            error = "材质面数非法（为负数）";
            return false;
        }
        m.faceCount = static_cast<std::uint32_t>(faceCount);

        // 贴图索引范围校验：越界引用直接丢弃。
        m.textureIndex = (textureIndex >= 0 && textureIndex < textureCount) ? textureIndex : -1;
        m.sphereTextureIndex = (sphereIndex >= 0 && sphereIndex < textureCount) ? sphereIndex : -1;

        doc.materials.push_back(std::move(m));
    }
    return true;
}

bool ParseBones(ByteReader& r, const PmxGlobals& g, PmxDocument& doc, std::string& error) {
    std::int32_t count = 0;
    if (!r.ReadI32(count)) {
        error = r.Error();
        return false;
    }
    // 骨骼最小字节数：两个文本长度(8) + 位置(12) + 父索引(1) + 层(4) + flags(2) = 27。
    if (!CheckCount(r, count, 27, kMaxBones, "骨骼")) {
        error = r.Error();
        return false;
    }
    doc.bones.reserve(static_cast<std::size_t>(count));

    for (std::int32_t i = 0; i < count; ++i) {
        PmxBone b;
        if (!ReadText(r, doc.utf16, b.name) || !ReadText(r, doc.utf16, b.nameEn) ||
            !r.ReadVec3(b.position.data())) {
            error = r.Error();
            return false;
        }
        int parent = -1;
        if (!r.ReadSignedIndex(g.boneIndexSize, parent) || !r.ReadU32(b.layer) ||
            !r.ReadU16(b.flags)) {
            error = r.Error();
            return false;
        }
        b.parentIndex = parent;

        if (b.flags & kBoneIndexedTail) {
            int tail = -1;
            if (!r.ReadSignedIndex(g.boneIndexSize, tail)) {
                error = r.Error();
                return false;
            }
            b.tailIndex = tail;
        } else {
            if (!r.ReadVec3(b.tailOffset.data())) {
                error = r.Error();
                return false;
            }
        }

        if (b.flags & (kBoneInheritRotation | kBoneInheritTranslation)) {
            int grantParent = -1;
            if (!r.ReadSignedIndex(g.boneIndexSize, grantParent) || !r.ReadF32(b.grantRatio)) {
                error = r.Error();
                return false;
            }
            b.hasGrant = true;
            b.grantParentIndex = grantParent;
        }

        if (b.flags & kBoneFixedAxis) {
            if (!r.ReadVec3(b.fixedAxis.data())) {
                error = r.Error();
                return false;
            }
            b.hasFixedAxis = true;
        }
        if (b.flags & kBoneLocalAxis) {
            if (!r.ReadVec3(b.localXAxis.data()) || !r.ReadVec3(b.localZAxis.data())) {
                error = r.Error();
                return false;
            }
            b.hasLocalAxis = true;
        }
        if (b.flags & kBoneExternalParentDeform) {
            if (!r.ReadU32(b.externalKey)) {
                error = r.Error();
                return false;
            }
        }
        if (b.flags & kBoneIk) {
            int effector = -1;
            std::uint32_t iteration = 0;
            float limitAngle = 0.0f;
            std::int32_t linkCount = 0;
            if (!r.ReadSignedIndex(g.boneIndexSize, effector) || !r.ReadU32(iteration) ||
                !r.ReadF32(limitAngle) || !r.ReadI32(linkCount)) {
                error = r.Error();
                return false;
            }
            if (linkCount < 0 || static_cast<std::uint32_t>(linkCount) > kMaxIkLinks) {
                error = "IK 链条目数量非法";
                return false;
            }
            b.hasIk = true;
            b.ikTargetIndex = effector;
            b.ikIteration = iteration;
            b.ikLimitAngle = limitAngle;
            b.ikLinks.reserve(static_cast<std::size_t>(linkCount));
            for (std::int32_t k = 0; k < linkCount; ++k) {
                PmxIkLink link;
                int boneIdx = -1;
                if (!r.ReadSignedIndex(g.boneIndexSize, boneIdx)) {
                    error = r.Error();
                    return false;
                }
                link.boneIndex = boneIdx;
                std::uint8_t hasLimit = 0;
                if (!r.ReadU8(hasLimit)) {
                    error = r.Error();
                    return false;
                }
                link.hasLimit = (hasLimit != 0);
                if (link.hasLimit) {
                    if (!r.ReadVec3(link.limitMin.data()) || !r.ReadVec3(link.limitMax.data())) {
                        error = r.Error();
                        return false;
                    }
                }
                b.ikLinks.push_back(link);
            }
        }

        // 父骨骼 / 尾端 / IK 目标 / IK 链的索引全部做范围校验。
        // 注意：范围上限必须用「本段骨骼总数」，不能用已解析的数量——
        // PMX 允许父骨骼下标大于自身，用已解析数量会把合法索引误判为越界。
        const std::int32_t boneCount = count;
        if (b.parentIndex < -1 || b.parentIndex >= boneCount) b.parentIndex = -1;
        if (b.tailIndex < -1 || b.tailIndex >= boneCount) b.tailIndex = -1;
        if (b.hasGrant && (b.grantParentIndex < -1 || b.grantParentIndex >= boneCount)) {
            b.hasGrant = false;
            b.grantParentIndex = -1;
        }
        if (b.hasIk && (b.ikTargetIndex < -1 || b.ikTargetIndex >= boneCount)) {
            b.ikTargetIndex = -1;
        }
        for (PmxIkLink& link : b.ikLinks) {
            if (link.boneIndex < -1 || link.boneIndex >= boneCount) link.boneIndex = -1;
        }

        doc.bones.push_back(std::move(b));
    }
    return true;
}

bool ParseMorphs(ByteReader& r, const PmxGlobals& g, PmxDocument& doc, std::string& error) {
    std::int32_t count = 0;
    if (!r.ReadI32(count)) {
        error = r.Error();
        return false;
    }
    // morph 最小字节数：两个文本长度(8) + panel(1) + type(1) + 元素数(4)。
    if (!CheckCount(r, count, 14, kMaxMorphs, "morph")) {
        error = r.Error();
        return false;
    }
    doc.morphs.reserve(static_cast<std::size_t>(count));

    // 索引范围校验必须用「本段总数」，不能用已解析的数量：
    // PMX 允许引用尚未解析到的下标（例如组 morph 引用后面的 morph）。
    const std::int32_t morphCount = count;
    const std::int32_t vertexCount = static_cast<std::int32_t>(doc.vertices.size());
    const std::int32_t boneCount = static_cast<std::int32_t>(doc.bones.size());
    const std::int32_t materialCount = static_cast<std::int32_t>(doc.materials.size());
    const std::int32_t rigidBodyCount = static_cast<std::int32_t>(doc.rigidBodies.size());

    for (std::int32_t i = 0; i < count; ++i) {
        PmxMorph m;
        if (!ReadText(r, doc.utf16, m.name) || !ReadText(r, doc.utf16, m.nameEn) ||
            !r.ReadU8(m.panel)) {
            error = r.Error();
            return false;
        }
        std::uint8_t morphType = 0;
        if (!r.ReadU8(morphType)) {
            error = r.Error();
            return false;
        }
        m.type = static_cast<PmxMorphType>(morphType);
        std::int32_t elementCount = 0;
        if (!r.ReadI32(elementCount)) {
            error = r.Error();
            return false;
        }
        if (elementCount < 0 || static_cast<std::uint32_t>(elementCount) > kMaxMorphElements) {
            error = "morph 元素数量非法";
            return false;
        }
        m.elementCount = static_cast<std::uint32_t>(elementCount);
        m.elements.reserve(static_cast<std::size_t>(elementCount));

        for (std::int32_t k = 0; k < elementCount; ++k) {
            PmxMorphElement e;
            switch (morphType) {
                case 0: {  // 组 morph：引用其它 morph + 影响率
                    int morphIndex = -1;
                    if (!r.ReadSignedIndex(g.morphIndexSize, morphIndex) || !r.ReadF32(e.influence)) {
                        error = r.Error();
                        return false;
                    }
                    e.index = (morphIndex >= 0 && morphIndex < morphCount) ? morphIndex : -1;
                    break;
                }
                case 1: {  // 顶点形变
                    std::uint32_t vIdx = 0;
                    if (!r.ReadUnsignedIndex(g.vertexIndexSize, vIdx) ||
                        !r.ReadVec3(e.offset.data())) {
                        error = r.Error();
                        return false;
                    }
                    e.index = (vIdx < static_cast<std::uint32_t>(vertexCount))
                                  ? static_cast<int>(vIdx)
                                  : -1;
                    e.offset[3] = 0.0f;
                    break;
                }
                case 2: {  // 骨骼形变
                    int boneIndex = -1;
                    if (!r.ReadSignedIndex(g.boneIndexSize, boneIndex) ||
                        !r.ReadVec3(e.offset.data()) || !r.ReadVec4(e.rotation.data())) {
                        error = r.Error();
                        return false;
                    }
                    e.index = (boneIndex >= 0 && boneIndex < boneCount) ? boneIndex : -1;
                    break;
                }
                case 3:    // UV 形变
                case 4:    // 附加 UV1
                case 5:    // 附加 UV2
                case 6:    // 附加 UV3
                case 7: {  // 附加 UV4
                    std::uint32_t vIdx = 0;
                    if (!r.ReadUnsignedIndex(g.vertexIndexSize, vIdx) || !r.ReadVec4(e.offset.data())) {
                        error = r.Error();
                        return false;
                    }
                    e.index = (vIdx < static_cast<std::uint32_t>(vertexCount))
                                  ? static_cast<int>(vIdx)
                                  : -1;
                    break;
                }
                case 8: {  // 材质形变
                    int materialIndex = -1;
                    std::uint8_t operation = 0;
                    if (!r.ReadSignedIndex(g.materialIndexSize, materialIndex) ||
                        !r.ReadU8(operation) || !r.ReadVec4(e.offset.data())) {
                        // e.offset 复用为 diffuse；其余颜色分量本次不保留。
                        error = r.Error();
                        return false;
                    }
                    std::array<float, 3> specular{};
                    float specularStrength = 0.0f;
                    std::array<float, 3> ambient{};
                    std::array<float, 4> edgeColor{};
                    float edgeSize = 0.0f;
                    std::array<float, 4> textureTint{};
                    std::array<float, 4> sphereTint{};
                    std::array<float, 4> toonTint{};
                    if (!r.ReadVec3(specular.data()) || !r.ReadF32(specularStrength) ||
                        !r.ReadVec3(ambient.data()) || !r.ReadVec4(edgeColor.data()) ||
                        !r.ReadF32(edgeSize) || !r.ReadVec4(textureTint.data()) ||
                        !r.ReadVec4(sphereTint.data()) || !r.ReadVec4(toonTint.data())) {
                        error = r.Error();
                        return false;
                    }
                    // -1 表示「影响全部材质」，合法。
                    e.index = (materialIndex == -1 ||
                                (materialIndex >= 0 && materialIndex < materialCount))
                                   ? materialIndex
                                   : -1;
                    break;
                }
                case 9: {  // 翻转形变（PMX 2.1）
                    int morphIndex = -1;
                    if (!r.ReadSignedIndex(g.morphIndexSize, morphIndex) || !r.ReadF32(e.influence)) {
                        error = r.Error();
                        return false;
                    }
                    e.index = (morphIndex >= 0 && morphIndex < morphCount) ? morphIndex : -1;
                    break;
                }
                case 10: {  // 冲量形变（PMX 2.1）
                    int rbIndex = -1;
                    std::uint8_t localFlag = 0;
                    std::array<float, 3> movement{};
                    std::array<float, 3> torque{};
                    if (!r.ReadSignedIndex(g.rigidBodyIndexSize, rbIndex) ||
                        !r.ReadU8(localFlag) || !r.ReadVec3(movement.data()) ||
                        !r.ReadVec3(torque.data())) {
                        error = r.Error();
                        return false;
                    }
                    e.index = (rbIndex >= 0 && rbIndex < rigidBodyCount) ? rbIndex : -1;
                    break;
                }
                default: {
                    std::ostringstream oss;
                    oss << "morph 类型非法（" << static_cast<int>(morphType) << "）";
                    error = oss.str();
                    return false;
                }
            }
            m.elements.push_back(e);
        }
        doc.morphs.push_back(std::move(m));
    }
    return true;
}

bool ParseDisplayFrames(ByteReader& r, const PmxGlobals& g, PmxDocument& doc, std::string& error) {
    std::int32_t count = 0;
    if (!r.ReadI32(count)) {
        error = r.Error();
        return false;
    }
    if (!CheckCount(r, count, 12, kMaxDisplayFrames, "显示帧")) {
        error = r.Error();
        return false;
    }
    doc.displayFrames.reserve(static_cast<std::size_t>(count));

    const std::int32_t boneCount = static_cast<std::int32_t>(doc.bones.size());
    const std::int32_t morphCount = static_cast<std::int32_t>(doc.morphs.size());

    for (std::int32_t i = 0; i < count; ++i) {
        PmxDisplayFrame f;
        if (!ReadText(r, doc.utf16, f.name) || !ReadText(r, doc.utf16, f.nameEn) ||
            !r.ReadU8(f.specialFlag)) {
            error = r.Error();
            return false;
        }
        std::int32_t itemCount = 0;
        if (!r.ReadI32(itemCount)) {
            error = r.Error();
            return false;
        }
        if (itemCount < 0 || static_cast<std::uint32_t>(itemCount) > kMaxDisplayFrames) {
            error = "显示帧条目数量非法";
            return false;
        }
        f.items.reserve(static_cast<std::size_t>(itemCount));
        for (std::int32_t k = 0; k < itemCount; ++k) {
            PmxDisplayFrame::Item item;
            if (!r.ReadU8(item.target)) {
                error = r.Error();
                return false;
            }
            int idx = -1;
            if (item.target == 0) {
                if (!r.ReadSignedIndex(g.boneIndexSize, idx)) {
                    error = r.Error();
                    return false;
                }
                item.index = (idx >= 0 && idx < boneCount) ? idx : -1;
            } else {
                if (!r.ReadSignedIndex(g.morphIndexSize, idx)) {
                    error = r.Error();
                    return false;
                }
                item.index = (idx >= 0 && idx < morphCount) ? idx : -1;
            }
            f.items.push_back(item);
        }
        doc.displayFrames.push_back(std::move(f));
    }
    return true;
}

bool ParseRigidBodies(ByteReader& r, const PmxGlobals& g, PmxDocument& doc, std::string& error) {
    std::int32_t count = 0;
    if (!r.ReadI32(count)) {
        error = r.Error();
        return false;
    }
    // 刚体最小字节数：两个文本长度(8) + 骨骼索引(1) + 组(1) + 掩码(2) + 形状(1)
    //   + 尺寸(12) + 位置(12) + 旋转(12) + 质量/衰减/阻尼/弹性/摩擦(20) + 模式(1) = 70。
    if (!CheckCount(r, count, 70, kMaxRigidBodies, "刚体")) {
        error = r.Error();
        return false;
    }
    doc.rigidBodies.reserve(static_cast<std::size_t>(count));

    const std::int32_t boneCount = static_cast<std::int32_t>(doc.bones.size());
    for (std::int32_t i = 0; i < count; ++i) {
        PmxRigidBody rb;
        if (!ReadText(r, doc.utf16, rb.name) || !ReadText(r, doc.utf16, rb.nameEn)) {
            error = r.Error();
            return false;
        }
        int boneIndex = -1;
        if (!r.ReadSignedIndex(g.boneIndexSize, boneIndex) || !r.ReadU8(rb.groupId) ||
            !r.ReadU16(rb.nonCollisionMask) || !r.ReadU8(rb.shapeType) ||
            !r.ReadVec3(rb.size.data()) || !r.ReadVec3(rb.position.data()) ||
            !r.ReadVec3(rb.rotation.data()) || !r.ReadF32(rb.mass) ||
            !r.ReadF32(rb.moveAttenuation) || !r.ReadF32(rb.rotationDamping) ||
            !r.ReadF32(rb.restitution) || !r.ReadF32(rb.friction) || !r.ReadU8(rb.physicsMode)) {
            error = r.Error();
            return false;
        }
        rb.boneIndex = (boneIndex >= 0 && boneIndex < boneCount) ? boneIndex : -1;
        doc.rigidBodies.push_back(std::move(rb));
    }
    return true;
}

bool ParseJoints(ByteReader& r, const PmxGlobals& g, PmxDocument& doc, std::string& error) {
    std::int32_t count = 0;
    if (!r.ReadI32(count)) {
        error = r.Error();
        return false;
    }
    // 关节最小字节数：两个文本长度(8) + 类型(1) + 两个刚体索引(2)
    //   + 位置/旋转/4 个限制/2 个弹簧(8 * 12 = 96) = 107。
    if (!CheckCount(r, count, 107, kMaxJoints, "关节")) {
        error = r.Error();
        return false;
    }
    doc.joints.reserve(static_cast<std::size_t>(count));

    const std::int32_t rigidBodyCount = static_cast<std::int32_t>(doc.rigidBodies.size());
    for (std::int32_t i = 0; i < count; ++i) {
        PmxJoint j;
        if (!ReadText(r, doc.utf16, j.name) || !ReadText(r, doc.utf16, j.nameEn) ||
            !r.ReadU8(j.type)) {
            error = r.Error();
            return false;
        }
        int a = -1, b = -1;
        if (!r.ReadSignedIndex(g.rigidBodyIndexSize, a) ||
            !r.ReadSignedIndex(g.rigidBodyIndexSize, b) ||
            !r.ReadVec3(j.position.data()) || !r.ReadVec3(j.rotation.data()) ||
            !r.ReadVec3(j.limitMin.data()) || !r.ReadVec3(j.limitMax.data()) ||
            !r.ReadVec3(j.rotLimitMin.data()) || !r.ReadVec3(j.rotLimitMax.data()) ||
            !r.ReadVec3(j.springPosition.data()) || !r.ReadVec3(j.springRotation.data())) {
            error = r.Error();
            return false;
        }
        j.rigidBodyA = (a >= 0 && a < rigidBodyCount) ? a : -1;
        j.rigidBodyB = (b >= 0 && b < rigidBodyCount) ? b : -1;
        doc.joints.push_back(std::move(j));
    }
    return true;
}

/// PMX 2.1 的 SoftBody 段位于文件最末尾。
/// 本项目不实现软体物理，故只读取数量并忽略其内容——因为该段之后没有任何数据，
/// 忽略它不会造成后续读取错位。
bool ParseSoftBodies(ByteReader& r, PmxDocument& doc, std::string& error) {
    if (doc.version < 2.1f) return true;
    std::int32_t count = 0;
    if (!r.ReadI32(count)) {
        error = r.Error();
        return false;
    }
    if (count < 0) {
        error = "软体数量非法（为负数）";
        return false;
    }
    return true;
}

/// 解析完成后的跨段索引校验。
///
/// 顶点段在骨骼段之前，因此顶点的骨骼索引在读取时还无法判断是否越界，
/// 必须在这里统一处理。其它段的索引都在各自段内就地校验过了。
void ValidateCrossReferences(PmxDocument& doc) {
    const std::int32_t boneCount = static_cast<std::int32_t>(doc.bones.size());
    for (PmxVertex& v : doc.vertices) {
        for (int& bone : v.boneIndices) {
            if (bone < -1 || bone >= boneCount) bone = -1;
        }
    }
}

}  // namespace

bool ParsePmx(const std::string& path, PmxDocument& out, std::string& error) {
    out = PmxDocument{};
    error.clear();

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "无法打开 PMX 文件：" + path;
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < 0) {
        error = "无法读取 PMX 文件大小：" + path;
        return false;
    }
    // 200 MB 足以容纳任何合法的 PMX 模型。
    if (fileSize > 200LL * 1024 * 1024) {
        error = "PMX 文件过大（超过 200 MB），已拒绝加载";
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<unsigned char> buffer(static_cast<std::size_t>(fileSize));
    if (fileSize > 0 && !file.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
        error = "读取 PMX 文件失败：" + path;
        return false;
    }

    ByteReader r(buffer.data(), buffer.size());
    PmxGlobals globals;
    if (!ParseHeader(r, out, globals, error)) return false;
    if (!ParseVertices(r, globals, out, error)) return false;
    if (!ParseFaces(r, globals, out, error)) return false;
    if (!ParseTextures(r, out, error)) return false;
    if (!ParseMaterials(r, globals, out, error)) return false;
    if (!ParseBones(r, globals, out, error)) return false;
    if (!ParseMorphs(r, globals, out, error)) return false;
    if (!ParseDisplayFrames(r, globals, out, error)) return false;
    if (!ParseRigidBodies(r, globals, out, error)) return false;
    if (!ParseJoints(r, globals, out, error)) return false;
    if (!ParseSoftBodies(r, out, error)) return false;

    ValidateCrossReferences(out);
    return true;
}

}  // namespace ppet::pmx
