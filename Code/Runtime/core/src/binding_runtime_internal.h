#pragma once

// T-412: declarations shared by the binding_runtime*.cpp files (split from one file along its section
// comments; Docs/tasks/T-412.md). Internal to this module: only those files include it.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

#include "binding_runtime.h"
#include "dispatch_guard.h"
#include "mini_css.h"
#include "mini_json.h"
#include "repeat_controller.h"
#include "rig_eval.h"
#include "style_decode.h"
#include "trigger_runtime.h"
#include "xt/animator.h"
#include "xt/num_guard.h"
#include "xt/property.h"
#include "xt/version.h"

namespace xt {

namespace binding_runtime_detail {

using minijson::Json;
using minijson::JsonReader;

/// T-394 (08.15): sets BindingRuntime::m_documentWrite for one write, restoring it after.
struct DocumentWriteScope {
    bool& slot;
    bool saved;
    DocumentWriteScope(bool& s, bool on) : slot(s), saved(s) { slot = on; }
    ~DocumentWriteScope() { slot = saved; }
    DocumentWriteScope(const DocumentWriteScope&) = delete;
    DocumentWriteScope& operator=(const DocumentWriteScope&) = delete;
};

bool isStylePropId(uint16_t id);

using BVal = BindingRuntime::BVal;
using Kind = BindingRuntime::BVal::Kind;

void markStyleDirtySubtree(NodeArena& arena, NodeId node);
BVal num(double v, bool isBool = false);
BVal str(std::string s);
BVal ofValue(Kind kind, const Value& v);
std::string numText(double v);
std::string toStr(const BVal& b);
double toNum(const BVal& b);
bool truthy(const BVal& b);
bool same(const BVal& a, const BVal& b);
BVal lerpB(const BVal& a, const BVal& b, float t);
std::string timeText(double seconds, const std::string& pattern);
std::string thousandsText(double v);
std::string fixedText(double v, int decimals);
bool parseColorText(std::string_view text, Color& out);
bool isEnumIndex(double v);
bool parseDouble(std::string_view text, double& out);

}  // namespace binding_runtime_detail

}  // namespace xt
