#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "expr_op_info.h"    // Code/Runtime/core/src (on the test include path).
#include "test_temp.h"       // tempEnv()
#include "timeline_expr.h"   // Code/Runtime/core/src.
#include "timeline_group.h"  // Code/Runtime/core/src: ExprTestAccess below.
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/expr_noise.h"
#include "xt/ir.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt/timeline.h"

/// T-776 (Docs/design/58-value-functions-and-drivers.md §58.5, Docs/tasks/T-776.md): timeline
/// expression programs at run time -- the IR's TEXP chunk (hand-built here: the compiler does not
/// emit it yet), its validation, the evaluator's ops, the group time of `@timeline` members, the
/// inputs (ports, layout, another element's property, the per-play seed, the port history) and the
/// guarantee that a timeline that is paused, held or released with unchanged inputs runs no program.

namespace xt {

/// The private state the tests reach (Document befriends it).
struct ExprTestAccess {
    static TimelineExprSet& exprs(Document& d) { return d.m_timelineGroups->exprs; }
    static void setSeedBase(Document& d, uint32_t base) { d.m_timelineGroups->exprs.setSeedBase(base); }
    static void pause(Document& d, size_t group, bool paused) { d.setTimelineGroupPaused(group, paused); }
};

}  // namespace xt

using namespace xt;
using namespace xt::compile;

namespace {

uint32_t bitsOf(float f) { return std::bit_cast<uint32_t>(f); }

IrExprOp op(ExprOp o, float imm = 0.0f, uint32_t ref = 0, int argc = -1) {
    IrExprOp r{};
    r.op = static_cast<uint8_t>(o);
    uint8_t lo = 0;
    uint8_t hi = 0;
    exprOpOperands(r.op, lo, hi);
    r.argc = static_cast<uint8_t>(argc < 0 ? lo : argc);
    r.imm = imm;
    r.ref = ref;
    return r;
}
IrExprOp k(float v) { return op(ExprOp::PushConst, v); }

IrExprKey key(float time, float value, uint8_t interp = 0) {
    IrExprKey out{};
    out.time = time;
    out.value = value;
    out.interp = interp;
    return out;
}

/// The deepest stack `prog` reaches (what IrTimelineExpr::maxStack must say).
uint16_t deepest(const std::vector<IrExprOp>& prog) {
    int depth = 0;
    int most = 0;
    for (const IrExprOp& o : prog) {
        depth = depth - o.argc + 1;
        most = std::max(most, depth);
    }
    return static_cast<uint16_t>(most);
}

/// Runs `prog` alone (no Document): every input reads 0, ValueAt reads env.value.
float run(const std::vector<IrExprOp>& prog, ExprEnv env = {}, bool* fellBack = nullptr,
          const std::vector<IrExprKey>& curveKeys = {}, uint32_t seed = 7) {
    std::vector<IrExprCurve> curves;
    if (!curveKeys.empty()) {
        curves.push_back(IrExprCurve{0, static_cast<uint32_t>(curveKeys.size())});
    }
    ExprProgram p;
    p.ops = prog;
    p.curves = curves;
    p.keys = curveKeys;
    p.seed = seed;
    p.index = 3.0f;
    return evalTimelineExpr(p, env, fellBack);
}

/// A hand-built TEXP chunk.
struct Texp {
    std::vector<IrTimelineExpr> exprs;
    std::vector<IrExprOp> ops;
    std::vector<IrExprCurve> curves;
    std::vector<IrExprKey> keys;

    uint32_t add(uint32_t group, uint32_t member, uint16_t track, uint8_t component,
                 const std::vector<IrExprOp>& prog, const std::vector<std::vector<IrExprKey>>& curveKeys = {},
                 float historySeconds = 0.0f) {
        IrTimelineExpr e{};
        e.group = group;
        e.member = member;
        e.track = track;
        e.component = component;
        e.opOffset = static_cast<uint32_t>(ops.size());
        e.opCount = static_cast<uint16_t>(prog.size());
        e.maxStack = deepest(prog);
        e.curveOffset = static_cast<uint32_t>(curves.size());
        e.curveCount = static_cast<uint16_t>(curveKeys.size());
        for (const IrExprOp& o : prog) {
            e.inputMask |= exprOpInput(o.op);
        }
        e.portHistorySeconds = historySeconds;
        e.seed = 1234;
        e.index = 1.0f;
        for (const std::vector<IrExprKey>& c : curveKeys) {
            curves.push_back(
                IrExprCurve{static_cast<uint32_t>(keys.size()), static_cast<uint32_t>(c.size())});
            keys.insert(keys.end(), c.begin(), c.end());
        }
        ops.insert(ops.end(), prog.begin(), prog.end());
        exprs.push_back(e);
        return static_cast<uint32_t>(exprs.size() - 1);
    }

    template <class T>
    static void put(std::vector<uint8_t>& out, const std::vector<T>& v) {
        const uint32_t n = static_cast<uint32_t>(v.size());
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&n);
        out.insert(out.end(), p, p + 4);
        const uint8_t* d = reinterpret_cast<const uint8_t*>(v.data());
        out.insert(out.end(), d, d + v.size() * sizeof(T));
    }
    std::vector<uint8_t> payload() const {
        std::vector<uint8_t> out;
        put(out, exprs);
        put(out, ops);
        put(out, curves);
        put(out, keys);
        return out;
    }
};

/// Appends a chunk to an `.xtir` blob (CRC 0: the tests open without CRC verification).
void appendChunk(std::vector<uint8_t>& ir, uint32_t type, const std::vector<uint8_t>& payload) {
    IrHeader h{};
    std::memcpy(&h, ir.data(), sizeof(h));
    IrChunkHeader ch{};
    std::memcpy(ch.type, &type, 4);
    ch.payloadLength = static_cast<uint32_t>(payload.size());
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&ch);
    ir.insert(ir.end(), p, p + sizeof(ch));
    ir.insert(ir.end(), payload.begin(), payload.end());
    h.chunkCount += 1;
    h.totalLength = static_cast<uint32_t>(ir.size());
    std::memcpy(ir.data(), &h, sizeof(h));
}

constexpr const char* kStyle = R"(
  #Box { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
  #Long { position: absolute; left: 50px; top: 0px; width: 10px; height: 10px; }
  #Other { position: absolute; left: 0px; top: 50px; width: 30px; height: 10px; opacity: 0.25; }
  @timeline main {
    #Box { 0s { opacity: 0 } 1s { opacity: 0.5 } }
    #Long { 0s { opacity: 0 } 4s { opacity: 1 } }
  }
)";
constexpr const char* kBody =
    R"(<div id="Box" port:hp="number"></div><div id="Long"></div><div id="Other"></div>)";

std::vector<uint8_t> compileIr(std::string_view style = kStyle, std::string_view body = kBody) {
    CompileOptions options;
    options.emitMeta = false;
    const std::string text = "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\">" +
                             std::string(body) + "</body></xt>";
    return test::compileMainOk(text, options);
}

/// Indices into the compiled IR the hand-built TEXP refers to.
struct Names {
    uint32_t group = 0;
    uint32_t box = 0;  // Member index of #Box in `main`.
    uint32_t lng = 0;  // Member index of #Long.
    uint16_t boxOpacity = 0;
    uint16_t longOpacity = 0;
    uint32_t otherElement = 0;  // DOCT index of #Other.
    uint32_t hp = 0;            // PORT index of `hp`.
};

Names names(const std::vector<uint8_t>& ir) {
    Result<IrView> opened = IrView::open(IrBlob{ir.data(), ir.size()});
    REQUIRE(opened.ok());
    const IrView& v = opened.value();
    Names n;
    const auto elementId = [&](uint32_t element) { return v.string(v.doc().elements()[element].id); };
    bool found = false;
    for (uint32_t g = 0; g < v.timelineGroups().groups().size(); ++g) {
        if (v.string(v.timelineGroups().groups()[g].name) == "main") {
            n.group = g;
            found = true;
        }
    }
    REQUIRE(found);
    const IrTimelineGroup& g = v.timelineGroups().groups()[n.group];
    for (uint32_t m = 0; m < g.memberCount; ++m) {
        const IrTimelineGroupMember& mem = v.timelineGroups().members()[g.memberOffset + m];
        const std::span<const IrTrack> tracks = v.timeline(mem.timeline).tracks();
        uint16_t opacity = 0xFFFF;
        for (uint16_t t = 0; t < tracks.size(); ++t) {
            if (tracks[t].prop == prop::Opacity) {
                opacity = t;
            }
        }
        if (elementId(mem.element) == "Box") {
            n.box = m;
            n.boxOpacity = opacity;
        } else if (elementId(mem.element) == "Long") {
            n.lng = m;
            n.longOpacity = opacity;
        }
    }
    REQUIRE(n.boxOpacity != 0xFFFF);
    REQUIRE(n.longOpacity != 0xFFFF);
    for (uint32_t e = 0; e < v.doc().elements().size(); ++e) {
        if (elementId(e) == "Other") {
            n.otherElement = e;
        }
    }
    for (uint32_t p = 0; p < v.ports().ports().size(); ++p) {
        if (v.string(v.ports().ports()[p].name) == "hp") {
            n.hp = p;
        }
    }
    return n;
}

struct Loaded {
    std::vector<uint8_t> ir;
    std::unique_ptr<Document> doc;
    PerfTracker perf;
    Names n;
    float clock = 0.0f;

    /// One frame at `t`; returns its FrameStats::exprEvaluated.
    uint32_t step(float t) {
        clock = t;
        perf.beginFrame(t);
        doc->update(t);
        return perf.current().exprEvaluated;
    }
    float opacity(std::string_view id) { return doc->byId(id).property(prop::Opacity).f; }
};

/// The default document with `texp` (built by `build` from the names) appended and loaded.
template <class Build>
std::unique_ptr<Loaded> load(Build build, std::string_view style = kStyle, std::string_view body = kBody) {
    auto l = std::make_unique<Loaded>();
    l->ir = compileIr(style, body);
    l->n = names(l->ir);
    Texp texp;
    build(texp, l->n);
    appendChunk(l->ir, kChunkTEXP, texp.payload());
    REQUIRE(Document::load(IrBlob{l->ir.data(), l->ir.size()}, false, nullptr, nullptr, l->doc) ==
            Status::kOk);
    l->perf.setEnabled(true);
    l->doc->setPerfTracker(&l->perf);
    l->doc->setViewport(Rect{0, 0, 200, 100});
    l->step(0.0f);
    return l;
}

PlayOptions hold() {
    PlayOptions o;
    o.hold = true;
    return o;
}

}  // namespace

// -- the evaluator alone ------------------------------------------------------------------------------

TEST_CASE("T-776: every op computes what the compiler's evaluator computes") {
    ExprEnv env;
    env.value = 10.0f;
    env.time = 1.5f;
    env.duration = 4.0f;
    CHECK(run({k(2.5f)}) == 2.5f);
    CHECK(run({op(ExprOp::PushValue)}, env) == 10.0f);
    CHECK(run({op(ExprOp::PushTime)}, env) == 1.5f);
    CHECK(run({op(ExprOp::PushDuration)}, env) == 4.0f);
    CHECK(run({op(ExprOp::PushIndex)}) == 3.0f);
    CHECK(run({op(ExprOp::PushCurve)}, env, nullptr, {key(1, 2), key(2, 4)}) == 3.0f);
    CHECK(run({op(ExprOp::PushCurve)}, env, nullptr, {key(1, 2, 1), key(2, 4)}) == 2.0f);  // Hold.
    CHECK(run({k(7), k(2), op(ExprOp::Add)}) == 9.0f);
    CHECK(run({k(7), k(2), op(ExprOp::Sub)}) == 5.0f);
    CHECK(run({k(7), k(2), op(ExprOp::Mul)}) == 14.0f);
    CHECK(run({k(7), k(2), op(ExprOp::Div)}) == 3.5f);
    CHECK(run({k(7), k(2), op(ExprOp::Mod)}) == 1.0f);
    CHECK(run({k(7), op(ExprOp::Neg)}) == -7.0f);
    CHECK(run({k(7), k(2), op(ExprOp::Min)}) == 2.0f);
    CHECK(run({k(7), k(2), op(ExprOp::Max)}) == 7.0f);
    CHECK(run({k(-7), op(ExprOp::Abs)}) == 7.0f);
    CHECK(run({k(2.5f), op(ExprOp::Round)}) == 3.0f);
    CHECK(run({k(-2.5f), op(ExprOp::Round)}) == -2.0f);  // JS Math.round: halves go up.
    CHECK(run({k(2.7f), op(ExprOp::Floor)}) == 2.0f);
    CHECK(run({k(2.2f), op(ExprOp::Ceil)}) == 3.0f);
    CHECK(run({k(9), op(ExprOp::Sqrt)}) == 3.0f);
    CHECK(run({k(2), k(10), op(ExprOp::Pow)}) == 1024.0f);
    CHECK(run({k(0), op(ExprOp::Exp)}) == 1.0f);
    CHECK(run({k(1), op(ExprOp::Log)}) == 0.0f);
    CHECK(run({k(0.5f), op(ExprOp::Sin)}) == std::sin(0.5f));
    CHECK(run({k(0.5f), op(ExprOp::Cos)}) == std::cos(0.5f));
    CHECK(run({k(0.5f), op(ExprOp::Tan)}) == std::tan(0.5f));
    CHECK(run({k(0.5f), op(ExprOp::Asin)}) == std::asin(0.5f));
    CHECK(run({k(0.5f), op(ExprOp::Acos)}) == std::acos(0.5f));
    CHECK(run({k(0.5f), op(ExprOp::Atan)}) == std::atan(0.5f));
    CHECK(run({k(1), k(-1), op(ExprOp::Atan2)}) == std::atan2(1.0f, -1.0f));
    CHECK(run({k(5), k(3), k(1), op(ExprOp::Clamp)}) == 3.0f);  // Bounds in either order.
    CHECK(run({k(-5), k(1), k(3), op(ExprOp::Clamp)}) == 1.0f);
    CHECK(run({k(10), k(20), k(0.25f), op(ExprOp::Lerp)}) == 12.5f);
    CHECK(run({k(1), k(2), op(ExprOp::Lt)}) == 1.0f);
    CHECK(run({k(2), k(2), op(ExprOp::Le)}) == 1.0f);
    CHECK(run({k(1), k(2), op(ExprOp::Gt)}) == 0.0f);
    CHECK(run({k(2), k(2), op(ExprOp::Ge)}) == 1.0f);
    CHECK(run({k(2), k(2), op(ExprOp::Eq)}) == 1.0f);
    CHECK(run({k(2), k(2), op(ExprOp::Ne)}) == 0.0f);
    CHECK(run({k(1), k(0), op(ExprOp::And)}) == 0.0f);
    CHECK(run({k(1), k(0), op(ExprOp::Or)}) == 1.0f);
    CHECK(run({k(0), op(ExprOp::Not)}) == 1.0f);
    CHECK(run({k(1), k(5), k(6), op(ExprOp::Select)}) == 5.0f);
    CHECK(run({k(0), k(5), k(6), op(ExprOp::Select)}) == 6.0f);
    // Without a Document every input reads 0 and ValueAt the row's value.
    CHECK(run({op(ExprOp::PushPort)}) == 0.0f);
    CHECK(run({k(1), op(ExprOp::ValueAt)}, env) == 10.0f);
}

TEST_CASE("T-776: linear and ease clamp at their ends, swap reversed bounds, follow AE's curves") {
    auto map = [](ExprOp o, float t, float t0, float t1) {
        return run({k(t), k(t0), k(t1), k(100), k(200), op(o)});
    };
    for (ExprOp o : {ExprOp::Linear, ExprOp::Ease, ExprOp::EaseIn, ExprOp::EaseOut}) {
        CHECK(map(o, -1, 0, 2) == 100.0f);
        CHECK(map(o, 0, 0, 2) == 100.0f);
        CHECK(map(o, 2, 0, 2) == 200.0f);
        CHECK(map(o, 5, 0, 2) == 200.0f);
        CHECK(map(o, 5, 2, 0) == 200.0f);  // tMin / tMax swapped; v1 / v2 are not.
    }
    CHECK(map(ExprOp::Linear, 0.5f, 0, 2) == 125.0f);
    const auto eased = [](const float* c, float p) {
        return 100.0f + 100.0f * evalEasing(Easing::bezier(c[0], c[1], c[2], c[3]), p);
    };
    CHECK(map(ExprOp::Ease, 0.5f, 0, 2) == eased(kExprEase, 0.25f));
    CHECK(map(ExprOp::EaseIn, 0.5f, 0, 2) == eased(kExprEaseIn, 0.25f));
    CHECK(map(ExprOp::EaseOut, 0.5f, 0, 2) == eased(kExprEaseOut, 0.25f));
}

TEST_CASE("T-776: noise, wiggle and random are xt/expr_noise.h bit for bit") {
    // The same functions with the same seed rules as the compiler (Tests/unit/test_expr_noise.cpp
    // pins the header's own outputs; here the evaluator must reproduce them exactly).
    ExprEnv env;
    env.value = 3.0f;
    env.duration = 15.0f;
    const uint32_t seed = 99;
    for (float x : {-2.25f, 0.0f, 0.5f, 1.75f, 1000.125f}) {
        CHECK(bitsOf(run({k(x), op(ExprOp::Noise, 0, 5)}, env, nullptr, {}, seed)) ==
              bitsOf(exprNoise(x, exprMixSeed(seed, 5))));
    }
    for (float t : {0.0f, 1.234f, 7.5f, 15.0f}) {
        env.time = t;
        // A constant frequency: periodic over the duration.
        CHECK(bitsOf(run({k(2), k(6), op(ExprOp::Wiggle, 0, 3)}, env, nullptr, {}, seed)) ==
              bitsOf(3.0f + 6.0f * exprWiggleNoise(t, 2.0f, 15.0f, exprMixSeed(seed, 3))));
        CHECK(bitsOf(run({k(2), k(6), k(3), k(0.5f), op(ExprOp::Wiggle, 0, 3, 4)}, env, nullptr, {}, seed)) ==
              bitsOf(3.0f + 6.0f * exprWiggleNoise(t, 2.0f, 15.0f, exprMixSeed(seed, 3), 3, 0.5f)));
        // A frequency from the time (not a constant): not periodic (the run-time rule).
        CHECK(bitsOf(run({op(ExprOp::PushTime), k(0), op(ExprOp::Mul), k(2), op(ExprOp::Add), k(6),
                          op(ExprOp::Wiggle, 0, 3)},
                         env, nullptr, {}, seed)) ==
              bitsOf(3.0f + 6.0f * exprWiggleNoise(t, 2.0f, 0.0f, exprMixSeed(seed, 3))));
        const uint32_t salted = exprMixSeed(seed, kExprRandomSalt);
        CHECK(bitsOf(run({k(10), k(20), op(ExprOp::Random, 0, 2)}, env, nullptr, {}, seed)) ==
              bitsOf(10.0f + 10.0f * exprRandom01(exprRandomSeed(salted, exprRandomFrame(t), 2, false))));
        CHECK(bitsOf(run({k(10), k(20), op(ExprOp::Random, 1, 2)}, env, nullptr, {}, seed)) ==
              bitsOf(10.0f + 10.0f * exprRandom01(exprRandomSeed(salted, 0, 2, true))));
    }
    // wiggle with a constant frequency gives the same value at 0 and at the duration.
    env.time = 0.0f;
    const float at0 = run({k(2), k(6), op(ExprOp::Wiggle, 0, 3)}, env, nullptr, {}, seed);
    env.time = 15.0f;
    CHECK(bitsOf(run({k(2), k(6), op(ExprOp::Wiggle, 0, 3)}, env, nullptr, {}, seed)) == bitsOf(at0));
    // random: a new number each frame unless timeless, the same within a frame.
    env.time = 1.0f;
    const float perFrame = run({k(0), k(1), op(ExprOp::Random, 0, 0)}, env);
    const float timeless = run({k(0), k(1), op(ExprOp::Random, 1, 0)}, env);
    env.time = 2.0f;
    CHECK(run({k(0), k(1), op(ExprOp::Random, 0, 0)}, env) != perFrame);
    CHECK(run({k(0), k(1), op(ExprOp::Random, 1, 0)}, env) == timeless);
    CHECK(run({k(0), k(1), k(1.0f), op(ExprOp::Random, 0, 0, 3)}, env) == perFrame);  // Explicit time.
}

TEST_CASE("T-777: CurveAt reads a hidden curve at any time; RandomSeeded is random() after seedRandom()") {
    ExprEnv env;
    env.time = 1.75f;
    const std::vector<IrExprKey> curve{key(1, 2), key(2, 4)};
    CHECK(run({k(1.5f), op(ExprOp::CurveAt)}, env, nullptr, curve) == 3.0f);
    CHECK(run({k(0.0f), op(ExprOp::CurveAt)}, env, nullptr, curve) == 2.0f);  // Before the first key.
    CHECK(run({k(9.0f), op(ExprOp::CurveAt)}, env, nullptr, curve) == 4.0f);
    CHECK(run({op(ExprOp::PushCurve)}, env, nullptr, curve) == 3.5f);  // PushCurve: at the group time.
    const uint32_t seed = 99;
    for (float t : {0.0f, 1.234f, 7.5f}) {
        env.time = t;
        const uint32_t seeded = exprMixSeed(seed, exprSeedFromNumber(42.0f));
        CHECK(bitsOf(run({k(42), k(10), k(20), op(ExprOp::RandomSeeded, 0, 2)}, env, nullptr, {}, seed)) ==
              bitsOf(10.0f + 10.0f * exprRandom01(exprRandomSeed(seeded, exprRandomFrame(t), 2, false))));
        CHECK(bitsOf(run({k(42), k(10), k(20), op(ExprOp::RandomSeeded, 1, 2)}, env, nullptr, {}, seed)) ==
              bitsOf(10.0f + 10.0f * exprRandom01(exprRandomSeed(seeded, 0, 2, true))));
        CHECK(bitsOf(run({k(42), k(10), k(20), k(0.5f), op(ExprOp::RandomSeeded, 0, 2, 4)}, env, nullptr, {},
                         seed)) ==
              bitsOf(10.0f + 10.0f * exprRandom01(exprRandomSeed(seeded, exprRandomFrame(0.5f), 2, false))));
    }
}

TEST_CASE("T-776: a result that is not finite falls back to the row's value") {
    ExprEnv env;
    env.value = 0.75f;
    bool fell = false;
    CHECK(run({k(1), k(0), op(ExprOp::Div)}, env, &fell) == 0.75f);
    CHECK(fell);
    fell = false;
    CHECK(run({k(1), k(0), op(ExprOp::Mod)}, env, &fell) == 0.75f);
    CHECK(fell);
    fell = false;
    CHECK(run({k(-1), op(ExprOp::Log)}, env, &fell) == 0.75f);
    CHECK(fell);
    fell = false;
    CHECK(run({k(3e38f), k(10), op(ExprOp::Mul)}, env, &fell) == 0.75f);
    CHECK(fell);
    fell = false;
    CHECK(run({k(1), k(2), op(ExprOp::Div)}, env, &fell) == 0.5f);
    CHECK_FALSE(fell);
}

// -- members, group time and when programs run ---------------------------------------------------------

TEST_CASE("T-776: a row whose keys ended still follows its program over the group time") {
    // #Box's keys end at 1 s of a 4 s group; value + curve0 differs at 2 s and 3 s.
    auto l = load([](Texp& t, const Names& n) {
        t.add(n.group, n.box, n.boxOpacity, 0,
              {op(ExprOp::PushValue), op(ExprOp::PushCurve), op(ExprOp::Add)},
              {{key(0, 0), key(2, 0.1f), key(3, 0.3f), key(4, 0.3f)}});
    });
    REQUIRE(l->doc->seekTimeline("", "main", 2.0f) == Status::kOk);
    CHECK(l->step(0.1f) == 1);
    CHECK(l->opacity("Box") == doctest::Approx(0.6f));
    REQUIRE(l->doc->seekTimeline("", "main", 3.0f) == Status::kOk);
    CHECK(l->step(0.2f) == 1);
    CHECK(l->opacity("Box") == doctest::Approx(0.8f));
    // Played (from 0 at clock 0.2) across those times, each frame runs it once.
    REQUIRE(l->doc->playTimeline("", "main", hold()) == Status::kOk);
    CHECK(l->step(1.2f) == 1);
    CHECK(l->step(2.2f) == 1);  // Group time 2.0.
    CHECK(l->opacity("Box") == doctest::Approx(0.6f));
    CHECK(l->step(2.7f) == 1);  // 2.5: halfway between the curve's 2 s and 3 s keys.
    CHECK(l->opacity("Box") == doctest::Approx(0.7f));
}

TEST_CASE("T-776: paused, held at the end or released with unchanged inputs runs no program") {
    auto build = [](Texp& t, const Names& n) {
        t.add(n.group, n.box, n.boxOpacity, 0,
              {op(ExprOp::PushValue), op(ExprOp::PushCurve), op(ExprOp::Add)}, {{key(0, 0), key(4, 0.4f)}});
    };
    SUBCASE("paused in the middle") {
        auto l = load(build);
        REQUIRE(l->doc->seekTimeline("", "main", 0.5f) == Status::kOk);
        CHECK(l->step(0.1f) == 1);
        for (int i = 2; i < 8; ++i) {
            CHECK(l->step(0.1f * static_cast<float>(i)) == 0);
        }
        CHECK(l->opacity("Box") == doctest::Approx(0.25f + 0.05f));
        // Paused while playing (a trigger's pause()): the same.
        REQUIRE(l->doc->playTimeline("", "main") == Status::kOk);
        CHECK(l->step(1.0f) == 1);
        xt::ExprTestAccess::pause(*l->doc, l->n.group, true);
        CHECK(l->step(1.1f) <= 1);
        for (int i = 2; i < 8; ++i) {
            CHECK(l->step(1.0f + 0.1f * static_cast<float>(i)) == 0);
        }
    }
    SUBCASE("held at the end") {
        auto l = load(build);
        REQUIRE(l->doc->playTimeline("", "main", hold()) == Status::kOk);
        for (float t = 1.0f; t <= 4.0f; t += 1.0f) {
            CHECK(l->step(t) == 1);
        }
        CHECK_FALSE(l->doc->isTimelinePlaying("", "main"));
        for (float t = 5.0f; t < 10.0f; t += 1.0f) {
            CHECK(l->step(t) == 0);
            CHECK(l->perf.current().animEvaluated == 0);  // W4 still holds for the members.
        }
        CHECK(l->opacity("Box") == doctest::Approx(0.9f));
    }
    SUBCASE("released") {
        auto l = load(build);
        REQUIRE(l->doc->playTimeline("", "main") == Status::kOk);
        for (float t = 1.0f; t <= 4.0f; t += 1.0f) {
            l->step(t);
        }
        for (float t = 5.0f; t < 8.0f; t += 1.0f) {
            CHECK(l->step(t) == 0);
        }
        CHECK(l->opacity("Box") == doctest::Approx(1.0f));  // The styled value again.
    }
}

TEST_CASE("T-776: a port change runs the program once; an unchanged port does not") {
    auto l = load([](Texp& t, const Names& n) {
        t.add(n.group, n.box, n.boxOpacity, 0,
              {op(ExprOp::PushValue), op(ExprOp::PushPort, 0, n.hp), op(ExprOp::Add)});
    });
    const PortId hp = l->doc->lookupPort("hp");
    REQUIRE(hp != 0);
    REQUIRE(hp - 1 == l->n.hp);
    REQUIRE(l->doc->seekTimeline("", "main", 0.5f) == Status::kOk);
    CHECK(l->step(0.1f) == 1);
    CHECK(l->step(0.2f) == 0);
    REQUIRE(l->doc->setPort(hp, Value::ofFloat(0.25f)) == Status::kOk);
    CHECK(l->step(0.3f) == 1);
    CHECK(l->opacity("Box") == doctest::Approx(0.5f));
    CHECK(l->step(0.4f) == 0);
    REQUIRE(l->doc->setPort(hp, Value::ofFloat(0.25f)) == Status::kOk);  // The same value.
    CHECK(l->step(0.5f) == 0);
    // Held at the end (both members at their end stop): a port change still reaches the screen.
    REQUIRE(l->doc->seekTimeline("", "main", 4.0f) == Status::kOk);
    CHECK(l->step(0.6f) == 1);
    CHECK(l->step(0.7f) == 0);
    CHECK(l->perf.current().animEvaluated == 0);  // Held.
    REQUIRE(l->doc->setPort(hp, Value::ofFloat(0.125f)) == Status::kOk);
    CHECK(l->step(0.8f) == 1);
    CHECK(l->opacity("Box") == doctest::Approx(0.625f));
    CHECK(l->step(0.9f) == 0);
}

TEST_CASE("T-783: a member with a program paused mid-way: no program and no member without a new input") {
    auto l = load([](Texp& t, const Names& n) {
        t.add(n.group, n.box, n.boxOpacity, 0,
              {op(ExprOp::PushValue), op(ExprOp::PushPort, 0, n.hp), op(ExprOp::Add)});
    });
    const PortId hp = l->doc->lookupPort("hp");
    REQUIRE(hp != 0);
    REQUIRE(l->doc->playTimeline("", "main") == Status::kOk);
    l->step(0.5f);
    xt::ExprTestAccess::pause(*l->doc, l->n.group, true);
    l->step(0.6f);
    const uint64_t generation = l->doc->visualGeneration();
    for (int i = 0; i < 5; ++i) {
        CHECK(l->step(0.7f + 0.1f * static_cast<float>(i)) == 0);
        CHECK(l->perf.current().animEvaluated == 0);
    }
    CHECK(l->doc->visualGeneration() == generation);
    CHECK(l->opacity("Box") == doctest::Approx(0.25f));
    CHECK(l->opacity("Long") == doctest::Approx(0.125f));
    REQUIRE(l->doc->setPort(hp, Value::ofFloat(0.25f)) == Status::kOk);
    CHECK(l->step(1.3f) == 1);
    CHECK(l->perf.current().animEvaluated == 1);  // #Box only; #Long has no program.
    CHECK(l->opacity("Box") == doctest::Approx(0.5f));
    CHECK(l->step(1.4f) == 0);
    CHECK(l->perf.current().animEvaluated == 0);
}

TEST_CASE("T-776: a layout change runs the program once, with the last layout's box") {
    auto l = load([](Texp& t, const Names& n) {
        t.add(n.group, n.box, n.boxOpacity, 0,
              {op(ExprOp::PushLayout, 2, n.otherElement), k(100), op(ExprOp::Div)});
    });
    REQUIRE(l->doc->seekTimeline("", "main", 0.5f) == Status::kOk);
    CHECK(l->step(0.1f) == 1);
    CHECK(l->opacity("Box") == doctest::Approx(0.3f));
    CHECK(l->step(0.2f) == 0);
    REQUIRE(l->doc->byId("Other").setStyle("width: 80px") == Status::kOk);
    CHECK(l->step(0.3f) == 0);  // The animation step reads the box before this frame's layout.
    CHECK(l->step(0.4f) == 1);  // One frame late (58 §58.5).
    CHECK(l->opacity("Box") == doctest::Approx(0.8f));
    CHECK(l->step(0.5f) == 0);
}

TEST_CASE("T-776: another element's property is read every update, written only when it changed") {
    auto l = load([](Texp& t, const Names& n) {
        t.add(n.group, n.box, n.boxOpacity, 0,
              {op(ExprOp::PushProp, static_cast<float>(prop::Opacity) * 4.0f, n.otherElement)});
    });
    REQUIRE(l->doc->seekTimeline("", "main", 4.0f) == Status::kOk);  // Both members held at the end.
    CHECK(l->step(0.1f) == 1);
    CHECK(l->opacity("Box") == doctest::Approx(0.25f));
    const uint64_t generation = l->doc->visualGeneration();
    for (int i = 2; i < 6; ++i) {
        CHECK(l->step(0.1f * static_cast<float>(i)) == 1);
        CHECK(l->perf.current().animEvaluated == 0);  // Same output: nothing written.
    }
    CHECK(l->doc->visualGeneration() == generation);
    REQUIRE(l->doc->byId("Other").setStyle("opacity: 0.5") == Status::kOk);
    l->step(0.7f);
    l->step(0.8f);
    CHECK(l->opacity("Box") == doctest::Approx(0.5f));
}

TEST_CASE("T-776: seeking to a time gives the value playing to it gives") {
    auto build = [](Texp& t, const Names& n) {
        t.add(n.group, n.box, n.boxOpacity, 0,
              {k(2), k(0.05f), op(ExprOp::Wiggle, 0, 1), op(ExprOp::PushCurve), op(ExprOp::Add), k(0),
               k(0.01f), op(ExprOp::Random, 0, 0), op(ExprOp::Add)},
              {{key(0, 0), key(1.5f, 0.2f), key(4, 0.1f)}});
    };
    auto played = load(build);
    REQUIRE(played->doc->playTimeline("", "main") == Status::kOk);
    for (float t = 0.25f; t <= 2.5f; t += 0.25f) {
        played->step(t);
    }
    CHECK(played->doc->timelineTime("", "main") == 2.5f);
    auto seeked = load(build);
    REQUIRE(seeked->doc->seekTimeline("", "main", 2.5f) == Status::kOk);
    seeked->step(0.1f);
    CHECK(bitsOf(played->opacity("Box")) == bitsOf(seeked->opacity("Box")));
}

TEST_CASE("T-776: the per-play random number is fixed within a play and new for the next") {
    auto build = [](Texp& t, const Names& n) {
        t.add(n.group, n.box, n.boxOpacity, 0, {op(ExprOp::PushPlayRandom, 0, 0)});
    };
    auto first = load(build);
    ExprTestAccess::setSeedBase(*first->doc, 99);
    REQUIRE(first->doc->playTimeline("", "main") == Status::kOk);
    first->step(0.5f);
    const float a = first->opacity("Box");
    CHECK(a >= 0.0f);
    CHECK(a < 1.0f);
    first->step(1.5f);
    CHECK(first->opacity("Box") == a);
    REQUIRE(first->doc->playTimeline("", "main") == Status::kOk);  // Played again: a new draw.
    first->step(2.0f);
    CHECK(first->opacity("Box") != a);
    // The same seed base reproduces the first draw.
    auto second = load(build);
    ExprTestAccess::setSeedBase(*second->doc, 99);
    REQUIRE(second->doc->playTimeline("", "main") == Status::kOk);
    second->step(0.5f);
    CHECK(bitsOf(second->opacity("Box")) == bitsOf(a));
}

TEST_CASE("T-776: valueAt reads this row's keys at any time, clamped outside them") {
    for (const auto& [t, want] : {std::pair{0.5f, 0.25f}, std::pair{-1.0f, 0.0f}, std::pair{10.0f, 0.5f}}) {
        CAPTURE(t);
        auto l = load([t](Texp& x, const Names& n) {
            x.add(n.group, n.box, n.boxOpacity, 0, {k(t), op(ExprOp::ValueAt)});
        });
        REQUIRE(l->doc->seekTimeline("", "main", 3.0f) == Status::kOk);
        l->step(0.1f);
        CHECK(l->opacity("Box") == doctest::Approx(want));
    }
}

TEST_CASE("T-776: RefValueAt reads another row, with its program, its keys, and a depth limit") {
    SUBCASE("a row with a program runs it at that time") {
        auto l = load([](Texp& x, const Names& n) {
            x.add(n.group, n.box, n.boxOpacity, 0, {k(2), op(ExprOp::RefValueAt, 0, 1)});
            x.add(n.group, n.lng, n.longOpacity, 0, {op(ExprOp::PushValue), k(0.1f), op(ExprOp::Add)});
        });
        REQUIRE(l->doc->seekTimeline("", "main", 3.0f) == Status::kOk);
        l->step(0.1f);
        CHECK(l->opacity("Box") == doctest::Approx(0.6f));    // Long's keys at 2 s (0.5) + 0.1.
        CHECK(l->opacity("Long") == doctest::Approx(0.85f));  // Long itself at 3 s.
    }
    SUBCASE("a row without a program gives its keys") {
        auto l = load([](Texp& x, const Names& n) {
            const uint32_t ref = kExprRefRow | (n.lng << 16) | (uint32_t{n.longOpacity} << 4);
            x.add(n.group, n.box, n.boxOpacity, 0, {k(2), op(ExprOp::RefValueAt, 0, ref)});
        });
        REQUIRE(l->doc->seekTimeline("", "main", 3.0f) == Status::kOk);
        l->step(0.1f);
        CHECK(l->opacity("Box") == doctest::Approx(0.5f));
    }
    SUBCASE("nested at most kExprMaxRefDepth deep") {
        // Box = Box(time) + 0.01, referring to itself: four nested runs, then the keys.
        auto l = load([](Texp& x, const Names& n) {
            x.add(n.group, n.box, n.boxOpacity, 0,
                  {op(ExprOp::PushTime), op(ExprOp::RefValueAt, 0, 0), k(0.01f), op(ExprOp::Add)});
        });
        REQUIRE(l->doc->seekTimeline("", "main", 2.0f) == Status::kOk);
        l->step(0.1f);
        CHECK(l->opacity("Box") == doctest::Approx(0.5f + 0.01f * (kExprMaxRefDepth + 1)));
    }
}

TEST_CASE("T-776: a port read with a delay comes from its history, recorded only while playing") {
    auto l = load([](Texp& x, const Names& n) {
        x.add(n.group, n.box, n.boxOpacity, 0, {k(0.5f), op(ExprOp::PushPortDelayed, 0, n.hp)}, {}, 0.5f);
    });
    const PortId hp = l->doc->lookupPort("hp");
    TimelineExprSet& set = ExprTestAccess::exprs(*l->doc);
    // Not playing (seeked, paused): nothing recorded however the port moves.
    REQUIRE(l->doc->seekTimeline("", "main", 0.5f) == Status::kOk);
    for (int i = 1; i <= 4; ++i) {
        REQUIRE(l->doc->setPort(hp, Value::ofFloat(0.25f * static_cast<float>(i))) == Status::kOk);
        l->step(0.125f * static_cast<float>(i));
    }
    CHECK(set.portHistoryCount(l->n.hp) == 0);
    CHECK(l->opacity("Box") == 1.0f);  // No history: the value now.
    // Playing: one record per update; a 0.5 s delay reads the value of 0.5 s ago.
    REQUIRE(l->doc->playTimeline("", "main") == Status::kOk);
    const float start = 1.0f;
    for (int i = 0; i <= 12; ++i) {
        const float c = start + 0.125f * static_cast<float>(i);
        REQUIRE(l->doc->setPort(hp, Value::ofFloat(c * 0.25f)) == Status::kOk);
        l->step(c);
        if (i < 4) {
            CHECK(l->opacity("Box") == start * 0.25f);  // Not enough history: the oldest record.
        } else {
            CHECK(l->opacity("Box") == doctest::Approx((c - 0.5f) * 0.25f));
        }
    }
    CHECK(set.portHistoryCount(l->n.hp) == 13);
}

TEST_CASE("T-781: a property read with a delay comes from its history, recorded only while playing") {
    const float code = static_cast<float>(prop::Opacity) * 4.0f;
    auto l = load([code](Texp& x, const Names& n) {
        x.add(n.group, n.box, n.boxOpacity, 0, {k(0.5f), op(ExprOp::PushPropDelayed, code, n.otherElement)},
              {}, 0.5f);
    });
    REQUIRE(ExprTestAccess::exprs(*l->doc).inputMask(0) == kExprInputPropHistory);
    TimelineExprSet& set = ExprTestAccess::exprs(*l->doc);
    ElementRef other = l->doc->byId("Other");
    const uint32_t ring = static_cast<uint32_t>(code);
    // Not playing (seeked): nothing recorded however the property moves; the value now is read.
    REQUIRE(l->doc->seekTimeline("", "main", 0.5f) == Status::kOk);
    l->step(0.0625f);
    for (int i = 1; i <= 4; ++i) {
        other.setProperty(prop::Opacity, Value::ofFloat(0.1f * static_cast<float>(i)));
        CHECK(l->step(0.125f * static_cast<float>(i)) == 0);  // Stopped: no history, no run.
    }
    CHECK(set.propHistoryCount(l->n.otherElement, ring) == 0);
    // Playing: one record per update; a 0.5 s delay reads the value of 0.5 s ago.
    REQUIRE(l->doc->playTimeline("", "main") == Status::kOk);
    const float start = 1.0f;
    for (int i = 0; i <= 12; ++i) {
        const float c = start + 0.125f * static_cast<float>(i);
        other.setProperty(prop::Opacity, Value::ofFloat(c * 0.25f));
        l->step(c);
        CAPTURE(c);
        if (i < 4) {
            CHECK(l->opacity("Box") == start * 0.25f);  // Not enough history: the oldest record.
        } else {
            CHECK(l->opacity("Box") == doctest::Approx((c - 0.5f) * 0.25f));
        }
    }
    CHECK(set.propHistoryCount(l->n.otherElement, ring) == 13);
    CHECK(set.portHistoryCount(l->n.hp) == 0);
    // Paused: the history stops, so the program never runs, however the property moves.
    ExprTestAccess::pause(*l->doc, l->n.group, true);
    CHECK(l->step(2.6f) <= 1);
    const float held = l->opacity("Box");
    for (int i = 1; i <= 5; ++i) {
        other.setProperty(prop::Opacity, Value::ofFloat(0.05f * static_cast<float>(i)));
        CHECK(l->step(2.6f + 0.1f * static_cast<float>(i)) == 0);
    }
    CHECK(set.propHistoryCount(l->n.otherElement, ring) == 13);
    CHECK(l->opacity("Box") == held);
}

TEST_CASE(
    "T-781: a member held at its end reading a property's history keeps static frames static (verify mode)") {
    std::vector<uint8_t> ir = compileIr();
    const Names n = names(ir);
    Texp texp;
    texp.add(
        n.group, n.box, n.boxOpacity, 0,
        {k(0.25f), op(ExprOp::PushPropDelayed, static_cast<float>(prop::Opacity) * 4.0f, n.otherElement)}, {},
        0.25f);
    appendChunk(ir, kChunkTEXP, texp.payload());
    FileAssetResolver assets(std::string(XT_TEST_XT_DIR), nullptr);
    ContextConfig config;
    config.assets = &assets;
    Context ctx(config);
    ctx.setVerifyStaticSkip(true);
    ctx.perf().setEnabled(true);
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
    REQUIRE(handle.ok());
    Document& d = *ctx.document(handle.value());
    CommandList list;
    DamageList damage;
    REQUIRE(d.playTimeline("", "main", hold()) == Status::kOk);
    for (int i = 0; i < 30; ++i) {  // Playing: recorded and run every update.
        ctx.update(1.0f / 30.0f);
        ctx.render(list, &damage);
        CHECK(ctx.perf().current().staticSkipMismatch == 0);
    }
    CHECK(ExprTestAccess::exprs(d).propHistoryCount(n.otherElement,
                                                    static_cast<uint32_t>(prop::Opacity) * 4u) >= 29);
    for (int i = 0; i < 12 && d.isTimelinePlaying("", "main"); ++i) {  // On to the end (4 s): held.
        ctx.update(0.25f);
        ctx.render(list, &damage);
    }
    REQUIRE_FALSE(d.isTimelinePlaying("", "main"));
    for (int i = 0; i < 3; ++i) {
        ctx.update(1.0f / 60.0f);
        ctx.render(list, &damage);
    }
    for (int i = 0; i < 5; ++i) {
        ctx.update(1.0f / 60.0f);
        ctx.render(list, &damage);
        const FrameStats& f = ctx.perf().current();
        CHECK(f.exprEvaluated == 0);
        CHECK(f.paintSkipped == 1);
        CHECK(f.staticSkipMismatch == 0);
    }
}

TEST_CASE("T-776: a held member's program reading a property keeps static frames static (verify mode)") {
    std::vector<uint8_t> ir = compileIr();
    const Names n = names(ir);
    Texp texp;
    texp.add(n.group, n.box, n.boxOpacity, 0,
             {op(ExprOp::PushProp, static_cast<float>(prop::Opacity) * 4.0f, n.otherElement)});
    appendChunk(ir, kChunkTEXP, texp.payload());
    FileAssetResolver assets(std::string(XT_TEST_XT_DIR), nullptr);
    ContextConfig config;
    config.assets = &assets;
    Context ctx(config);
    ctx.setVerifyStaticSkip(true);
    ctx.perf().setEnabled(true);
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
    REQUIRE(handle.ok());
    Document& d = *ctx.document(handle.value());
    REQUIRE(d.seekTimeline("", "main", 4.0f) == Status::kOk);  // Both members held at the end.
    CommandList list;
    DamageList damage;  // The skip needs the previous list as the damage base.
    for (int i = 0; i < 4; ++i) {
        ctx.update(1.0f / 60.0f);
        ctx.render(list, &damage);
    }
    for (int i = 0; i < 5; ++i) {
        ctx.update(1.0f / 60.0f);
        ctx.render(list, &damage);
        const FrameStats& f = ctx.perf().current();
        CHECK(f.exprEvaluated == 1);
        CHECK(f.paintSkipped == 1);
        CHECK(f.staticSkipMismatch == 0);
    }
    REQUIRE(d.byId("Other").setStyle("opacity: 0.5") == Status::kOk);
    for (int i = 0; i < 3; ++i) {
        ctx.update(1.0f / 60.0f);
        ctx.render(list, &damage);
        CHECK(ctx.perf().current().staticSkipMismatch == 0);
    }
    CHECK(d.byId("Box").property(prop::Opacity).f == doctest::Approx(0.5f));
}

// -- the IR ---------------------------------------------------------------------------------------------

namespace {

/// A valid TEXP for the default document, then `mutate` applied; returns IrView::openError's text
/// ("" = accepted).
template <class Mutate>
std::string texpError(Mutate mutate) {
    std::vector<uint8_t> ir = compileIr();
    const Names n = names(ir);
    Texp t;
    t.add(n.group, n.box, n.boxOpacity, 0, {op(ExprOp::PushValue), op(ExprOp::PushCurve), op(ExprOp::Add)},
          {{key(0, 0), key(1, 1)}});
    mutate(t, n);
    appendChunk(ir, kChunkTEXP, t.payload());
    return IrView::openError(IrBlob{ir.data(), ir.size()});
}

}  // namespace

TEST_CASE("T-776: the validator rejects each broken TEXP rule") {
    CHECK(texpError([](Texp&, const Names&) {}).empty());
    struct Rule {
        const char* expect;
        void (*mutate)(Texp&, const Names&);
    };
    const Rule rules[] = {
        {"key range", [](Texp& t, const Names&) { t.curves[0].keyCount = 0; }},
        {"key range", [](Texp& t, const Names&) { t.curves[0].keyOffset = 5; }},
        {"not finite",
         [](Texp& t, const Names&) { t.keys[1].value = std::numeric_limits<float>::infinity(); }},
        {"interpolation", [](Texp& t, const Names&) { t.keys[0].interp = 2; }},
        {"time order", [](Texp& t, const Names&) { t.keys[1].time = -1.0f; }},
        {"no timeline group", [](Texp& t, const Names&) { t.exprs[0].group = 99; }},
        {"no member", [](Texp& t, const Names&) { t.exprs[0].member = 99; }},
        {"no track", [](Texp& t, const Names&) { t.exprs[0].track = 999; }},
        {"component", [](Texp& t, const Names&) { t.exprs[0].component = 1; }},
        {"program is empty", [](Texp& t, const Names&) { t.exprs[0].opCount = 0; }},
        {"program is empty", [](Texp& t, const Names&) { t.exprs[0].opCount = 257; }},
        {"program is empty", [](Texp& t, const Names&) { t.exprs[0].opOffset = 2; }},
        {"curve range", [](Texp& t, const Names&) { t.exprs[0].curveCount = 2; }},
        {"history", [](Texp& t, const Names&) { t.exprs[0].portHistorySeconds = -1.0f; }},
        {"history",
         [](Texp& t, const Names&) { t.exprs[0].index = std::numeric_limits<float>::quiet_NaN(); }},
        {"unknown op", [](Texp& t, const Names&) { t.ops[0].op = kExprOpLast + 1; }},
        {"operand count", [](Texp& t, const Names&) { t.ops[2].argc = 3; }},
        {"PushConst", [](Texp& t, const Names&) { t.ops[0] = k(std::numeric_limits<float>::quiet_NaN()); }},
        {"no curve", [](Texp& t, const Names&) { t.ops[1].ref = 1; }},
        {"CurveAt names no curve", [](Texp& t, const Names&) { t.ops[1] = op(ExprOp::CurveAt, 0, 1); }},
        {"RandomSeeded's mode",
         [](Texp& t, const Names&) {
             t.ops = {k(1), k(0), k(1), op(ExprOp::RandomSeeded, 2)};
             t.exprs[0].opCount = 4;
             t.exprs[0].maxStack = 3;
         }},
        {"no port", [](Texp& t, const Names&) { t.ops[1] = op(ExprOp::PushPort, 0, 99); }},
        {"no port", [](Texp& t, const Names&) { t.ops[1] = op(ExprOp::PushPortDelayed, 0, 99); }},
        {"PushProp names", [](Texp& t, const Names&) { t.ops[1] = op(ExprOp::PushProp, 4, 9999); }},
        {"property code",
         [](Texp& t, const Names& n) { t.ops[1] = op(ExprOp::PushProp, 4.5f, n.otherElement); }},
        {"PushLayout names", [](Texp& t, const Names&) { t.ops[1] = op(ExprOp::PushLayout, 0, 9999); }},
        // T-781: PushPropDelayed names an element and a property this runtime knows.
        {"PushPropDelayed names no element",
         [](Texp& t, const Names&) {
             t.ops[1] = op(ExprOp::PushPropDelayed, static_cast<float>(prop::Opacity) * 4.0f, 9999);
         }},
        {"PushPropDelayed's property code",
         [](Texp& t, const Names& n) { t.ops[1] = op(ExprOp::PushPropDelayed, 4.5f, n.otherElement); }},
        {"PushPropDelayed's property code",
         [](Texp& t, const Names& n) {
             t.ops[1] = op(ExprOp::PushPropDelayed, 65000.0f * 4.0f, n.otherElement);
         }},
        {"PushPropDelayed's property code",
         [](Texp& t, const Names& n) { t.ops[1] = op(ExprOp::PushPropDelayed, -4.0f, n.otherElement); }},
        {"box value", [](Texp& t, const Names& n) { t.ops[1] = op(ExprOp::PushLayout, 4, n.otherElement); }},
        {"mode",
         [](Texp& t, const Names&) {
             t.ops = {k(0), k(1), op(ExprOp::Random, 2)};
             t.exprs[0].opCount = 3;
             t.exprs[0].maxStack = 2;
         }},
        {"no expression",
         [](Texp& t, const Names&) {
             t.ops = {k(0), op(ExprOp::RefValueAt, 0, 5)};
             t.exprs[0].opCount = 2;
             t.exprs[0].maxStack = 1;
         }},
        {"no member of the group",
         [](Texp& t, const Names&) {
             t.ops = {k(0), op(ExprOp::RefValueAt, 0, kExprRefRow | (99u << 16))};
             t.exprs[0].opCount = 2;
             t.exprs[0].maxStack = 1;
         }},
        {"empty stack", [](Texp& t, const Names&) { t.ops[0] = op(ExprOp::Neg); }},
        {"maxStack", [](Texp& t, const Names&) { t.exprs[0].maxStack = 3; }},
        {"maxStack",
         [](Texp& t, const Names&) {
             std::vector<IrExprOp> prog(33, k(1));
             prog.insert(prog.end(), 32, op(ExprOp::Add));
             t.ops = prog;
             t.exprs[0].opCount = static_cast<uint16_t>(prog.size());
             t.exprs[0].maxStack = 33;
         }},
        {"exactly one value",
         [](Texp& t, const Names&) {
             t.ops[2] = k(1);
             t.exprs[0].maxStack = 3;
         }},
        {"same component", [](Texp& t, const Names& n) { t.add(n.group, n.box, n.boxOpacity, 0, {k(1)}); }},
    };
    for (const Rule& r : rules) {
        CAPTURE(r.expect);
        const std::string error = texpError(r.mutate);
        CHECK(error.find("TEXP") != std::string::npos);
        CHECK(error.find(r.expect) != std::string::npos);
    }
    // Structural: a second TEXP chunk, a truncated payload.
    std::vector<uint8_t> ir = compileIr();
    const Names n = names(ir);
    Texp t;
    t.add(n.group, n.box, n.boxOpacity, 0, {k(1)});
    std::vector<uint8_t> twice = ir;
    appendChunk(twice, kChunkTEXP, t.payload());
    appendChunk(twice, kChunkTEXP, t.payload());
    CHECK_FALSE(IrView::open(IrBlob{twice.data(), twice.size()}).ok());
    std::vector<uint8_t> payload = t.payload();
    payload.resize(payload.size() - 4);
    appendChunk(ir, kChunkTEXP, payload);
    CHECK_FALSE(IrView::open(IrBlob{ir.data(), ir.size()}).ok());
}

TEST_CASE("T-776: an IR without TEXP plays as before; the regression seed opens, loads and plays") {
    // Without TEXP nothing runs.
    std::vector<uint8_t> plain = compileIr();
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{plain.data(), plain.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    PerfTracker perf;
    perf.setEnabled(true);
    doc->setPerfTracker(&perf);
    REQUIRE(doc->playTimeline("", "main") == Status::kOk);
    perf.beginFrame(0.5f);
    doc->update(0.5f);
    CHECK(perf.current().exprEvaluated == 0);
    CHECK(perf.current().animEvaluated == 2);

    // Tests/fuzz/regressions/ir/texp_programs.bin: a document with a program of every input kind
    // (rewritten with XT_WRITE_TEXP_SEED=1), replayed by run_fuzz.sh before the corpus.
    const std::string path = std::string(XT_TEST_XT_DIR) + "/../fuzz/regressions/ir/texp_programs.bin";
    if (!xt::test::tempEnv("XT_WRITE_TEXP_SEED").empty()) {
        std::vector<uint8_t> ir = compileIr();
        const Names n = names(ir);
        Texp t;
        t.add(n.group, n.box, n.boxOpacity, 0,
              {op(ExprOp::PushValue), op(ExprOp::PushPort, 0, n.hp), op(ExprOp::Add), op(ExprOp::PushCurve),
               op(ExprOp::Add), op(ExprOp::PushLayout, 2, n.otherElement), op(ExprOp::Add), k(2), k(1),
               op(ExprOp::Wiggle, 0, 1), op(ExprOp::Add), op(ExprOp::PushPlayRandom), op(ExprOp::Add),
               k(0.5f), op(ExprOp::PushPortDelayed, 0, n.hp), op(ExprOp::Add)},
              {{key(0, 0), key(2, 1, 1), key(4, 0)}}, 0.5f);
        t.add(
            n.group, n.lng, n.longOpacity, 0,
            {op(ExprOp::PushTime), op(ExprOp::RefValueAt, 0, 0), op(ExprOp::PushProp, 4, n.otherElement),
             op(ExprOp::Max), k(0), k(1), op(ExprOp::Random, 1, 3), op(ExprOp::Mul),
             // T-777: CurveAt and RandomSeeded.
             op(ExprOp::PushTime), op(ExprOp::CurveAt), op(ExprOp::Add), k(42), k(0), k(1),
             op(ExprOp::RandomSeeded, 0, 4), op(ExprOp::Add),
             // T-781: PushPropDelayed.
             k(0.25f), op(ExprOp::PushPropDelayed, static_cast<float>(prop::Opacity) * 4.0f, n.otherElement),
             op(ExprOp::Add)},
            {{key(0, 0), key(4, 1)}}, 0.25f);
        appendChunk(ir, kChunkTEXP, t.payload());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(ir.data()), static_cast<std::streamsize>(ir.size()));
    }
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> seed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(seed.empty());
    Result<IrView> view = IrView::open(IrBlob{seed.data(), seed.size()});
    REQUIRE(view.ok());
    CHECK(view.value().timelineExprs().exprs().size() == 2);
    ContextConfig config;
    Context ctx(config);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{seed.data(), seed.size()}, false);
    REQUIRE(handle.ok());
    ctx.perf().setEnabled(true);
    ctx.setViewport(Rect{0, 0, 800, 600}, 1.0f);
    REQUIRE(ctx.document(handle.value())->playTimeline("", "main") == Status::kOk);
    CommandList cmds;
    for (int i = 0; i < 3; ++i) {
        ctx.update(1.0f / 60.0f);
        ctx.render(cmds);
        CHECK(ctx.perf().current().exprEvaluated == 2);
    }
}
