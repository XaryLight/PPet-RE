// PPet - C++ 移植版
// 控制台格式化纯函数单测（报告 2.3.1：App God Object 拆分）。
// 这些函数原先是 AppConsole.cpp 匿名命名空间里的自由函数，
// 以及 debug 命令内联的约 105 行诊断渲染；抽出后无需 App/图形环境即可验证。
#include <src/tools/app/ConsoleFormat.hpp>

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

using namespace ppet;
using namespace ppet::console_fmt;

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

AppConfig MakeCfg() {
    AppConfig cfg;
    cfg.modelList = {"/models/cry/model3.pmx", "/models/nengdai_2.model3.json",
                     "/models/akari.vrm"};
    cfg.modelNames = {" cry ", "", "阿卡丽"};
    cfg.modelPath = "/models/nengdai_2.model3.json";
    return cfg;
}

void TestPathAndNames() {
    std::printf("-- 路径与显示名 --\n");
    Check(ConsoleBaseName("") == "(未加载)", "空路径回退文案");
    Check(ConsoleBaseName("/a/b/cry.pmx") == "cry", "普通文件名去后缀");
    Check(ConsoleBaseName("/a/b/nengdai_2.model3.json") == "nengdai_2",
          "复合后缀整体去掉");
    Check(ConsoleBaseName("/a/b/noext") == "noext", "无后缀原样返回");

    const AppConfig cfg = MakeCfg();
    Check(ModelNameAt(cfg, 0) == " cry ", "优先使用配置里的显示名");
    Check(ModelNameAt(cfg, 1) == "nengdai_2", "空显示名回退文件名");
    Check(ModelNameAt(cfg, 2) == "阿卡丽", "中文显示名");
    Check(FindModelIndex(cfg, "/models/akari.vrm") == 2, "按路径找到下标");
    Check(FindModelIndex(cfg, "/models/missing.pmx") == -1, "找不到返回 -1");
    Check(CurrentModelName(cfg) == "nengdai_2", "当前模型取列表中的显示名");

    AppConfig orphan = MakeCfg();
    orphan.modelPath = "/outside/standalone.glb";
    Check(CurrentModelName(orphan) == "standalone", "当前模型不在列表时回退文件名");
}

void TestFailureDetection() {
    std::printf("-- 失败结果判定 --\n");
    Check(IsConsoleFailure("fail: 下标越界"), "fail 前缀");
    Check(IsConsoleFailure("用法: add <path>"), "用法前缀");
    Check(IsConsoleFailure("未知指令: blah"), "未知指令前缀");
    Check(IsConsoleFailure("To turn 0\nfail: 加载失败"), "多行结果含 fail");
    Check(IsConsoleFailure("result\n用法: select <idx>"), "多行结果含用法");
    Check(!IsConsoleFailure("Success"), "正常结果不算失败");
    Check(!IsConsoleFailure("Model: x\nPath : y"), "普通多行结果不算失败");
}

void TestTypeNameAndParseIndex() {
    std::printf("-- 类型名与下标解析 --\n");
    Check(std::string(TypeName(ModelType::Pmx)) == "PMX", "PMX 类型名");
    Check(std::string(TypeName(ModelType::Vrm)) == "VRM", "VRM 类型名");
    Check(std::string(TypeName(ModelType::Live2D)) == "Live2D", "Live2D 类型名");
    Check(std::string(TypeName(ModelType::Unknown)) == "Unknown", "Unknown 类型名");

    int out = -999;
    Check(ParseIndex("0", out) && out == 0, "解析 0");
    Check(ParseIndex("42", out) && out == 42, "解析正整数");
    Check(ParseIndex("-1", out) && out == -1, "解析负数");
    Check(ParseIndex(" 12", out) && out == 12, "前导空白（stoi 语义）仍接受");
    Check(!ParseIndex("12 ", out), "尾部空白拒绝");
    Check(!ParseIndex("12x", out), "夹杂非数字拒绝");
    Check(!ParseIndex("", out), "空串拒绝");
    Check(!ParseIndex("abc", out), "纯字母拒绝");
}

void TestJoinFailureSection() {
    std::printf("-- Join / Failure / Section --\n");
    Check(Join({}, ", ") == "", "空列表连接");
    Check(Join({"only"}, ", ") == "only", "单元素连接");
    Check(Join({"a", "b", "c"}, ", ") == "a, b, c", "多元素连接");
    Check(Join({"a", "b"}, "-") == "a-b", "自定义分隔符");

    ModelLoadError err;
    err.summary = "文件不存在";
    Check(Failure(err) == "fail: 文件不存在", "优先用一句话原因");
    ModelLoadError empty;
    Check(Failure(empty) == "fail: ", "空错误回退 ToString");

    Check(Section("status") == "[status]", "分节标题格式");
}

void TestFormatDebugInfoAll() {
    std::printf("-- debug 诊断渲染：all --\n");
    ModelDebugInfo info;
    info.displayName = "测试模型";
    info.type = ModelType::Vrm;
    info.loaded = true;
    info.gpuReady = true;
    info.renderingOk = true;
    info.vertexCount = 1234;
    info.boneCount = 56;
    info.modelHeight = 1.75f;
    info.motionStatus = "待机";
    info.expressionStatus = "开心";
    info.formatVersion = "VRM 1.0";
    info.textEncoding = "UTF-8";

    ModelDebugInfo::TextureInfo tex;
    tex.name = "body.png";
    tex.width = 512;
    tex.height = 512;
    tex.loaded = true;
    info.textures.push_back(tex);
    info.missingResources.push_back("hair.png");

    info.bones.push_back({0, "root", -1, 0, "可位移"});
    info.parts.push_back({"身体", 600, 0, "材质0"});
    info.properties.push_back("坐标系=Y-up");
    info.dialogueStatus = "空闲";

    DebugViewContext ctx;
    ctx.fallbackPath = "/fallback/model.vrm";
    ctx.resources = {{"shader", "toon"}, {"physics", "enabled"}};
    ctx.motionNames = {"idle", "tap"};
    ctx.expressionIds = {"joy", "angry"};

    const std::string all = FormatDebugInfo(info, "all", ctx);
    Check(Contains(all, "Model: 测试模型 [VRM]"), "概览含类型名");
    Check(Contains(all, "Path : /fallback/model.vrm"), "空 info.path 回退到 fallbackPath");
    Check(Contains(all, "[status 渲染状态]"), "all 含 status 分节");
    Check(Contains(all, "[params 规模参数]"), "all 含 params 分节");
    Check(Contains(all, "vertices=1234"), "规模参数数值");
    Check(Contains(all, "[res 加载的资源]"), "all 含 res 分节");
    Check(Contains(all, "shader : toon"), "外部资源清单进入输出");
    Check(Contains(all, "texture : body.png  512x512  [loaded]"), "贴图明细");
    Check(Contains(all, "MISSING : hair.png"), "缺失资源");
    Check(Contains(all, "[bones 骨架 (1)]"), "骨架分节计数");
    Check(Contains(all, "#0 root  parent=-1  depth=0"), "骨架条目");
    Check(Contains(all, "[parts 模型部分 (1)]"), "parts 分节计数");
    Check(Contains(all, "[motion 动作]"), "motion 分节");
    Check(Contains(all, "actions: idle, tap"), "动作名来自 ctx");
    Check(Contains(all, "[expression 表情]"), "expression 分节");
    Check(Contains(all, "presets: joy, angry"), "表情名来自 ctx");
    Check(Contains(all, "dialogue: 空闲"), "对话状态");
    Check(Contains(all, "[property 性质]"), "property 分节");
    Check(Contains(all, "坐标系=Y-up"), "额外性质");
    Check(Contains(all, "(无)"), "无加载错误显示占位");
}

void TestFormatDebugInfoSections() {
    std::printf("-- debug 诊断渲染：分节筛选 --\n");
    ModelDebugInfo info;
    info.displayName = "M";
    info.type = ModelType::Pmx;
    info.modelId = "stable-id";
    info.path = "/real/path.pmx";
    DebugViewContext ctx;
    ctx.fallbackPath = "/should/not/appear";

    const std::string status = FormatDebugInfo(info, "STATUS", ctx);
    Check(Contains(status, "[status 渲染状态]"), "节名大小写不敏感");
    Check(!Contains(status, "[params 规模参数]"), "单节输出不含其它节");
    Check(Contains(status, "modelId     : stable-id"), "非空 modelId 优先");

    ModelDebugInfo noIds = info;
    noIds.modelId.clear();
    noIds.path.clear();
    const std::string fallback = FormatDebugInfo(noIds, "status", ctx);
    Check(Contains(fallback, "modelId     : /should/not/appear"), "空 modelId 回退");

    const std::string res = FormatDebugInfo(info, "res", ctx);
    Check(Contains(res, "(后端未提供资源清单)"), "空资源清单占位");
    Check(!Contains(res, "[status 渲染状态]"), "res 单节不含 status");

    // 有加载错误时输出分类/摘要/明细。
    DebugViewContext errCtx = ctx;
    errCtx.hasLoadError = true;
    errCtx.errorCategory = "MissingTexture";
    errCtx.errorSummary = "贴图缺失";
    errCtx.errorDetails = {"hair.png", "face.png"};
    const std::string withErr = FormatDebugInfo(info, "error", errCtx);
    Check(Contains(withErr, "category: MissingTexture"), "错误分类");
    Check(Contains(withErr, "summary : 贴图缺失"), "错误摘要");
    Check(Contains(withErr, "    - hair.png"), "错误明细逐条");
    Check(Contains(withErr, "    - face.png"), "错误明细第二条");
    Check(!Contains(withErr, "(无)"), "有错时不显示占位");

    // 骨架超过 40 根时给出省略提示。
    ModelDebugInfo many = info;
    for (int i = 0; i < 45; ++i) {
        many.bones.push_back({i, "b" + std::to_string(i), -1, 0, ""});
    }
    const std::string bones = FormatDebugInfo(many, "bones", ctx);
    Check(Contains(bones, "[bones 骨架 (45)]"), "骨架总数正确");
    Check(Contains(bones, "其余 5 根省略"), "超出 40 根时省略提示");
}

}  // namespace

int main() {
    std::printf("== 控制台格式化纯函数单测 ==\n");
    TestPathAndNames();
    TestFailureDetection();
    TestTypeNameAndParseIndex();
    TestJoinFailureSection();
    TestFormatDebugInfoAll();
    TestFormatDebugInfoSections();
    std::printf("== %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
