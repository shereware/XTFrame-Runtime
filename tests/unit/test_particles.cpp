#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/command_simplifier.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/particles.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"

/// `<particles>` (docs/design/03-scene-and-properties.md §3.12.5, 18 §18.12, docs/tasks/T-202.md):
/// the CPU simulation (emission rate, lifetime, cap, determinism, gravity/drag, burst), the
/// element (attributes, `.xts`, play/stop), DrawParticles emission, the simplifier rule and the
/// software rasterization of solid normal/additive quads.

using namespace xt;
using namespace xt::compile;

namespace {

/// A fresh emitter with no randomness in the parts a test measures.
ParticleNodeData emitter() {
    ParticleNodeData d;
    d.rate = 0;
    d.lifetime[0] = d.lifetime[1] = 100;
    d.speed[0] = d.speed[1] = 0;
    d.spread = 0;
    d.seed = 1234;
    d.emitting = 1;
    return d;
}

const Rect kBox{0, 0, 10, 10};

/// Steps `d` by `seconds` in exact fixed steps (one stepParticles call per step).
void run(ParticleNodeData& d, float seconds) {
    const int steps = static_cast<int>(std::lround(seconds / kParticleStep));
    for (int i = 0; i < steps; ++i) {
        stepParticles(d, kBox, kParticleStep);
    }
}

std::string doc(std::string_view body, std::string_view style = "") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"body\">" +
           std::string(body) + "</body></xt>";
}

CompileResult compileXt(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    options.rootDir = XT_TEST_XT_DIR;
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

bool hasError(const CompileResult& r, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct Loaded {
    CompileResult result;
    std::unique_ptr<Document> doc;
};

Loaded load(const std::string& xt) {
    Loaded out;
    out.result = compileXt(xt);
    for (const Diagnostic& d : out.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 200, 200});
    return out;
}

const ParticleNodeData& particles(const Document& d, std::string_view id) {
    const ParticleNodeData* p = d.particleData(d.byId(id).node);
    REQUIRE(p != nullptr);
    return *p;
}

bool sameParticles(const ParticleNodeData& a, const ParticleNodeData& b) {
    if (a.particles.size() != b.particles.size()) {
        return false;
    }
    for (size_t i = 0; i < a.particles.size(); ++i) {
        const Particle& p = a.particles[i];
        const Particle& q = b.particles[i];
        if (p.pos.x != q.pos.x || p.pos.y != q.pos.y || p.vel.x != q.vel.x || p.vel.y != q.vel.y ||
            p.life != q.life || p.rot != q.rot) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("particles: rate emission with a fractional carry") {
    ParticleNodeData d = emitter();
    d.rate = 30;  // 0.25 per step
    run(d, 1.0f);
    CHECK(d.particles.size() >= 29);
    CHECK(d.particles.size() <= 30);
    run(d, 1.0f);
    CHECK(d.particles.size() >= 59);
    CHECK(d.particles.size() <= 60);
    d.emitting = 0;
    const size_t before = d.particles.size();
    run(d, 0.5f);
    CHECK(d.particles.size() == before);  // stopped: nothing new, nobody died (life 100 s)
}

TEST_CASE("particles: lifetime recycling") {
    ParticleNodeData d = emitter();
    d.lifetime[0] = d.lifetime[1] = 0.5f;
    burstParticles(d, kBox, 10);
    CHECK(d.particles.size() == 10);
    run(d, 0.4f);
    CHECK(d.particles.size() == 10);
    run(d, 0.2f);
    CHECK(d.particles.empty());
    // A steady state: rate x life particles alive.
    d.rate = 40;
    run(d, 2.0f);
    CHECK(d.particles.size() >= 19);
    CHECK(d.particles.size() <= 21);
}

TEST_CASE("particles: maxParticles caps burst and rate") {
    ParticleNodeData d = emitter();
    d.maxParticles = 16;
    burstParticles(d, kBox, 100);
    CHECK(d.particles.size() == 16);
    d.rate = 1000;
    run(d, 0.5f);
    CHECK(d.particles.size() == 16);
    d.maxParticles = 0;
    d.particles.clear();
    burstParticles(d, kBox, 5);
    CHECK(d.particles.empty());
}

TEST_CASE("particles: deterministic streams") {
    ParticleNodeData a = emitter();
    a.rate = 50;
    a.speed[0] = 10;
    a.speed[1] = 90;
    a.spread = 120;
    a.lifetime[0] = 0.5f;
    a.lifetime[1] = 1.5f;
    a.rotation[1] = 360;
    a.emitterShape = ParticleShape::Rect;
    ParticleNodeData b = a;
    burstParticles(a, kBox, 7);
    burstParticles(b, kBox, 7);
    run(a, 0.75f);
    run(b, 0.75f);
    CHECK(sameParticles(a, b));
    // Another seed (another element id) is another stream.
    ParticleNodeData c = emitter();
    c = ParticleNodeData(a);
    c.particles.clear();
    c.emitSeq = 0;
    c.seed = particleSeed("Other");
    ParticleNodeData e = c;
    e.seed = particleSeed("Sparks");
    burstParticles(c, kBox, 7);
    burstParticles(e, kBox, 7);
    CHECK_FALSE(sameParticles(c, e));
    CHECK(particleSeed("Sparks") == particleSeed("Sparks"));
    // Every emitter point of each shape stays within the box (edge: on its perimeter).
    for (ParticleShape shape :
         {ParticleShape::Point, ParticleShape::Rect, ParticleShape::Circle, ParticleShape::Edge}) {
        ParticleNodeData s = emitter();
        s.emitterShape = shape;
        burstParticles(s, kBox, 64);
        for (const Particle& p : s.particles) {
            CHECK(p.pos.x >= 0.0f);
            CHECK(p.pos.x <= 10.0f);
            CHECK(p.pos.y >= 0.0f);
            CHECK(p.pos.y <= 10.0f);
            if (shape == ParticleShape::Point) {
                CHECK(p.pos.x == doctest::Approx(5));
                CHECK(p.pos.y == doctest::Approx(5));
            }
            if (shape == ParticleShape::Edge) {
                const bool onEdge =
                    p.pos.x <= 1e-4f || p.pos.x >= 10 - 1e-4f || p.pos.y <= 1e-4f || p.pos.y >= 10 - 1e-4f;
                CHECK(onEdge);
            }
        }
    }
}

TEST_CASE("particles: gravity and drag integrate per fixed step") {
    // Straight right at 100 px/s: 100 px in 1 s.
    ParticleNodeData d = emitter();
    d.speed[0] = d.speed[1] = 100;
    d.direction = 0;
    burstParticles(d, kBox, 1);
    run(d, 1.0f);
    REQUIRE(d.particles.size() == 1);
    CHECK(d.particles[0].pos.x == doctest::Approx(105).epsilon(0.001));
    CHECK(d.particles[0].pos.y == doctest::Approx(5).epsilon(0.001));
    // Gravity 200 from rest, semi-implicit Euler: v = 200 after 120 steps,
    // y = 200 dt^2 * (120 * 121 / 2) = 100.8333 below the start.
    ParticleNodeData g = emitter();
    g.gravity = 200;
    burstParticles(g, kBox, 1);
    run(g, 1.0f);
    REQUIRE(g.particles.size() == 1);
    CHECK(g.particles[0].vel.y == doctest::Approx(200).epsilon(0.001));
    CHECK(g.particles[0].pos.y - 5.0f == doctest::Approx(100.8333).epsilon(0.001));
    // Drag 0.5 per second: v = 100 (1 - 0.5 / 120)^120 = 60.590.
    ParticleNodeData r = emitter();
    r.speed[0] = r.speed[1] = 100;
    r.direction = 0;
    r.drag = 0.5f;
    burstParticles(r, kBox, 1);
    run(r, 1.0f);
    REQUIRE(r.particles.size() == 1);
    CHECK(r.particles[0].vel.x == doctest::Approx(60.590).epsilon(0.001));
    // -90 is up.
    ParticleNodeData u = emitter();
    u.speed[0] = u.speed[1] = 50;
    burstParticles(u, kBox, 1);
    CHECK(u.particles[0].vel.x == doctest::Approx(0).epsilon(0.001));
    CHECK(u.particles[0].vel.y == doctest::Approx(-50).epsilon(0.001));
}

TEST_CASE("particles: size and 4-stop color over the life") {
    ParticleNodeData d = emitter();
    d.size[0] = 8;
    d.size[1] = 2;
    d.lifetime[0] = d.lifetime[1] = 1;
    d.color[0] = Color{1, 0, 0, 1};
    d.color[1] = Color{0, 1, 0, 1};
    d.color[2] = Color{0, 0, 1, 1};
    d.color[3] = Color{1, 1, 1, 0};
    burstParticles(d, kBox, 1);
    Particle p = d.particles[0];
    CHECK(particleSize(p) == doctest::Approx(8));
    CHECK(particleColor(d, p).r == doctest::Approx(1));
    p.age = 0.5f;
    CHECK(particleSize(p) == doctest::Approx(5));
    const Color mid = particleColor(d, p);  // halfway between stop 1 and stop 2
    CHECK(mid.g == doctest::Approx(0.5));
    CHECK(mid.b == doctest::Approx(0.5));
    p.age = 1.0f;
    CHECK(particleColor(d, p).a == doctest::Approx(0));
}

TEST_CASE("particles: attributes, .xts and style compile to the particle PropIds") {
    Loaded l =
        load(doc(R"~(<particles id="P" shape="circle" rate="30" burst="4" lifetime="0.6 1.2"
        speed="80 160" direction="-45" spread="30" gravity="200" drag="0.1" size="6 0"
        color="#ff0000 #0000ff" rotation="0 360" blend="additive" max="99" autoplay></particles>
        <particles id="Q" style="particle-max: 12"></particles>)~",
                 "#Q { particle-rate: 5; particle-lifetime: 2; particle-color: #00ff00; particle-max: 50; "
                 "particle-size: 3 4; } particles { width: 20px; height: 10px; }"));
    Document& d = *l.doc;
    d.update(0);
    const ParticleNodeData& p = particles(d, "P");
    CHECK(p.emitterShape == ParticleShape::Circle);
    CHECK(p.blend == ParticleBlend::Additive);
    CHECK(p.rate == doctest::Approx(30));
    CHECK(p.burst == doctest::Approx(4));
    CHECK(p.lifetime[0] == doctest::Approx(0.6));
    CHECK(p.lifetime[1] == doctest::Approx(1.2));
    CHECK(p.speed[1] == doctest::Approx(160));
    CHECK(p.direction == doctest::Approx(-45));
    CHECK(p.spread == doctest::Approx(30));
    CHECK(p.gravity == doctest::Approx(200));
    CHECK(p.drag == doctest::Approx(0.1));
    CHECK(p.size[0] == doctest::Approx(6));
    CHECK(p.size[1] == doctest::Approx(0));
    CHECK(p.color[0].r == doctest::Approx(1));
    CHECK(p.color[1].r == doctest::Approx(2.0 / 3.0).epsilon(0.01));  // two colors: the a->b line
    CHECK(p.color[1].b == doctest::Approx(1.0 / 3.0).epsilon(0.01));
    CHECK(p.color[3].b == doctest::Approx(1));
    CHECK(p.rotation[1] == doctest::Approx(360));
    CHECK(p.maxParticles == 99);
    CHECK(p.particles.size() == 4);  // autoplay: the burst on the first update
    const ElementRef q = d.byId("Q");
    const ParticleNodeData& qp = particles(d, "Q");
    CHECK(qp.rate == doctest::Approx(5));
    CHECK(qp.lifetime[0] == doctest::Approx(2));
    CHECK(qp.lifetime[1] == doctest::Approx(2));
    CHECK(qp.color[2].g == doctest::Approx(1));
    CHECK(qp.size[1] == doctest::Approx(4));
    CHECK(qp.maxParticles == 12);  // the style attribute wins over the rule
    CHECK(qp.emitting == 0);       // no autoplay
    CHECK(d.arena().element(q.node).style.base[prop::ParticleRate].f == doctest::Approx(5));
    CHECK(d.arena().kind(d.particlesOf(q.node)) == NodeKind::Particles);

    CHECK(hasError(compileXt(doc(R"~(<particles shape="hex"></particles>)~")), "invalid \"shape\""));
    CHECK(hasError(compileXt(doc(R"~(<particles blend="screen"></particles>)~")), "invalid \"blend\""));
    CHECK(hasError(compileXt(doc(R"~(<particles rate="fast"></particles>)~")), "invalid <particles>"));
    CHECK(hasError(compileXt(doc(R"~(<particles><div></div></particles>)~")), "must not have children"));
    CHECK(hasError(compileXt(doc(R"~(<particles id="P"></particles><div on:click="seek(P, 1s)"></div>)~")),
                   "seek() does not apply to particles"));
}

TEST_CASE("particles: play/stop triggers switch emission; editor mode keeps an emitter still") {
    {
        Loaded l = load(doc(R"~(<particles id="P" burst="3" rate="60" lifetime="5"></particles>)~",
                            "#P { width: 10px; height: 10px; } body { width: 100px; height: 100px; }"));
        Document& d = *l.doc;
        d.update(0);
        CHECK(particles(d, "P").particles.empty());
        CHECK(d.playParticles(d.byId("P").node) == Status::kOk);
        d.update(0);
        CHECK(particles(d, "P").particles.size() == 3);  // the burst
        d.update(0.5f);
        CHECK(particles(d, "P").particles.size() >= 32);
        CHECK(d.stopParticles(d.byId("P").node) == Status::kOk);
        const size_t alive = particles(d, "P").particles.size();
        d.update(1.0f);
        CHECK(particles(d, "P").particles.size() == alive);  // stopped, still alive (life 5 s)
        CHECK(d.playParticles(d.byId("body").node) == Status::kInvalidArgument);
    }
    {
        // `on:show` runs play(P) / stop(Q) on the first update.
        Loaded l = load(R"~(<xt><body id="body" on:show="play(P) then stop(Q)">
            <particles id="P" burst="5" rate="0" lifetime="5"></particles>
            <particles id="Q" burst="5" rate="0" lifetime="5" autoplay></particles></body></xt>)~");
        Document& d = *l.doc;
        d.update(0);
        CHECK(particles(d, "P").particles.size() == 5);
        CHECK(particles(d, "Q").particles.empty());
        CHECK(particles(d, "Q").emitting == 0);
    }
    {
        Loaded l = load(doc(R"~(<particles id="A" rate="60" lifetime="5" autoplay></particles>
            <particles id="B" rate="60" lifetime="5" simulate-in-editor="false" autoplay></particles>)~"));
        Document& d = *l.doc;
        d.setParticleEditorMode(true);
        d.update(0);
        d.update(0.5f);
        CHECK(particles(d, "A").particles.size() >= 29);
        CHECK(particles(d, "B").particles.empty());
        CHECK(particles(d, "B").simulateInEditor == 0);
        d.setParticleEditorMode(false);
        d.update(1.0f);
        CHECK(particles(d, "B").particles.size() >= 29);
    }
}

TEST_CASE("particles: DrawParticles in paint order, dump, simplifier") {
    Loaded l =
        load(doc(R"~(<div id="Under"></div><particles id="P" burst="6" rate="0" lifetime="5"
        size="4" color="#ffffff80" autoplay></particles><div id="Over"></div>)~",
                 "div, particles { position: absolute; left: 20px; top: 30px; width: 40px; height: 40px; } "
                 "div { background-color: #ff0000; }"));
    Document& d = *l.doc;
    d.update(0);
    CommandList list;
    d.paint(list);
    int particlesAt = -1;
    int fills = 0;
    for (size_t i = 0; i < list.commands.size(); ++i) {
        if (list.commands[i].kind == CmdKind::DrawParticles) {
            particlesAt = static_cast<int>(i);
            CHECK(fills == 1);  // after #Under, before #Over
        } else if (list.commands[i].kind == CmdKind::FillPath) {
            ++fills;
        }
    }
    REQUIRE(particlesAt >= 0);
    CHECK(fills == 2);
    const ParticlesCmd& pc = list.particles[list.commands[static_cast<size_t>(particlesAt)].index];
    CHECK(pc.count == 6);
    CHECK(pc.texture == kInvalidAsset);
    CHECK(pc.blend == ParticleBlend::Normal);
    CHECK(pc.xf.e == doctest::Approx(20));
    CHECK(pc.xf.f == doctest::Approx(30));
    const ParticleInstance& first = list.particleInstances[pc.instanceOffset];
    CHECK(first.x == doctest::Approx(20));  // point emitter: the box center, local coordinates
    CHECK(first.w == doctest::Approx(4));
    CHECK(first.rgba.a == doctest::Approx(128.0 / 255.0).epsilon(0.01));
    CHECK(first.rgba.r == doctest::Approx(first.rgba.a));  // premultiplied white
    std::string dump;
    dumpCommandList(list, dump);
    CHECK(dump.find("DrawParticles texture=none blend=normal count=6") != std::string::npos);
    CHECK(dump.find("  particle c=(20.000,20.000) size=(4.000,4.000)") != std::string::npos);

    CommandListSimplifier simplifier;
    CommandList kept = list;
    SimplifyOptions keep;
    keep.particles = false;
    SimplifyStats stats = simplifier.simplify(kept, keep, nullptr);
    CHECK(stats.droppedParticles == 0);
    CHECK(kept.particles.size() == 1);
    CHECK(kept.particleInstances.size() == 6);
    CommandList dropped = list;
    stats = simplifier.simplify(dropped, SimplifyOptions{}, nullptr);
    CHECK(stats.droppedParticles == 6);
    CHECK(dropped.particles.empty());
    for (const Command& c : dropped.commands) {
        CHECK(c.kind != CmdKind::DrawParticles);
    }

    // visibility: hidden hides them; no live particles, no command.
    Loaded h = load(doc(R"~(<particles burst="3" autoplay style="visibility: hidden"></particles>
        <particles id="E"></particles>)~"));
    h.doc->update(0);
    CommandList none;
    h.doc->paint(none);
    CHECK(none.particles.empty());
}

TEST_CASE("particles: software quads, normal and additive") {
    constexpr int kW = 16;
    constexpr int kH = 16;
    auto render = [](ParticleBlend blend, Color backdrop, float rot) {
        CommandList list;
        list.viewport = Rect{0, 0, kW, kH};
        ParticleInstance p{};
        p.x = 8;
        p.y = 8;
        p.w = 8;
        p.h = 8;
        p.rot = rot;
        p.rgba = Color{0.5f, 0.0f, 0.0f, 0.5f};  // premultiplied half-transparent red
        p.uv0 = Vec2{0, 0};
        p.uv1 = Vec2{1, 1};
        list.particleInstances.push_back(p);
        ParticlesCmd pc{};
        pc.texture = kInvalidAsset;
        pc.blend = blend;
        pc.instanceOffset = 0;
        pc.count = 1;
        list.particles.push_back(pc);
        list.commands.push_back(Command{CmdKind::DrawParticles, {}, 0});
        std::vector<uint8_t> buf(static_cast<size_t>(kW) * kH * 4, 0);
        sw::SwRenderer renderer;
        sw::SwTarget target{buf.data(), kW, kH, kW * 4};
        renderer.setTarget(target);
        renderer.clear(backdrop);
        renderer.render(list, nullptr, nullptr);
        return buf;
    };
    auto px = [](const std::vector<uint8_t>& b, int x, int y, int c) {
        return static_cast<int>(
            b[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)]);
    };
    // Normal over opaque blue: 0.5 red over blue.
    std::vector<uint8_t> normal = render(ParticleBlend::Normal, Color{0, 0, 1, 1}, 0);
    CHECK(px(normal, 8, 8, 0) == doctest::Approx(128).epsilon(0.02));
    CHECK(px(normal, 8, 8, 2) == doctest::Approx(128).epsilon(0.02));
    CHECK(px(normal, 1, 1, 2) == 255);  // outside the quad
    // Additive over opaque blue: red added, blue kept.
    std::vector<uint8_t> additive = render(ParticleBlend::Additive, Color{0, 0, 1, 1}, 0);
    CHECK(px(additive, 8, 8, 0) == doctest::Approx(128).epsilon(0.02));
    CHECK(px(additive, 8, 8, 2) == 255);
    // A 45 degree rotation: the corner of the axis-aligned square is now outside, the tip inside.
    std::vector<uint8_t> rotated = render(ParticleBlend::Normal, Color{0, 0, 0, 1}, 3.14159265f / 4.0f);
    CHECK(px(rotated, 4, 4, 0) == 0);
    CHECK(px(rotated, 8, 3, 0) > 60);
}

TEST_CASE("particles: @timeline and bind: drive the particle PropIds") {
    Loaded l = load(R"~(<xt><head><style>
        @timeline ramp { #P { 0s { particle-rate: 0 } 1s { particle-rate: 100 } } }
        </style></head><body id="body" on:show="play(ramp)">
        <particles id="P" rate="0" port:r="number in" bind:particle-gravity="r"></particles></body></xt>)~");
    Document& d = *l.doc;
    d.update(0);
    d.update(0.5f);
    CHECK(particles(d, "P").rate == doctest::Approx(50).epsilon(0.02));
    const PortId r = d.lookupPort("r");
    REQUIRE(r != kInvalidPort);
    REQUIRE(d.setPort(r, Value::ofFloat(42)) == Status::kOk);
    d.update(0.6f);
    CHECK(particles(d, "P").gravity == doctest::Approx(42));
}
