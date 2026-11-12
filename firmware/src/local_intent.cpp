#include "local_intent.h"

namespace localintent {
namespace {

// 每个标签一组说法。顺序即优先级：先判"最具体"的，再判泛化的。
struct Entry {
    const char* label;
    const char* phrase;
};

const Entry kEntries[] = {
    // 关（先判，避免「关灯」被「灯」类泛化词抢走）
    {"off", "关灯"}, {"off", "关掉灯"}, {"off", "把灯关"}, {"off", "关闭灯"}, {"off", "熄灯"},
    {"off", "关一下"}, {"off", "别亮"},

    // 最亮
    {"max", "最亮"}, {"max", "全亮"}, {"max", "最大亮度"}, {"max", "最亮模式"},

    // 调暗（含"太亮"）
    {"down", "暗一点"}, {"down", "暗一些"}, {"down", "暗点"}, {"down", "调暗"},
    {"down", "变暗"}, {"down", "太亮"},

    // 调亮（含"太暗"）
    {"up", "亮一点"}, {"up", "亮一些"}, {"up", "亮点"}, {"up", "调亮"},
    {"up", "变亮"}, {"up", "太暗"},

    // 场景
    {"night", "夜灯"}, {"night", "夜间模式"},
    {"study", "学习模式"}, {"study", "看书模式"}, {"study", "学习灯"},
    {"rest", "休息模式"}, {"rest", "睡眠模式"},

    // 开（放最后：上面都匹配不到才当开灯）
    {"on", "开灯"}, {"on", "打开灯"}, {"on", "把灯开"}, {"on", "打开台灯"},
    {"on", "亮起来"}, {"on", "开一下"},
};

}  // namespace

String match(const String& text) {
    if (text.isEmpty()) return String();
    for (const Entry& e : kEntries) {
        if (text.indexOf(e.phrase) >= 0) return String(e.label);
    }
    return String();
}

}  // namespace localintent
