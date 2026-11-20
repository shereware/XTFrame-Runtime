#pragma once

#include <cstddef>
#include <cstdint>

#include "xt/dirty_cause.h"
#include "xt/perf.h"

/// T-750 (Docs/design/57-measurement.md §57.2, §57.5): FrameRecord v1 -- the one per-frame record the
/// runtime produces for every tool and host, and its CSV form.
///
/// The budget is closed: the eight top-level phases are exclusive, and
/// `unaccountedMs = frameWallMs - (input + update + flow + paint + damage + build + submit + wait)` for
/// a frame the host marked (PerfTracker::markFrameStart / markFrameEnd); unmarked frames carry
/// frameWallMs = 0 and unaccountedMs = 0. unaccountedMs is never clamped (it may be negative).
///
/// Field order is fixed; fields are only ever added at the end (after `causes`; T-753 added the first,
/// XT_FRAME_RECORD_TAIL_FIELDS -- the version stays 1). The C ABI's
/// xt_frame_record (xt/capi/xtframe.h) has the same layout, field for field.

namespace xt {

/// FrameRecord::version of this layout.
inline constexpr uint32_t kFrameRecordVersion = 1;

// clang-format off
/// The scalar fields of FrameRecord in declaration order, as X(member, csv_name, kind) with kind F
/// (float, written with 4 decimals) or U (uint32_t). csv_name is also the C ABI field name.
/// structSize is not a column; causes (after these) is written as cause_<cause>_<kind> columns.
#define XT_FRAME_RECORD_FIELDS(X)                                                                       \
    /* identity */                                                                                     \
    X(version, version, U) X(frame, frame, U) X(steady, steady, U) X(timeSeconds, time_s, F)           \
    X(backend, backend, U) X(frameMarked, frame_marked, U)                                             \
    /* the closed budget (57 §57.2): whole frame, remainder, eight exclusive phases (wall + CPU) */     \
    X(frameWallMs, frame_wall_ms, F) X(frameCpuMs, frame_cpu_ms, F) X(unaccountedMs, unaccounted_ms, F)\
    X(inputMs, input_ms, F) X(inputCpuMs, input_cpu_ms, F)                                             \
    X(updateMs, update_ms, F) X(updateCpuMs, update_cpu_ms, F)                                         \
    X(flowMs, flow_ms, F) X(flowCpuMs, flow_cpu_ms, F)                                                 \
    X(paintMs, paint_ms, F) X(paintCpuMs, paint_cpu_ms, F)                                             \
    X(damageMs, damage_ms, F) X(damageCpuMs, damage_cpu_ms, F)                                         \
    X(buildMs, build_ms, F) X(buildCpuMs, build_cpu_ms, F)                                             \
    X(submitMs, submit_ms, F) X(submitCpuMs, submit_cpu_ms, F)                                         \
    X(waitMs, wait_ms, F) X(waitCpuMs, wait_cpu_ms, F)                                                 \
    /* sub-phases (inside their phase, not part of the sum) */                                         \
    X(updateStyleMs, update_style_ms, F) X(updateLayoutMs, update_layout_ms, F)                        \
    X(updateAnimateMs, update_animate_ms, F) X(updateTransformMs, update_transform_ms, F)              \
    X(submitPlanMs, submit_plan_ms, F) X(submitUploadMs, submit_upload_ms, F)                          \
    X(submitEncodeMs, submit_encode_ms, F) X(submitQueueMs, submit_queue_ms, F)                        \
    /* counts (57 §57.4 item 1) */                                                                     \
    X(styledElements, styled_elements, U) X(laidOutElements, laid_out_elements, U)                     \
    X(layoutRoots, layout_roots, U) X(layoutFullPasses, layout_full_passes, U)                         \
    X(activeAnimations, active_animations, U) X(activeTransitions, active_transitions, U)              \
    X(animEvaluated, anim_evaluated, U) X(exprEvaluated, expr_evaluated, U)                            \
    X(setStyleCalls, set_style_calls, U) X(flowSignals, flow_signals, U)                               \
    X(flowNodeEvals, flow_node_evals, U) X(flowSinkWrites, flow_sink_writes, U)                        \
    X(commands, commands, U) X(layers, layers, U) X(effectLayers, effect_layers, U)                    \
    X(projections, projections, U) X(glyphs, glyphs, U) X(pathPoints, path_points, U)                  \
    X(imageDraws, image_draws, U) X(culledOffscreen, culled_offscreen, U)                              \
    X(borderDashHits, border_dash_hits, U) X(borderDashMisses, border_dash_misses, U)                  \
    X(bands, bands, U) X(bandRedraws, band_redraws, U)                                                 \
    X(passes, passes, U) X(drawCalls, draw_calls, U) X(pipelineBinds, pipeline_binds, U)               \
    X(bindGroups, bind_groups, U) X(uploadBytes, upload_bytes, U)                                      \
    X(transientCreates, transient_creates, U) X(frameReused, frame_reused, U)                          \
    X(effectCacheHits, effect_cache_hits, U) X(effectCacheMisses, effect_cache_misses, U)              \
    X(coverageHits, coverage_hits, U) X(coverageMisses, coverage_misses, U)                            \
    /* waste (57 §57.3 item 4) */                                                                      \
    X(idlePaintMs, idle_paint_ms, F) X(paintSkipped, paint_skipped, U)                                 \
    X(staticSkipMismatch, static_skip_mismatch, U) X(partialLayoutMismatch, partial_layout_mismatch, U)\
    /* memory (57 §57.5 item 1) */                                                                     \
    X(arenaNodes, arena_nodes, U) X(commandBytes, command_bytes, U)                                    \
    X(effectCacheBytes, effect_cache_bytes, U) X(transientPoolBytes, transient_pool_bytes, U)

/// T-753 (57 §57.3 item 4): the fields added after `causes` (57 §57.5 item 1: "only added at the end"; the
/// version stays 1 -- a reader finds columns by name, a C caller's struct_size says what it holds), in the
/// same X(member, csv_name, kind) form. Their CSV columns follow the cause columns. Counted at detail
/// level >= 1 (FrameStats::redundant*, repaintedUnchanged*).
#define XT_FRAME_RECORD_TAIL_FIELDS(X)                                                                 \
    /* redundantWork: re-styled with an unchanged style, laid out with an unchanged box, commands      \
       the damage diff matched unchanged */                                                            \
    X(redundantStyled, redundant_styled, U) X(redundantLaidOut, redundant_laid_out, U)                 \
    X(redundantCommands, redundant_commands, U)                                                        \
    /* repaintedUnchanged: effect segments / layers drawn in full with a command hash of the last      \
       build, by why the result was not reused */                                                      \
    X(repaintedUnchangedNotCacheable, repainted_unchanged_not_cacheable, U)                            \
    X(repaintedUnchangedKeyChanged, repainted_unchanged_key_changed, U)                                \
    X(repaintedUnchangedBudget, repainted_unchanged_budget, U)                                         \
    X(repaintedUnchangedFirstSight, repainted_unchanged_first_sight, U)
// clang-format on

/// One frame (57 §57.5 item 1). POD; see the field list above for names and units (ms = milliseconds;
/// *_cpu_ms = thread CPU, 0 unless PerfTracker::setThreadCpuTiming(true)).
struct FrameRecord {
    /// sizeof(FrameRecord) (the C ABI's struct_size).
    uint32_t structSize = sizeof(FrameRecord);
#define XT_FRAME_RECORD_DECL_F(member) float member = 0;
#define XT_FRAME_RECORD_DECL_U(member) uint32_t member = 0;
#define XT_FRAME_RECORD_DECL(member, csv, kind) XT_FRAME_RECORD_DECL_##kind(member)
    XT_FRAME_RECORD_FIELDS(XT_FRAME_RECORD_DECL)
    /// The cause table (57 §57.3 item 2): writes per DirtyCause (row) and kind (column: style,
    /// layout, transform, paint).
    uint32_t causes[kDirtyCauseCount][kDirtyKindCount] = {};
    // T-753: the fields after `causes`.
    XT_FRAME_RECORD_TAIL_FIELDS(XT_FRAME_RECORD_DECL)
#undef XT_FRAME_RECORD_DECL
#undef XT_FRAME_RECORD_DECL_U
#undef XT_FRAME_RECORD_DECL_F
};

/// Number of scalar fields of FrameRecord v1 (both field lists above), and of its CSV columns.
constexpr size_t frameRecordScalarCount() {
    size_t n = 0;
#define XT_FRAME_RECORD_COUNT(member, csv, kind) ++n;
    XT_FRAME_RECORD_FIELDS(XT_FRAME_RECORD_COUNT)
    XT_FRAME_RECORD_TAIL_FIELDS(XT_FRAME_RECORD_COUNT)
#undef XT_FRAME_RECORD_COUNT
    return n;
}
inline constexpr size_t kFrameRecordCsvColumns =
    frameRecordScalarCount() + static_cast<size_t>(kDirtyCauseCount) * kDirtyKindCount;

/// The record of `stats` (one PerfTracker frame): the phases made exclusive (update = updateMs -
/// inputMs; build = gpuBuildMs, or renderMs for the SW backend; submit = the backend's submission),
/// the budget closed for a marked frame. `steady` is 0; the caller sets it.
FrameRecord makeFrameRecord(const FrameStats& stats);

/// The CSV header (column names, comma separated, no line end) into `buffer`, NUL-terminated and
/// truncated to `capacity - 1` characters. Returns the length the whole text needs (excluding the
/// NUL), like snprintf; `buffer` may be null when `capacity` is 0.
size_t frameRecordCsvHeader(char* buffer, size_t capacity);
/// One CSV row of `record` (no line end), with the same contract as frameRecordCsvHeader. Numbers are
/// written in the C locale (formatFixed, 4 decimals for floats).
size_t frameRecordCsvRow(const FrameRecord& record, char* buffer, size_t capacity);

/// Helpers for a host's own `host.*` columns after the record (57 §57.5 item 3): append ",<text>"
/// (a column name) or ",<number>" to the text of length `used` already in `buffer`. Same contract:
/// NUL-terminated, truncated to `capacity - 1`; returns the length the whole text needs.
size_t csvAppendText(char* buffer, size_t capacity, size_t used, const char* text);
size_t csvAppendNumber(char* buffer, size_t capacity, size_t used, double value, int decimals);

}  // namespace xt
