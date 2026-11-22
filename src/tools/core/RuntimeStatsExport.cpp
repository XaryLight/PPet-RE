// PPet - C++ 移植版
// 运行时指标导出的实现（详见 RuntimeStatsExport.hpp）。
#include <src/tools/core/RuntimeStatsExport.hpp>

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace ppet {
namespace {

/// 浮点转十进制字符串：6 位有效数字，避免 inf/nan 污染导出文件。
std::string Fmt(float v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    std::ostringstream os;
    os << std::setprecision(6) << v;
    return os.str();
}

std::string Fmt(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    std::ostringstream os;
    os << std::setprecision(6) << v;
    return os.str();
}

std::string B(bool v) { return v ? "true" : "false"; }

}  // namespace

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    // 其余控制字符用 \u00XX 转义；UTF-8 多字节（>=0x80）原样透传。
                    static const char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string JsonUnescape(const std::string& escaped) {
    std::string out;
    out.reserve(escaped.size());
    for (std::size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] != '\\' || i + 1 >= escaped.size()) {
            out += escaped[i];
            continue;
        }
        const char n = escaped[++i];
        switch (n) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
                // 仅支持 \u00XX（本模块只产出这一种）：i 指向 'u'，
                // 其后依次是 '0','0',hi,lo 四个 hex 位，取 i+3/i+4。
                if (i + 4 < escaped.size()) {
                    auto hex = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    const int hi = hex(escaped[i + 3]);
                    const int lo = hex(escaped[i + 4]);
                    if (hi >= 0 && lo >= 0) {
                        out += static_cast<char>((hi << 4) | lo);
                        i += 4;
                    }
                }
                break;
            default: out += n; break;
        }
    }
    return out;
}

std::string CsvEscape(const std::string& value) {
    const bool needsQuote = value.find_first_of(",\"\n\r") != std::string::npos;
    if (!needsQuote) return value;
    std::string out;
    out.reserve(value.size() + 8);
    out += '"';
    for (char c : value) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += '"';
    return out;
}

Metrics BuildMetrics(const ModelDebugInfo& model, const RuntimeStats& runtime) {
    Metrics m;
    // 模型身份与规模。
    m.emplace_back("model_id", model.modelId.empty() ? model.path : model.modelId);
    m.emplace_back("model_name", model.displayName);
    m.emplace_back("model_type", ModelTypeName(model.type));
    m.emplace_back("model_path", model.path);
    m.emplace_back("source_archive", model.sourceArchive);
    m.emplace_back("format_version", model.formatVersion);
    m.emplace_back("text_encoding", model.textEncoding);
    m.emplace_back("loaded", B(model.loaded));
    m.emplace_back("rendering_ok", B(model.renderingOk));
    m.emplace_back("gpu_ready", B(model.gpuReady));
    m.emplace_back("gl_error", std::to_string(runtime.glError));
    m.emplace_back("render_status", model.renderStatus);
    m.emplace_back("vertices", std::to_string(model.vertexCount));
    m.emplace_back("indices", std::to_string(model.indexCount));
    m.emplace_back("triangles", std::to_string(model.triangleCount));
    m.emplace_back("bones", std::to_string(model.boneCount));
    m.emplace_back("materials", std::to_string(model.materialCount));
    m.emplace_back("sub_meshes", std::to_string(model.subMeshCount));
    m.emplace_back("morphs", std::to_string(model.morphCount));
    m.emplace_back("textures_referenced", std::to_string(model.textureReferenced));
    m.emplace_back("textures_loaded", std::to_string(model.textureLoaded));
    m.emplace_back("draw_calls", std::to_string(model.drawCalls));
    m.emplace_back("model_height", Fmt(model.modelHeight));
    // 运行时帧统计。
    m.emplace_back("fps", Fmt(runtime.fps));
    m.emplace_back("frame_ms", Fmt(runtime.frameMs));
    m.emplace_back("delta_time", Fmt(runtime.deltaTime));
    m.emplace_back("frame_count", std::to_string(runtime.frameCount));
    m.emplace_back("viewport_width", std::to_string(runtime.viewportWidth));
    m.emplace_back("viewport_height", std::to_string(runtime.viewportHeight));
    m.emplace_back("render_scale", Fmt(runtime.renderScale));
    m.emplace_back("desktop_width", std::to_string(runtime.desktopWidth));
    m.emplace_back("desktop_height", std::to_string(runtime.desktopHeight));
    m.emplace_back("global_cursor_x", Fmt(runtime.globalCursor.x));
    m.emplace_back("global_cursor_y", Fmt(runtime.globalCursor.y));
    m.emplace_back("local_cursor_x", Fmt(runtime.localCursor.x));
    m.emplace_back("local_cursor_y", Fmt(runtime.localCursor.y));
    m.emplace_back("has_global_cursor", B(runtime.hasGlobalCursor));
    m.emplace_back("tracker_running", B(runtime.trackerRunning));
    // 帧缓冲像素证据。
    m.emplace_back("has_frame_evidence", B(runtime.hasFrameEvidence));
    m.emplace_back("frame_coverage_percent", Fmt(runtime.frameCoveragePercent));
    m.emplace_back("frame_non_transparent", std::to_string(runtime.frameNonTransparent));
    m.emplace_back("frame_total_pixels", std::to_string(runtime.frameTotalPixels));
    m.emplace_back("frame_evidence_width", std::to_string(runtime.frameEvidenceWidth));
    m.emplace_back("frame_evidence_height", std::to_string(runtime.frameEvidenceHeight));
    // 缺失资源：合并成单字段，分隔符用 "; "。
    std::string missing;
    for (std::size_t i = 0; i < model.missingResources.size(); ++i) {
        if (i) missing += "; ";
        missing += model.missingResources[i];
    }
    m.emplace_back("missing_resources", missing);
    return m;
}

std::string MetricsToJson(const Metrics& metrics) {
    std::string out = "{\n";
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        out += "  \"" + JsonEscape(metrics[i].first) + "\": \"" +
               JsonEscape(metrics[i].second) + "\"";
        if (i + 1 < metrics.size()) out += ",";
        out += "\n";
    }
    out += "}\n";
    return out;
}

std::string MetricsToCsv(const Metrics& metrics) {
    std::string out;
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        if (i) out += ",";
        out += CsvEscape(metrics[i].first);
    }
    out += "\n";
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        if (i) out += ",";
        out += CsvEscape(metrics[i].second);
    }
    out += "\n";
    return out;
}

}  // namespace ppet
