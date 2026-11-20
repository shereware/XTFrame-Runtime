#include "xt/perf_record.h"

#include <cstring>

#include "xt/charconv.h"

/// T-750: FrameRecord v1 and its CSV form. See xt/perf_record.h, Docs/design/57-measurement.md §57.2,
/// §57.5.

namespace xt {

namespace {

/// snprintf-like accumulation: copies what fits (leaving room for the NUL), counts everything.
struct Appender {
    char* buffer;
    size_t capacity;
    size_t length;

    void put(const char* text, size_t n) {
        if (buffer != nullptr && capacity > 0 && length + 1 < capacity) {
            const size_t room = capacity - 1 - length;
            std::memcpy(buffer + length, text, n < room ? n : room);
        }
        length += n;
    }
    void put(const char* text) { put(text, std::strlen(text)); }
    void number(double value, int decimals) {
        char tmp[48];
        const size_t n = formatFixed(tmp, sizeof(tmp), value, decimals);
        put(tmp, n);
    }
    size_t finish() {
        if (buffer != nullptr && capacity > 0) {
            buffer[length < capacity ? length : capacity - 1] = '\0';
        }
        return length;
    }
};

const char* kindName(uint32_t kind) {
    switch (kind) {
        case kDirtyKindStyle:
            return "style";
        case kDirtyKindLayout:
            return "layout";
        case kDirtyKindTransform:
            return "transform";
        default:
            return "paint";
    }
}

void putLower(Appender& out, const char* text) {
    for (const char* p = text; *p != '\0'; ++p) {
        const char c = (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p - 'A' + 'a') : *p;
        out.put(&c, 1);
    }
}

}  // namespace

FrameRecord makeFrameRecord(const FrameStats& s) {
    FrameRecord r;
    r.version = kFrameRecordVersion;
    r.frame = s.frameIndex;
    r.timeSeconds = s.timeSeconds;
    r.backend = s.backend;
    r.frameMarked = s.frameMarked;

    // The eight exclusive phases (57 §57.2 item 1). inputMs runs inside updateMs.
    r.inputMs = s.inputMs;
    r.inputCpuMs = s.inputCpuMs;
    r.updateMs = s.updateMs - s.inputMs;
    r.updateCpuMs = s.updateCpuMs - s.inputCpuMs;
    r.flowMs = s.flowMs;
    r.flowCpuMs = s.flowCpuMs;
    r.paintMs = s.paintMs;
    r.paintCpuMs = s.paintCpuMs;
    r.damageMs = s.damageMs;
    r.damageCpuMs = s.damageCpuMs;
    r.buildMs = s.backend == static_cast<uint32_t>(PerfBackend::Sw) ? s.renderMs : s.gpuBuildMs;
    r.buildCpuMs = s.buildCpuMs;
    r.submitMs = s.submitMs;
    r.submitCpuMs = s.submitCpuMs;
    r.waitMs = s.waitMs;
    r.waitCpuMs = s.waitCpuMs;
    if (s.frameMarked != 0) {
        r.frameWallMs = s.frameWallMs;
        r.frameCpuMs = s.frameCpuMs;
        r.unaccountedMs = s.frameWallMs - (r.inputMs + r.updateMs + r.flowMs + r.paintMs + r.damageMs +
                                           r.buildMs + r.submitMs + r.waitMs);
    }

    r.updateStyleMs = s.styleMs;
    r.updateLayoutMs = s.layoutMs;
    r.updateAnimateMs = s.animateMs;
    r.updateTransformMs = s.transformMs;
    r.submitPlanMs = s.submitPlanMs;
    r.submitUploadMs = s.submitUploadMs;
    r.submitEncodeMs = s.submitEncodeMs;
    r.submitQueueMs = s.submitQueueMs;

    r.styledElements = s.styledElements;
    r.laidOutElements = s.laidOutElements;
    r.layoutRoots = s.layoutRoots;
    r.layoutFullPasses = s.layoutFullPasses;
    r.activeAnimations = s.activeAnimations;
    r.activeTransitions = s.activeTransitions;
    r.animEvaluated = s.animEvaluated;
    r.exprEvaluated = s.exprEvaluated;
    r.setStyleCalls = s.setStyleCalls;
    r.flowSignals = s.flowSignals;
    r.flowNodeEvals = s.flowNodeEvals;
    r.flowSinkWrites = s.flowSinkWrites;
    r.commands = s.commands;
    r.layers = s.layers;
    r.effectLayers = s.effectLayers;
    r.projections = s.projections;
    r.glyphs = s.glyphs;
    r.pathPoints = s.pathPoints;
    r.imageDraws = s.imageDraws;
    r.culledOffscreen = s.culledOffscreen;
    r.borderDashHits = s.borderDashHits;
    r.borderDashMisses = s.borderDashMisses;
    r.bands = s.bands;
    r.bandRedraws = s.bandRedraws;
    r.passes = s.passes;
    r.drawCalls = s.drawCalls;
    r.pipelineBinds = s.pipelineBinds;
    r.bindGroups = s.bindGroups;
    r.uploadBytes = s.uploadBytes;
    r.transientCreates = s.transientCreates;
    r.frameReused = s.frameReused;
    r.effectCacheHits = s.effectCacheHits;
    r.effectCacheMisses = s.effectCacheMisses;
    r.coverageHits = s.coverageHits;
    r.coverageMisses = s.coverageMisses;

    r.idlePaintMs = s.idlePaintMs;
    r.paintSkipped = s.paintSkipped;
    r.staticSkipMismatch = s.staticSkipMismatch;
    r.partialLayoutMismatch = s.partialLayoutMismatch;

    r.arenaNodes = s.arenaNodes;
    r.commandBytes = s.commandBytes;
    r.effectCacheBytes = s.effectCacheBytes;
    r.transientPoolBytes = s.transientPoolBytes;

    std::memcpy(r.causes, s.causes, sizeof(r.causes));

    // T-753 (57 §57.3 item 4): the redundant-work counts (detail level >= 1).
    r.redundantStyled = s.redundantStyled;
    r.redundantLaidOut = s.redundantLaidOut;
    r.redundantCommands = s.redundantCommands;
    r.repaintedUnchangedNotCacheable = s.repaintedUnchangedNotCacheable;
    r.repaintedUnchangedKeyChanged = s.repaintedUnchangedKeyChanged;
    r.repaintedUnchangedBudget = s.repaintedUnchangedBudget;
    r.repaintedUnchangedFirstSight = s.repaintedUnchangedFirstSight;
    return r;
}

size_t frameRecordCsvHeader(char* buffer, size_t capacity) {
    Appender out{buffer, capacity, 0};
    bool first = true;
    auto column = [&](const char* name) {
        if (!first) {
            out.put(",", 1);
        }
        first = false;
        out.put(name);
    };
#define XT_FRAME_RECORD_HEAD(member, csv, kind) column(#csv);
    XT_FRAME_RECORD_FIELDS(XT_FRAME_RECORD_HEAD)
    for (uint32_t c = 0; c < kDirtyCauseCount; ++c) {
        for (uint32_t k = 0; k < kDirtyKindCount; ++k) {
            out.put(",cause_", 7);
            putLower(out, dirtyCauseName(static_cast<DirtyCause>(c)));
            out.put("_", 1);
            out.put(kindName(k));
        }
    }
    XT_FRAME_RECORD_TAIL_FIELDS(XT_FRAME_RECORD_HEAD)  // T-753
#undef XT_FRAME_RECORD_HEAD
    return out.finish();
}

size_t frameRecordCsvRow(const FrameRecord& r, char* buffer, size_t capacity) {
    Appender out{buffer, capacity, 0};
    bool first = true;
    auto sep = [&]() {
        if (!first) {
            out.put(",", 1);
        }
        first = false;
    };
#define XT_FRAME_RECORD_ROW_F(member) \
    sep();                            \
    out.number(static_cast<double>(r.member), 4);
#define XT_FRAME_RECORD_ROW_U(member) \
    sep();                            \
    out.number(static_cast<double>(r.member), 0);
#define XT_FRAME_RECORD_ROW(member, csv, kind) XT_FRAME_RECORD_ROW_##kind(member)
    XT_FRAME_RECORD_FIELDS(XT_FRAME_RECORD_ROW)
    for (uint32_t c = 0; c < kDirtyCauseCount; ++c) {
        for (uint32_t k = 0; k < kDirtyKindCount; ++k) {
            out.put(",", 1);
            out.number(static_cast<double>(r.causes[c][k]), 0);
        }
    }
    XT_FRAME_RECORD_TAIL_FIELDS(XT_FRAME_RECORD_ROW)  // T-753
#undef XT_FRAME_RECORD_ROW
#undef XT_FRAME_RECORD_ROW_U
#undef XT_FRAME_RECORD_ROW_F
    return out.finish();
}

size_t csvAppendText(char* buffer, size_t capacity, size_t used, const char* text) {
    Appender out{buffer, capacity, used};
    out.put(",", 1);
    out.put(text != nullptr ? text : "");
    return out.finish();
}

size_t csvAppendNumber(char* buffer, size_t capacity, size_t used, double value, int decimals) {
    Appender out{buffer, capacity, used};
    out.put(",", 1);
    out.number(value, decimals);
    return out.finish();
}

}  // namespace xt
