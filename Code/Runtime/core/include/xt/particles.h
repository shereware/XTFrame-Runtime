#pragma once

#include <cstdint>
#include <string_view>

#include "xt/node.h"
#include "xt/types.h"

/// Particle simulation of a `<particles>` element (Docs/design/03-scene-and-properties.md §3.12.5,
/// Docs/tasks/T-202.md item 2): the CPU emitter behind NodeKind::Particles. Document drives it
/// from update() after layout; the functions are public so tests (and a host that wants to
/// pre-warm an emitter) can step a ParticleNodeData directly.
///
/// Rules:
///   - Fixed step: `kParticleStep` (1/120 s) with an accumulator; `stepParticles(dt)` runs as many
///     whole steps as `dt` covers and keeps the remainder. At most `kParticleMaxCatchUp` seconds
///     are simulated per call (a stalled host is not replayed in full).
///   - One step: every live particle ages by the step and dies (removed in place, order kept)
///     once `age >= life`; survivors move with `vel.y += gravity * dt`,
///     `vel *= max(0, 1 - drag * dt)`, `pos += vel * dt`, `rot += rotVel * dt`. Then, while
///     `emitting`, `rate * dt` is added to the fractional carry and one particle is emitted per
///     whole unit -- at most `maxParticles` minus the live count per step; once that room is used
///     up, the rest of the carry is dropped (nothing is emitted past the cap, T-258a).
///   - Rate: the element's `particle-rate` is taken clamped to [0, kParticleMaxRate] (a
///     non-finite one is 0).
///   - Burst: `burstParticles` emits up to `count` particles at once (same cap).
///   - Emission: a new particle starts at an emitter point (below) with angle
///     `direction + uniform(-spread / 2, spread / 2)` degrees (-90 points up, y grows downward),
///     speed `uniform(speed)`, life `uniform(lifetime)`, rotation `uniform(rotation)`, sizes
///     `size[0]`/`size[1]` and age 0.
///   - Determinism: every particle's random values come from an xorshift32 stream whose state is
///     derived from `seed` and the emission sequence number, so the same document produces the
///     same particles on every run and backend (golden images).
///   - Emitter box: the element's border box in its own coordinates; `point` emits from its
///     center, `rect` uniformly inside it, `circle` inside the inscribed ellipse, `edge` along
///     its perimeter.
///   - Appearance at age t = age / life: size lerps `size0 -> size1`; color is piecewise linear
///     over the 4 stops `color[0..3]` at t = 0, 1/3, 2/3, 1.

namespace xt {

/// The fixed simulation step, seconds.
inline constexpr float kParticleStep = 1.0f / 120.0f;
/// The most simulated time one stepParticles() call covers, seconds.
inline constexpr float kParticleMaxCatchUp = 1.0f;
/// The highest emission rate a `<particles>` element runs at, particles per second (T-258a).
inline constexpr float kParticleMaxRate = 100000.0f;

/// FNV-1a 32-bit hash of an element id: the seed of its emitter's random stream.
uint32_t particleSeed(std::string_view id);

/// Advances `data` by `dt` seconds (>= 0) in fixed steps. `box` is the emitter rectangle new
/// particles start in (local coordinates).
void stepParticles(ParticleNodeData& data, const Rect& box, float dt);

/// Emits up to `count` particles at once (subject to `maxParticles`).
void burstParticles(ParticleNodeData& data, const Rect& box, uint32_t count);

/// The size (px) of `p` at its current age.
float particleSize(const Particle& p);

/// The straight (non-premultiplied) color of `p` at its current age.
Color particleColor(const ParticleNodeData& data, const Particle& p);

}  // namespace xt
