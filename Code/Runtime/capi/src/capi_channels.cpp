#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "capi_internal.h"
#include "xt/input_channels.h"

/// T-873 (Docs/design/60-input-channels.md §60.2, §60.5): the C ABI of the input channels. Channel ids
/// handed to the host are process-wide (one name, one id, whatever the bundle or surface), so a host
/// can look them up once; each surface maps them onto its Context's own ids on first use.

namespace {

/// The process-wide channel names: id = index + 1. Guarded by a mutex because xt_input_channel may
/// be called from any thread (the surface calls stay on their bundle's thread, 50.1).
struct ChannelRegistry {
    std::mutex mutex;
    std::vector<std::string> names;
    std::unordered_map<std::string, uint32_t> ids;
};

ChannelRegistry& registry() {
    static ChannelRegistry r;
    return r;
}

/// The canonical name of process-wide id `id`, or "" for an unknown one.
std::string nameOf(uint32_t id) {
    ChannelRegistry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    return id == 0 || id > r.names.size() ? std::string() : r.names[id - 1];
}

/// `s`'s Context id for process-wide id `id`; kInvalidChannel for an unknown one.
xt::ChannelId localId(const xt_surface* s, uint32_t id) {
    if (id != 0 && id <= s->channelIds.size() && s->channelIds[id - 1] != xt::kInvalidChannel) {
        return s->channelIds[id - 1];
    }
    const std::string name = nameOf(id);
    if (name.empty()) {
        return xt::kInvalidChannel;
    }
    auto* surface = const_cast<xt_surface*>(s);  // The id cache only; no observable state changes.
    const xt::ChannelId local = surface->ctx->inputChannel(name);
    if (surface->channelIds.size() < id) {
        surface->channelIds.resize(id, xt::kInvalidChannel);
    }
    surface->channelIds[id - 1] = local;
    return local;
}

}  // namespace

XT_CAPI uint32_t xt_input_channel(const char* name_utf8) {
    XT_CAPI_GUARD_OR("xt_input_channel", uint32_t, 0u, {
        if (name_utf8 == nullptr) {
            xt::capi::setLastError("xt_input_channel: name is NULL");
            return 0u;
        }
        std::string canonical;
        uint8_t player = 0;
        std::string err;
        if (!xt::parseChannelRef(name_utf8, canonical, player, &err) || player != 0 ||
            std::string_view(name_utf8).find("player(") != std::string_view::npos) {
            xt::capi::setLastError("xt_input_channel: " +
                                   (err.empty() ? std::string("a channel name has no player prefix") : err));
            return 0u;
        }
        ChannelRegistry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (auto it = r.ids.find(canonical); it != r.ids.end()) {
            return it->second;
        }
        r.names.push_back(canonical);
        const auto id = static_cast<uint32_t>(r.names.size());
        r.ids.emplace(canonical, id);
        return id;
    });
}

XT_CAPI xt_status xt_surface_push_channel(xt_surface* s, uint32_t channel, float value, float strength,
                                          uint32_t flags, uint32_t player) {
    XT_CAPI_GUARD("xt_surface_push_channel", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_push_channel");
        if (player >= xt::kMaxPlayers || !std::isfinite(value)) {
            return xt::capi::fail(XT_STATUS_INVALID_ARGUMENT,
                                  "xt_surface_push_channel: player above 7 or a non-finite value");
        }
        const xt::ChannelId local = localId(s, channel);
        if (local == xt::kInvalidChannel) {
            return xt::capi::fail(XT_STATUS_INVALID_ARGUMENT,
                                  "xt_surface_push_channel: unknown channel id " + std::to_string(channel));
        }
        xt::InputEvent event;
        event.kind = xt::InputEvent::Channel;
        event.channel = local;
        event.channelValue = value;
        event.channelStrength = std::isfinite(strength) ? strength : 1.0f;
        event.channelFlags = static_cast<uint8_t>(flags & (XT_CHANNEL_RELATIVE | XT_CHANNEL_BUTTON));
        event.player = static_cast<uint8_t>(player);
        s->ctx->pushInput(event);
        ++s->debugInputCount;
        return XT_STATUS_OK;
    });
}

XT_CAPI float xt_surface_channel_value(const xt_surface* s, uint32_t channel, uint32_t player) {
    XT_CAPI_GUARD_OR("xt_surface_channel_value", float, 0.0f, {
        if (s == nullptr || player >= xt::kMaxPlayers) {
            return 0.0f;
        }
        const xt::ChannelId local = localId(s, channel);
        return local == xt::kInvalidChannel ? 0.0f
                                            : s->ctx->channelValue(local, static_cast<uint8_t>(player));
    });
}
