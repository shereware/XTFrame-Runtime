#include <cstddef>
#include <cstring>
#include <string>

#include "capi_internal.h"
#include "xt/perf_record.h"

/// T-750 (Docs/design/57-measurement.md §57.2, §57.5): xt_frame_record -- xt::FrameRecord, field for
/// field -- and its CSV form through the runtime's one column table. T-753: the frame marks and the
/// thread CPU switch. T-749: the perf detail level.

namespace {

// The C struct is xt::FrameRecord with C names: same size, same offsets.
static_assert(sizeof(xt_frame_record) == sizeof(xt::FrameRecord),
              "xt_frame_record must mirror xt::FrameRecord");
static_assert(offsetof(xt_frame_record, struct_size) == offsetof(xt::FrameRecord, structSize));
#define XT_CAPI_RECORD_OFFSET(member, csv, kind) \
    static_assert(offsetof(xt_frame_record, csv) == offsetof(xt::FrameRecord, member), #csv);
XT_FRAME_RECORD_FIELDS(XT_CAPI_RECORD_OFFSET)
static_assert(offsetof(xt_frame_record, causes) == offsetof(xt::FrameRecord, causes));
static_assert(sizeof(((xt_frame_record*)nullptr)->causes) == sizeof(((xt::FrameRecord*)nullptr)->causes));
XT_FRAME_RECORD_TAIL_FIELDS(XT_CAPI_RECORD_OFFSET)  // T-753
#undef XT_CAPI_RECORD_OFFSET

/// The part of a caller's struct its struct_size covers.
size_t coveredBytes(uint32_t structSize) {
    return structSize < sizeof(xt::FrameRecord) ? structSize : sizeof(xt::FrameRecord);
}

}  // namespace

extern "C" {

XT_CAPI xt_status xt_surface_frame_record(const xt_surface* s, xt_frame_record* out) {
    XT_CAPI_GUARD("xt_surface_frame_record", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_frame_record");
        if (out == nullptr) {
            return xt::capi::fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_frame_record: out is NULL");
        }
        const uint32_t size = out->struct_size;
        if (size < offsetof(xt_frame_record, version) + sizeof(uint32_t)) {
            return xt::capi::fail(XT_STATUS_INVALID_ARGUMENT,
                                  "xt_surface_frame_record: struct_size too small");
        }
        const xt::FrameRecord record = xt::makeFrameRecord(s->ctx->perf().current());
        // Everything after struct_size the caller's struct covers; struct_size stays the caller's.
        const size_t n = coveredBytes(size);
        std::memcpy(reinterpret_cast<unsigned char*>(out) + sizeof(uint32_t),
                    reinterpret_cast<const unsigned char*>(&record) + sizeof(uint32_t), n - sizeof(uint32_t));
        return XT_STATUS_OK;
    });
}

XT_CAPI size_t xt_frame_record_csv_header(char* buf, size_t cap) {
    XT_CAPI_GUARD_OR("xt_frame_record_csv_header", size_t, size_t{0},
                     { return xt::frameRecordCsvHeader(buf, cap); });
}

XT_CAPI size_t xt_frame_record_csv_row(const xt_frame_record* r, char* buf, size_t cap) {
    XT_CAPI_GUARD_OR("xt_frame_record_csv_row", size_t, size_t{0}, {
        if (r == nullptr) {
            if (buf != nullptr && cap > 0) {
                buf[0] = '\0';
            }
            return size_t{0};
        }
        // Fields beyond the caller's struct_size are written as 0.
        xt::FrameRecord record;
        std::memset(static_cast<void*>(&record), 0, sizeof(record));  // GCC -Wclass-memaccess
        const size_t n = coveredBytes(r->struct_size);
        if (n > sizeof(uint32_t)) {
            std::memcpy(reinterpret_cast<unsigned char*>(&record) + sizeof(uint32_t),
                        reinterpret_cast<const unsigned char*>(r) + sizeof(uint32_t), n - sizeof(uint32_t));
        }
        record.structSize = sizeof(record);
        return xt::frameRecordCsvRow(record, buf, cap);
    });
}

XT_CAPI xt_status xt_surface_mark_frame_start(xt_surface* s) {
    XT_CAPI_GUARD("xt_surface_mark_frame_start", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_mark_frame_start");
        s->ctx->perf().markFrameStart();
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_mark_frame_end(xt_surface* s) {
    XT_CAPI_GUARD("xt_surface_mark_frame_end", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_mark_frame_end");
        s->ctx->perf().markFrameEnd();
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_set_perf_detail(xt_surface* s, int level) {
    XT_CAPI_GUARD("xt_surface_set_perf_detail", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_perf_detail");
        if (level < 0 || level > 2) {
            return xt::capi::fail(
                XT_STATUS_INVALID_ARGUMENT,
                "xt_surface_set_perf_detail: level must be 0, 1 or 2, got " + std::to_string(level));
        }
        s->ctx->perf().setDetail(level);  // T-749 (57 §57.3; T-753's levels)
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_set_thread_cpu_timing(xt_surface* s, int enabled) {
    XT_CAPI_GUARD("xt_surface_set_thread_cpu_timing", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_thread_cpu_timing");
        s->ctx->perf().setThreadCpuTiming(enabled != 0);
        return XT_STATUS_OK;
    });
}

}  // extern "C"
