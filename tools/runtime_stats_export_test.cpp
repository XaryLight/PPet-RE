// PPet - C++ 移植版
// 运行时指标导出单测（报告 4.2.3）：纯逻辑，零 Qt 依赖。
#include <src/tools/core/RuntimeStatsExport.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool cond, const char* what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  [FAIL] %s\n", what);
    }
}

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

using namespace ppet;

std::string FindMetric(const Metrics& m, const std::string& key) {
    for (const auto& kv : m) if (kv.first == key) return kv.second;
    return "<MISSING>";
}

void TestJsonEscape() {
    std::printf("-- JSON 转义与往返 --\n");
    Check(JsonEscape("a\"b\\c") == "a\\\"b\\\\c", "引号与反斜杠");
    Check(JsonEscape("line1\nline2\tend") == "line1\\nline2\\tend", "换行与制表符");
    Check(JsonEscape(std::string("ctl") + char(0x01)) == "ctl\\u0001", "控制字符 \\u00XX");

    const std::vector<std::string> samples = {
        "plain", "引号\"和\\斜杠", "多行\n第二行", "中文与 emoji 😀",
        std::string("混合") + char(0x02) + "末尾"};
    for (const auto& s : samples) {
        Check(JsonUnescape(JsonEscape(s)) == s, "转义往返一致");
    }
    // UTF-8 多字节必须原样透传，不被误转义。
    Check(JsonEscape("中文") == "中文", "UTF-8 原样透传");
}

void TestCsvEscape() {
    std::printf("-- CSV 转义 --\n");
    Check(CsvEscape("plain") == "plain", "普通值不加引号");
    Check(CsvEscape("a,b") == "\"a,b\"", "含逗号加引号");
    Check(CsvEscape("a\"b") == "\"a\"\"b\"", "引号双写");
    Check(CsvEscape("a\nb") == "\"a\nb\"", "含换行加引号");
    Check(CsvEscape("") == "", "空值不加引号");
}

void TestMetrics() {
    std::printf("-- 指标表采集 --\n");
    ModelDebugInfo model;
    model.modelId = "stable-id";
    model.displayName = "测试模型";
    model.type = ModelType::Vrm;
    model.path = "/models/a.vrm";
    model.renderingOk = true;
    model.gpuReady = true;
    model.vertexCount = 1234;
    model.missingResources = {"hair.png", "face.png"};

    RuntimeStats rt;
    rt.fps = 59.94f;
    rt.frameCount = 1000;
    rt.viewportWidth = 437;
    rt.viewportHeight = 600;
    rt.glError = 0;
    rt.globalCursor = {10.5f, 20.25f};
    rt.hasGlobalCursor = true;

    const Metrics m = BuildMetrics(model, rt);
    Check(FindMetric(m, "model_id") == "stable-id", "model_id 优先用稳定标识");
    Check(FindMetric(m, "model_name") == "测试模型", "中文模型名");
    Check(FindMetric(m, "model_type") == "VRM", "类型名");
    Check(FindMetric(m, "rendering_ok") == "true", "bool true");
    Check(FindMetric(m, "gpu_ready") == "true", "gpuReady 独立取值");
    Check(FindMetric(m, "tracker_running") == "false", "bool false");
    Check(FindMetric(m, "vertices") == "1234", "顶点数");
    Check(FindMetric(m, "frame_count") == "1000", "帧计数");
    Check(FindMetric(m, "viewport_width") == "437", "视口宽");
    Check(FindMetric(m, "global_cursor_x") == "10.5", "光标 x");
    Check(FindMetric(m, "missing_resources") == "hair.png; face.png", "缺失资源合并");

    // 空 modelId 回退到 path。
    ModelDebugInfo noId = model;
    noId.modelId.clear();
    const Metrics m2 = BuildMetrics(noId, rt);
    Check(FindMetric(m2, "model_id") == "/models/a.vrm", "空 id 回退 path");
}

void TestFormats() {
    std::printf("-- JSON / CSV 格式 --\n");
    ModelDebugInfo model;
    model.displayName = "A\"B";  // 含引号，验证转义进入输出。
    RuntimeStats rt;
    rt.fps = 60.0f;
    const Metrics m = BuildMetrics(model, rt);

    const std::string json = MetricsToJson(m);
    Check(json.size() > 2 && json.front() == '{' && json.back() == '\n', "JSON 以 { 开头、换行结尾");
    Check(Contains(json, "\"fps\": \"60\""), "JSON 键值格式");
    Check(Contains(json, "A\\\"B"), "JSON 内引号已转义");
    Check(Contains(json, "\"tracker_running\": \"false\""), "JSON bool 是字符串值");

    const std::string csv = MetricsToCsv(m);
    Check(csv.find('\n') != std::string::npos, "CSV 含换行");
    // 表头与数据行数（含末尾换行，应有 3 个 \n）。
    int lines = 0;
    for (char c : csv) if (c == '\n') ++lines;
    Check(lines == 2, "CSV 两行（表头 + 数据）");
    // 表头首列应为 key。
    Check(csv.rfind("model_id,", 0) == 0, "CSV 表头首列");
}

}  // namespace

int main() {
    std::printf("== 运行时指标导出单测 ==\n");
    TestJsonEscape();
    TestCsvEscape();
    TestMetrics();
    TestFormats();
    std::printf("== %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
