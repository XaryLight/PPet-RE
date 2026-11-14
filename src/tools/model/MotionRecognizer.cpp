// PPet - C++ 移植版
// 智能动作识别实现（需求 2）。
#include <src/tools/model/MotionRecognizer.hpp>

#include <algorithm>
#include <sstream>

namespace ppet::live2d {

namespace {

/// 把单个已判定用途的动作并入分析结构。
void RecordKind(MotionSetAnalysis& out, MotionGroupKind kind,
                const std::string& name, const std::string& hitArea) {
    switch (kind) {
        case MotionGroupKind::Idle:
            out.hasIdle = true;
            ++out.idleCount;
            if (!name.empty()) out.idleNames.push_back(name);
            break;
        case MotionGroupKind::Expression:
            ++out.expressionCount;
            break;
        case MotionGroupKind::Tap:
        case MotionGroupKind::Pinch:
        case MotionGroupKind::Shake:
        case MotionGroupKind::Flick:
            ++out.interactionCount;
            if (!name.empty()) out.touchNames.push_back(name);
            if (!hitArea.empty()) out.touchByRegion[hitArea].push_back(name);
            break;
        case MotionGroupKind::Other:
        default:
            ++out.otherCount;
            break;
    }
}

}  // namespace

MotionSetAnalysis AnalyzeMotionFiles(const std::vector<std::string>& fileNames) {
    MotionSetAnalysis out;
    for (const std::string& file : fileNames) {
        const MotionGroupKind kind = ClassifyMotionFile(file);
        const std::string hitArea = HitAreaFromMotionFile(file);
        // 文件名群体没有「组名」概念，这里用去掉后缀的基名作为动作名，
        // 保证随机待机 / 触摸分发能按名字找到对应动作。
        std::string base = file;
        const std::size_t slash = base.find_last_of("/\\");
        if (slash != std::string::npos) base = base.substr(slash + 1);
        static const char* kSuffixes[] = {".motion3.json", ".m3.json", ".json"};
        for (const char* s : kSuffixes) {
            const std::size_t len = std::strlen(s);
            if (base.size() > len && base.compare(base.size() - len, len, s) == 0) {
                base = base.substr(0, base.size() - len);
                break;
            }
        }
        RecordKind(out, kind, base, hitArea);
    }
    return out;
}

MotionSetAnalysis AnalyzeMotionGroups(const std::vector<MotionGroup>& groups) {
    MotionSetAnalysis out;
    for (const MotionGroup& g : groups) {
        RecordKind(out, g.kind, g.name, g.hitArea);
    }
    return out;
}

std::string MotionSetAnalysis::Summary() const {
    std::ostringstream os;
    os << "动作识别：待机 " << idleCount << " / 触摸 " << interactionCount
       << " / 表情 " << expressionCount << " / 其它 " << otherCount;
    if (!touchByRegion.empty()) {
        os << "；触摸分区 ";
        bool first = true;
        for (const auto& kv : touchByRegion) {
            if (!first) os << "，";
            first = false;
            os << kv.first << "×" << kv.second.size();
        }
    }
    return os.str();
}

}  // namespace ppet::live2d
