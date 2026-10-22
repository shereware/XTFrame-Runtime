#include "xt/particles.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>

#include "xt/asset_resolver.h"
#include "xt/document.h"
#include "xt/node_arena.h"
#include "xt/num_guard.h"
#include "xt/property.h"

/// `<particles>` elements (Docs/design/03-scene-and-properties.md §3.12.5, Docs/tasks/T-202.md
/// items 1-2): the CPU simulation (stepParticles and friends, rules in particles.h) and the
/// Document side -- the NodeKind::Particles child created at load time (loadParticles), mirroring
/// the element's PropIds into it and advancing every emitter after layout (stepParticleNodes),
/// and the `play(<id>)` / `stop(<id>)` targets (emission on / off).

namespace xt {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// xorshift32; `state` must be non-zero.
uint32_t nextRandom(uint32_t& state) {
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

/// Uniform in [0, 1).
float unitRandom(uint32_t& state) {
    return static_cast<float>(nextRandom(state) >> 8) * (1.0f / 16777216.0f);
}

float uniform(uint32_t& state, float lo, float hi) { return lo + (hi - lo) * unitRandom(state); }

/// Mixes the emitter seed with the emission sequence number into a non-zero xorshift state.
uint32_t streamState(uint32_t seed, uint32_t sequence) {
    uint32_t h = seed + sequence * 0x9E3779B9u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h == 0 ? 0x1234567u : h;
}

Vec2 emitterPoint(ParticleShape shape, const Rect& box, uint32_t& rng) {
    const Vec2 center{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    switch (shape) {
        case ParticleShape::Rect:
            return Vec2{box.x + unitRandom(rng) * box.w, box.y + unitRandom(rng) * box.h};
        case ParticleShape::Circle: {
            // Uniform over the inscribed ellipse: sqrt for the radius, then a full-turn angle.
            const float r = std::sqrt(unitRandom(rng));
            const float a = unitRandom(rng) * 2.0f * kPi;
            return Vec2{center.x + std::cos(a) * r * box.w * 0.5f, center.y + std::sin(a) * r * box.h * 0.5f};
        }
        case ParticleShape::Edge: {
            const float perimeter = 2.0f * (box.w + box.h);
            if (perimeter <= 0.0f) {
                return center;
            }
            float d = unitRandom(rng) * perimeter;
            if (d < box.w) {
                return Vec2{box.x + d, box.y};  // top edge
            }
            d -= box.w;
            if (d < box.h) {
                return Vec2{box.x + box.w, box.y + d};  // right edge
            }
            d -= box.h;
            if (d < box.w) {
                return Vec2{box.x + box.w - d, box.y + box.h};  // bottom edge
            }
            d -= box.w;
            return Vec2{box.x, box.y + box.h - d};  // left edge
        }
        case ParticleShape::Point:
        default:
            return center;
    }
}

void emitOne(ParticleNodeData& data, const Rect& box) {
    uint32_t rng = streamState(data.seed, data.emitSeq++);
    Particle p;
    p.pos = emitterPoint(data.emitterShape, box, rng);
    const float angle =
        (data.direction + uniform(rng, -data.spread * 0.5f, data.spread * 0.5f)) * (kPi / 180.0f);
    const float speed = uniform(rng, data.speed[0], data.speed[1]);
    p.vel = Vec2{std::cos(angle) * speed, std::sin(angle) * speed};
    p.age = 0;
    p.life = std::max(0.0f, uniform(rng, data.lifetime[0], data.lifetime[1]));
    p.size0 = data.size[0];
    p.size1 = data.size[1];
    p.rot = uniform(rng, data.rotation[0], data.rotation[1]);
    p.rotVel = 0;
    p.colorSeed = unitRandom(rng);
    data.particles.push_back(p);
}

void stepOnce(ParticleNodeData& data, const Rect& box, float dt) {
    // Age and move; a particle whose lifetime is over is dropped, keeping the others' order.
    size_t kept = 0;
    const float damping = std::max(0.0f, 1.0f - data.drag * dt);
    for (size_t i = 0; i < data.particles.size(); ++i) {
        Particle p = data.particles[i];
        p.age += dt;
        if (p.age >= p.life) {
            continue;
        }
        p.vel.y += data.gravity * dt;
        p.vel.x *= damping;
        p.vel.y *= damping;
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.rot += p.rotVel * dt;
        data.particles[kept++] = p;
    }
    data.particles.resize(kept);
    if (data.emitting != 0 && data.rate > 0.0f) {
        data.emitCarry += data.rate * dt;
        // T-258a: at most the free room per step; the whole particles still owed are then dropped
        // in one go -- a carry that `-= 1` no longer changes (a rate of 1e30 or Inf) cannot spin
        // here. Below 2^24 every `-= 1` is exact, so keeping the fraction is what the old
        // one-by-one loop left behind; above that (or non-finite) there is no fraction to keep.
        const size_t room =
            data.particles.size() < data.maxParticles ? data.maxParticles - data.particles.size() : 0;
        for (size_t emitted = 0; emitted < room && data.emitCarry >= 1.0f; ++emitted) {
            data.emitCarry -= 1.0f;
            emitOne(data, box);
        }
        if (data.emitCarry >= 1.0f) {
            data.emitCarry =
                data.emitCarry < 16777216.0f ? data.emitCarry - std::floor(data.emitCarry) : 0.0f;
        }
    } else {
        data.emitCarry = 0;
    }
}

}  // namespace

uint32_t particleSeed(std::string_view id) {
    uint32_t h = 2166136261u;
    for (char c : id) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h;
}

void burstParticles(ParticleNodeData& data, const Rect& box, uint32_t count) {
    for (uint32_t i = 0; i < count && data.particles.size() < data.maxParticles; ++i) {
        emitOne(data, box);
    }
}

void stepParticles(ParticleNodeData& data, const Rect& box, float dt) {
    if (!(dt > 0.0f)) {
        return;
    }
    data.accumulator = std::min(data.accumulator + dt, kParticleMaxCatchUp);
    while (data.accumulator >= kParticleStep) {
        data.accumulator -= kParticleStep;
        stepOnce(data, box, kParticleStep);
    }
}

float particleSize(const Particle& p) {
    const float t = p.life > 0.0f ? std::clamp(p.age / p.life, 0.0f, 1.0f) : 1.0f;
    return p.size0 + (p.size1 - p.size0) * t;
}

Color particleColor(const ParticleNodeData& data, const Particle& p) {
    const float t = p.life > 0.0f ? std::clamp(p.age / p.life, 0.0f, 1.0f) : 1.0f;
    const float s = t * 3.0f;
    const int i = std::min(2, static_cast<int>(s));
    const float f = s - static_cast<float>(i);
    const Color& a = data.color[i];
    const Color& b = data.color[i + 1];
    return Color{a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, a.a + (b.a - a.a) * f};
}

// --- Document side ---------------------------------------------------------------------------

namespace {

std::string_view attributeOf(const IrView& ir, const ElementData& data, std::string_view name,
                             bool& present) {
    for (const auto& entry : data.attributes) {
        if (ir.string(entry.first) == name) {
            present = true;
            return ir.string(entry.second);
        }
    }
    present = false;
    return {};
}

/// Copies the element's particle PropIds (110-128) into its child's fields.
void mirrorParams(const ElementData& element, ParticleNodeData& data) {
    const Value* base = element.style.base;
    data.rate = clampFinite(base[prop::ParticleRate].f, 0.0f, kParticleMaxRate, 0.0f);
    data.burst = std::max(0.0f, base[prop::ParticleBurst].f);
    data.lifetime[0] = base[prop::ParticleLifetimeMin].f;
    data.lifetime[1] = base[prop::ParticleLifetimeMax].f;
    data.speed[0] = base[prop::ParticleSpeedMin].f;
    data.speed[1] = base[prop::ParticleSpeedMax].f;
    data.direction = base[prop::ParticleDirection].f;
    data.spread = base[prop::ParticleSpread].f;
    data.gravity = base[prop::ParticleGravity].f;
    data.drag = std::max(0.0f, base[prop::ParticleDrag].f);
    data.size[0] = base[prop::ParticleSizeStart].f;
    data.size[1] = base[prop::ParticleSizeEnd].f;
    data.color[0] = base[prop::ParticleColor0].c;
    data.color[1] = base[prop::ParticleColor1].c;
    data.color[2] = base[prop::ParticleColor2].c;
    data.color[3] = base[prop::ParticleColor3].c;
    data.rotation[0] = base[prop::ParticleRotationMin].f;
    data.rotation[1] = base[prop::ParticleRotationMax].f;
    // The cap is a Float PropId; keep it a sane integer (memory is `max` particles).
    const float cap = clampFinite(base[prop::ParticleMax].f, 0.0f, 65536.0f, 0.0f);
    data.maxParticles = toUint32Sat(cap);
}

}  // namespace

void Document::loadParticles(NodeId element) {
    const ElementData& data = m_arena.element(element);
    if (data.particlesNode != kInvalidNode) {
        return;
    }
    ParticleNodeData x;
    x.element = element;
    bool present = false;
    const std::string_view texture = attributeOf(m_ir, data, "data-xt-texture", present);
    uint32_t asset = 0;
    if (present &&
        std::from_chars(texture.data(), texture.data() + texture.size(), asset).ec == std::errc{}) {
        x.texture = asset;
        if (m_assets != nullptr) {
            x.textureSubUv = m_assets->imageSubUv(asset, m_ir);  // T-199: a bundle atlas cell.
        }
    }
    const std::string_view shape = attributeOf(m_ir, data, "shape", present);
    x.emitterShape = shape == "rect"     ? ParticleShape::Rect
                     : shape == "circle" ? ParticleShape::Circle
                     : shape == "edge"   ? ParticleShape::Edge
                                         : ParticleShape::Point;
    x.blend = attributeOf(m_ir, data, "blend", present) == "additive" ? ParticleBlend::Additive
                                                                      : ParticleBlend::Normal;
    const std::string_view autoplay = attributeOf(m_ir, data, "autoplay", present);
    x.autoplay = present && autoplay != "false" ? 1 : 0;
    x.pendingAutoplay = x.autoplay;
    const std::string_view simulate = attributeOf(m_ir, data, "simulate-in-editor", present);
    x.simulateInEditor = present && simulate == "false" ? 0 : 1;
    const std::string_view id = attributeOf(m_ir, data, "id", present);
    x.seed = present ? particleSeed(id) : (element + 1u) * 0x9E3779B9u;
    NodeId child = m_arena.create(NodeKind::Particles);
    m_arena.particles(child) = std::move(x);
    m_arena.appendChild(element, child);
    m_arena.element(element).particlesNode = child;
    m_particleElements.push_back(element);
}

NodeId Document::particlesOf(NodeId node) const {
    if (!m_arena.alive(node)) {
        return kInvalidNode;
    }
    if (m_arena.kind(node) == NodeKind::Particles) {
        return node;
    }
    if (m_arena.kind(node) != NodeKind::Element) {
        return kInvalidNode;
    }
    return m_arena.element(node).particlesNode;
}

const ParticleNodeData* Document::particleData(NodeId node) const {
    const NodeId child = particlesOf(node);
    return child == kInvalidNode ? nullptr : &m_arena.particles(child);
}

Status Document::playParticles(NodeId node) {
    const NodeId child = particlesOf(node);
    if (child == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    ParticleNodeData& x = m_arena.particles(child);
    if (x.emitting == 0) {
        x.emitting = 1;
        x.burstPending = 1;  // The burst goes out on the next step, from the current box.
    }
    x.pendingAutoplay = 0;
    return Status::kOk;
}

Status Document::stopParticles(NodeId node) {
    const NodeId child = particlesOf(node);
    if (child == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    ParticleNodeData& x = m_arena.particles(child);
    x.emitting = 0;
    x.burstPending = 0;
    x.pendingAutoplay = 0;
    return Status::kOk;
}

void Document::stepParticleNodes(float clock) {
    const float dt = m_particleClockValid ? std::max(0.0f, clock - m_particleClock) : 0.0f;
    m_particleClock = clock;
    m_particleClockValid = true;
    // Elements that died (a recycled `<repeat>` / spawn instance) leave the list.
    m_particleElements.erase(std::remove_if(m_particleElements.begin(), m_particleElements.end(),
                                            [&](NodeId e) {
                                                return !m_arena.alive(e) ||
                                                       m_arena.kind(e) != NodeKind::Element ||
                                                       m_arena.element(e).particlesNode == kInvalidNode;
                                            }),
                             m_particleElements.end());
    for (NodeId element : m_particleElements) {
        ElementData& data = m_arena.element(element);
        ParticleNodeData& x = m_arena.particles(data.particlesNode);
        mirrorParams(data, x);
        if (x.pendingAutoplay != 0) {
            x.pendingAutoplay = 0;
            x.emitting = 1;
            x.burstPending = 1;
        }
        if (m_particleEditorMode && x.simulateInEditor == 0) {
            continue;  // The editor's canvas keeps this emitter still (T-202 item 6).
        }
        const Rect emitter{0, 0, std::max(0.0f, data.box.width), std::max(0.0f, data.box.height)};
        if (x.burstPending != 0) {
            x.burstPending = 0;
            burstParticles(x, emitter, std::min(toUint32Sat(x.burst), x.maxParticles));
        }
        stepParticles(x, emitter, dt);
    }
}

}  // namespace xt
