// T-376 (Docs/design/23-host-integration.md §23.ab): the non-Windows side of a borderless App
// window -- nothing to do (no window style to widen, no DWM shadow to restore).
// T-393 (08.14): no platform file drop target either: SDL's drop events (App::handleDropEvent)
// are the fallback, ExternalDrop only.

#include <SDL.h>

#include <functional>
#include <span>
#include <string_view>

#include "xt/input.h"

namespace xt::shell {

void prepareBorderlessWindow() {}

void applyBorderlessShadow(SDL_Window* /*window*/) {}

void keepMaximizedInWorkArea(SDL_Window* /*window*/) {}

void initPlatformFileDrop() {}

void shutdownPlatformFileDrop() {}

void* registerFileDrop(
    SDL_Window* /*window*/,
    std::function<bool(InputEvent::Kind, Vec2, std::span<const std::string_view>)> /*drag*/) {
    return nullptr;
}

void revokeFileDrop(void* /*handle*/) {}

}  // namespace xt::shell
