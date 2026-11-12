#pragma once
// 固定命令词的本地匹配：命中就直接执行，不调云端 LLM。
//
// 为什么单独成文件：语料要能脱离固件单测，而且这份表以后会随用户说法变长，
// 放在 cloud_asr.cpp 里会被网络代码淹没。

#include <Arduino.h>

namespace localintent {

// 返回 on / off / up / down / max / night / study / rest；匹配不到返回空串。
// 只做包含匹配，顺序从"最长、最具体"到"最短"，避免「亮一点」被「亮」抢先。
String match(const String& text);

}  // namespace localintent
