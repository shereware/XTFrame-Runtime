// XTFrame reference backend -- MIT License (see LICENSE). Copy and edit (Code/Backends/README.md).
// Host API: SDL 2.0.18 or newer (built against 2.30.9, Code/SDKs/versions.cmake).
#include "xt_platform_sdl2.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <cstring>

#if defined(__APPLE__)
#include <SDL_metal.h>
#endif

namespace xt::backend {

namespace {

PointerButton mapButton(Uint8 sdlButton) {
    switch (sdlButton) {
        case SDL_BUTTON_RIGHT:
            return PointerButton::Right;
        case SDL_BUTTON_MIDDLE:
            return PointerButton::Middle;
        default:
            return PointerButton::Left;
    }
}

uint8_t currentModifiers() { return PlatformSdl2::modifiers(static_cast<uint16_t>(SDL_GetModState())); }

/// Bytes of the UTF-8 sequence led by `c` (1 for a continuation or invalid byte).
size_t utf8SeqLen(unsigned char c) {
    if ((c & 0x80u) == 0x00u) return 1;
    if ((c & 0xE0u) == 0xC0u) return 2;
    if ((c & 0xF0u) == 0xE0u) return 3;
    if ((c & 0xF8u) == 0xF0u) return 4;
    return 1;
}

/// The byte offset of character `chars` in `text` (SDL_TEXTEDITING counts characters).
uint32_t utf8ByteOffset(std::string_view text, uint32_t chars) {
    size_t pos = 0;
    for (uint32_t i = 0; i < chars && pos < text.size(); ++i) {
        pos += utf8SeqLen(static_cast<unsigned char>(text[pos]));
    }
    return static_cast<uint32_t>(pos < text.size() ? pos : text.size());
}

/// SDL scancodes are USB HID usages: KeyA..Space and Delete..Up share the numbering, the rest of
/// KeyCode (T-143) is mapped one by one, as in the shell.
KeyCode mapScancode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_SPACE) return static_cast<KeyCode>(sc);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + (sc - SDL_SCANCODE_F1));
    }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::Numpad1) + (sc - SDL_SCANCODE_KP_1));
    }
    switch (sc) {
        case SDL_SCANCODE_DELETE:
            return KeyCode::Delete;
        case SDL_SCANCODE_HOME:
            return KeyCode::Home;
        case SDL_SCANCODE_END:
            return KeyCode::End;
        case SDL_SCANCODE_RIGHT:
            return KeyCode::Right;
        case SDL_SCANCODE_LEFT:
            return KeyCode::Left;
        case SDL_SCANCODE_DOWN:
            return KeyCode::Down;
        case SDL_SCANCODE_UP:
            return KeyCode::Up;
        case SDL_SCANCODE_PAGEUP:
            return KeyCode::PageUp;
        case SDL_SCANCODE_PAGEDOWN:
            return KeyCode::PageDown;
        case SDL_SCANCODE_INSERT:
            return KeyCode::Insert;
        case SDL_SCANCODE_LSHIFT:
            return KeyCode::ShiftLeft;
        case SDL_SCANCODE_RSHIFT:
            return KeyCode::ShiftRight;
        case SDL_SCANCODE_LCTRL:
            return KeyCode::ControlLeft;
        case SDL_SCANCODE_RCTRL:
            return KeyCode::ControlRight;
        case SDL_SCANCODE_LALT:
            return KeyCode::AltLeft;
        case SDL_SCANCODE_RALT:
            return KeyCode::AltRight;
        case SDL_SCANCODE_LGUI:
            return KeyCode::MetaLeft;
        case SDL_SCANCODE_RGUI:
            return KeyCode::MetaRight;
        case SDL_SCANCODE_CAPSLOCK:
            return KeyCode::CapsLock;
        case SDL_SCANCODE_MINUS:
            return KeyCode::Minus;
        case SDL_SCANCODE_EQUALS:
            return KeyCode::Equal;
        case SDL_SCANCODE_LEFTBRACKET:
            return KeyCode::BracketLeft;
        case SDL_SCANCODE_RIGHTBRACKET:
            return KeyCode::BracketRight;
        case SDL_SCANCODE_BACKSLASH:
        case SDL_SCANCODE_NONUSHASH:
            return KeyCode::Backslash;
        case SDL_SCANCODE_SEMICOLON:
            return KeyCode::Semicolon;
        case SDL_SCANCODE_APOSTROPHE:
            return KeyCode::Quote;
        case SDL_SCANCODE_GRAVE:
            return KeyCode::Backquote;
        case SDL_SCANCODE_COMMA:
            return KeyCode::Comma;
        case SDL_SCANCODE_PERIOD:
            return KeyCode::Period;
        case SDL_SCANCODE_SLASH:
            return KeyCode::Slash;
        case SDL_SCANCODE_KP_0:
            return KeyCode::Numpad0;
        case SDL_SCANCODE_KP_PLUS:
            return KeyCode::NumpadAdd;
        case SDL_SCANCODE_KP_MINUS:
            return KeyCode::NumpadSubtract;
        case SDL_SCANCODE_KP_MULTIPLY:
            return KeyCode::NumpadMultiply;
        case SDL_SCANCODE_KP_DIVIDE:
            return KeyCode::NumpadDivide;
        case SDL_SCANCODE_KP_PERIOD:
            return KeyCode::NumpadDecimal;
        case SDL_SCANCODE_KP_ENTER:
            return KeyCode::NumpadEnter;
        default:
            return KeyCode::Unknown;
    }
}

}  // namespace

PlatformSdl2::~PlatformSdl2() { destroy(); }

bool PlatformSdl2::create(const Sdl2Options& options) {
    destroy();
    SDL_SetMainReady();  // This file never relies on SDL2main.
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        m_error = std::string("SDL_Init failed: ") + SDL_GetError();
        return false;
    }
    m_sdlInit = true;
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;
    if (options.resizable) flags |= SDL_WINDOW_RESIZABLE;
    flags |= options.hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN;
    if (options.graphics == Sdl2Graphics::OpenGL) {
        flags |= SDL_WINDOW_OPENGL;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    }
#if defined(__APPLE__)
    if (options.graphics == Sdl2Graphics::Surface) flags |= SDL_WINDOW_METAL;
#endif
    m_window = SDL_CreateWindow(options.title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, options.width,
                                options.height, flags);
    if (m_window == nullptr) {
        m_error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        destroy();
        return false;
    }
    if (options.graphics == Sdl2Graphics::OpenGL) {
        m_glContext = SDL_GL_CreateContext(m_window);
        if (m_glContext == nullptr || SDL_GL_MakeCurrent(m_window, m_glContext) != 0) {
            m_error = std::string("SDL_GL_CreateContext failed: ") + SDL_GetError();
            destroy();
            return false;
        }
        SDL_GL_SetSwapInterval(options.vsync ? 1 : 0);
    }
#if defined(__APPLE__)
    if (options.graphics == Sdl2Graphics::Surface) m_metalView = SDL_Metal_CreateView(m_window);
#endif
    // Text input stays off until a Context asks for it (syncTextInput).
    SDL_StopTextInput();
    m_startCounter = SDL_GetPerformanceCounter();
    m_viewportSynced = false;
    return true;
}

void PlatformSdl2::destroy() {
#if defined(__APPLE__)
    if (m_metalView != nullptr) SDL_Metal_DestroyView(m_metalView);
#endif
    m_metalView = nullptr;
    if (m_glContext != nullptr) SDL_GL_DeleteContext(static_cast<SDL_GLContext>(m_glContext));
    m_glContext = nullptr;
    if (m_window != nullptr) SDL_DestroyWindow(m_window);
    m_window = nullptr;
    if (m_sdlInit) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    m_sdlInit = false;
    m_composing = m_textInput = false;
}

uint8_t PlatformSdl2::modifiers(uint16_t sdlMod) {
    uint8_t mods = 0;
    if ((sdlMod & KMOD_SHIFT) != 0) mods |= static_cast<uint8_t>(kShift);
    if ((sdlMod & KMOD_CTRL) != 0) mods |= static_cast<uint8_t>(kCtrl);
    if ((sdlMod & KMOD_ALT) != 0) mods |= static_cast<uint8_t>(kAlt);
    if ((sdlMod & KMOD_GUI) != 0) mods |= static_cast<uint8_t>(kSuper);
    return mods;
}

bool PlatformSdl2::translateEvent(const SDL_Event& event, InputEvent& out) {
    switch (event.type) {
        case SDL_MOUSEMOTION:
            out = InputEvent{};
            out.kind = InputEvent::PointerMove;
            out.pos = Vec2{static_cast<float>(event.motion.x), static_cast<float>(event.motion.y)};
            out.modifiers = currentModifiers();
            return true;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            out = InputEvent{};
            out.kind = event.type == SDL_MOUSEBUTTONDOWN ? InputEvent::PointerDown : InputEvent::PointerUp;
            out.pos = Vec2{static_cast<float>(event.button.x), static_cast<float>(event.button.y)};
            out.button = mapButton(event.button.button);
            out.clicks = event.button.clicks;
            out.modifiers = currentModifiers();
            return true;
        case SDL_MOUSEWHEEL: {
            out = InputEvent{};
            out.kind = InputEvent::Wheel;
            float x = event.wheel.preciseX;
            float y = event.wheel.preciseY;
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                x = -x;
                y = -y;
            }
            out.wheelDelta = Vec2{x, -y};  // positive y = scroll down (08 §8.1)
            out.modifiers = currentModifiers();
            return true;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            out = InputEvent{};
            out.kind = event.type == SDL_KEYDOWN ? InputEvent::KeyDown : InputEvent::KeyUp;
            out.key = mapScancode(event.key.keysym.scancode);
            out.modifiers = modifiers(event.key.keysym.mod);
            out.repeat = event.key.repeat;
            return true;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                out = InputEvent{};
                out.kind = InputEvent::PointerLeave;
                return true;
            }
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                out = InputEvent{};
                out.kind = InputEvent::PointerCancel;  // a press in progress is cancelled (08 §8.11)
                return true;
            }
            return false;
        default:
            return false;
    }
}

size_t PlatformSdl2::translateTextInput(const SDL_Event& event, InputEvent* out, size_t maxOut) {
    if (event.type != SDL_TEXTINPUT || maxOut == 0) return 0;
    const char* text = event.text.text;
    const size_t len = std::strlen(text);
    size_t produced = 0;
    size_t pos = 0;
    while (pos < len && produced < maxOut) {
        size_t chunk = 0;
        while (pos + chunk < len) {
            size_t seq = utf8SeqLen(static_cast<unsigned char>(text[pos + chunk]));
            if (pos + chunk + seq > len) seq = 1;
            if (chunk + seq > 7) break;
            chunk += seq;
        }
        if (chunk == 0) chunk = 1;
        InputEvent& ev = out[produced++];
        ev = InputEvent{};
        ev.kind = InputEvent::TextInput;
        std::memcpy(ev.utf8, text + pos, chunk);
        ev.utf8[chunk] = '\0';
        ev.utf8Len = static_cast<uint8_t>(chunk);
        pos += chunk;
    }
    return produced;
}

void PlatformSdl2::syncViewport(Context& context) {
    int w = 0, h = 0;
    logicalSize(w, h);
    context.setViewport(Rect{0, 0, static_cast<float>(w), static_cast<float>(h)}, devicePixelRatio());
    m_viewportSynced = true;
}

void PlatformSdl2::syncTextInput(Context& context) {
    // 08 §8.8: platform text input follows wantsTextInput(); the candidate window sits at the caret.
    const bool wants = context.wantsTextInput();
    if (m_textInput && !wants) {
        if (m_composing) {
            InputEvent end{};
            end.kind = InputEvent::CompositionEnd;
            context.pushInput(end);
            m_composing = false;
        }
        SDL_StopTextInput();
        m_textInput = false;
    } else if (!m_textInput && wants) {
        SDL_StartTextInput();
        m_textInput = true;
    }
    if (wants) {
        const Rect caret = context.textInputRect();
        SDL_Rect rect{static_cast<int>(caret.x), static_cast<int>(caret.y), static_cast<int>(caret.w),
                      static_cast<int>(caret.h)};
        SDL_SetTextInputRect(&rect);
    }
}

bool PlatformSdl2::pumpEvents(Context& context) {
    if (m_window == nullptr) return false;
    if (!m_viewportSynced) syncViewport(context);
    bool open = true;
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
        if (event.type == SDL_QUIT) {
            open = false;
            continue;
        }
        if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                open = false;
                continue;
            }
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                event.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                m_resized = true;
                syncViewport(context);
            }
            if (event.window.event == SDL_WINDOWEVENT_LEAVE && SDL_GetMouseState(nullptr, nullptr) != 0) {
                context.cancelPointer();  // the pointer left with a button held (08 §8.11)
            }
        }
        if (event.type == SDL_TEXTINPUT) {
            InputEvent chunks[8];
            const size_t n = translateTextInput(event, chunks, 8);
            for (size_t i = 0; i < n; ++i) context.pushInput(chunks[i]);
            continue;
        }
        if (event.type == SDL_TEXTEDITING) {
            // 08 §8.8: CompositionStart / Update / End from TEXTEDITING's presence and emptiness.
            const std::string_view text(event.edit.text);
            if (!text.empty() && !m_composing) {
                InputEvent start{};
                start.kind = InputEvent::CompositionStart;
                context.pushInput(start);
                m_composing = true;
            }
            if (!text.empty()) {
                const uint32_t start = event.edit.start < 0 ? 0u : static_cast<uint32_t>(event.edit.start);
                const uint32_t length = event.edit.length < 0 ? 0u : static_cast<uint32_t>(event.edit.length);
                const uint32_t cursor = utf8ByteOffset(text, start);
                InputEvent update{};
                update.kind = InputEvent::CompositionUpdate;
                update.composition = text;
                update.compositionCursor = cursor;
                update.compositionSelLen = utf8ByteOffset(text, start + length) - cursor;
                context.pushInput(update);
            } else if (m_composing) {
                InputEvent end{};
                end.kind = InputEvent::CompositionEnd;
                context.pushInput(end);
                m_composing = false;
            }
            continue;
        }
        InputEvent input;
        if (translateEvent(event, input)) context.pushInput(input);
    }
    syncTextInput(context);
    return open;
}

bool PlatformSdl2::takeResized() {
    const bool r = m_resized;
    m_resized = false;
    return r;
}

void PlatformSdl2::logicalSize(int& w, int& h) const {
    w = h = 0;
    if (m_window != nullptr) SDL_GetWindowSize(m_window, &w, &h);
}

void PlatformSdl2::pixelSize(int& w, int& h) const {
    w = h = 0;
    if (m_window == nullptr) return;
    if (m_glContext != nullptr) {
        SDL_GL_GetDrawableSize(m_window, &w, &h);
    } else {
        SDL_GetWindowSizeInPixels(m_window, &w, &h);
    }
}

float PlatformSdl2::devicePixelRatio() const {
    int lw = 0, lh = 0, pw = 0, ph = 0;
    logicalSize(lw, lh);
    pixelSize(pw, ph);
    return lw > 0 && pw > 0 ? static_cast<float>(pw) / static_cast<float>(lw) : 1.0f;
}

double PlatformSdl2::seconds() const {
    return static_cast<double>(SDL_GetPerformanceCounter() - m_startCounter) /
           static_cast<double>(SDL_GetPerformanceFrequency());
}

std::string PlatformSdl2::clipboardText() const {
    std::string out;
    if (SDL_HasClipboardText() == SDL_TRUE) {
        char* text = SDL_GetClipboardText();
        if (text != nullptr) {
            out = text;
            SDL_free(text);
        }
    }
    return out;
}

void PlatformSdl2::setClipboardText(std::string_view utf8) {
    SDL_SetClipboardText(std::string(utf8).c_str());
}

Sdl2NativeWindow PlatformSdl2::nativeWindow() const {
    Sdl2NativeWindow out;
    if (m_window == nullptr) return out;
#if defined(__APPLE__)
    if (m_metalView != nullptr) {
        out.kind = Sdl2NativeWindow::Kind::Metal;
        out.window = SDL_Metal_GetLayer(m_metalView);
        return out;
    }
#endif
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(m_window, &info) != SDL_TRUE) return out;
    switch (info.subsystem) {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
        case SDL_SYSWM_WINDOWS:
            out.kind = Sdl2NativeWindow::Kind::Win32;
            out.display = info.info.win.hinstance;
            out.window = info.info.win.window;
            break;
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
        case SDL_SYSWM_X11:
            out.kind = Sdl2NativeWindow::Kind::X11;
            out.display = info.info.x11.display;
            out.x11Window = static_cast<uint64_t>(info.info.x11.window);
            break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            out.kind = Sdl2NativeWindow::Kind::Wayland;
            out.display = info.info.wl.display;
            out.window = info.info.wl.surface;
            break;
#endif
        default:
            break;
    }
    return out;
}

void* PlatformSdl2::glGetProcAddress(const char* name) {
    return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
}

void PlatformSdl2::glMakeCurrent() {
    if (m_glContext != nullptr) SDL_GL_MakeCurrent(m_window, static_cast<SDL_GLContext>(m_glContext));
}

void PlatformSdl2::glSwap() {
    if (m_glContext != nullptr) SDL_GL_SwapWindow(m_window);
}

bool PlatformSdl2::blitRgba(const uint8_t* rgba, int w, int h, int stride) {
    if (m_window == nullptr || rgba == nullptr || w <= 0 || h <= 0) return false;
    // The window framebuffer (SDL's video subsystem, so it works in builds without SDL_RENDER and on
    // the offscreen driver); SDL converts the format and scales to the window.
    SDL_Surface* target = SDL_GetWindowSurface(m_window);
    SDL_Surface* source = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint8_t*>(rgba), w, h, 32, stride,
                                                             SDL_PIXELFORMAT_RGBA32);
    bool ok = target != nullptr && source != nullptr;
    if (ok) {
        SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
        ok = (source->w == target->w && source->h == target->h
                  ? SDL_BlitSurface(source, nullptr, target, nullptr)
                  : SDL_BlitScaled(source, nullptr, target, nullptr)) == 0 &&
             SDL_UpdateWindowSurface(m_window) == 0;
    }
    if (!ok) m_error = std::string("window surface blit failed: ") + SDL_GetError();
    if (source != nullptr) SDL_FreeSurface(source);
    return ok;
}

}  // namespace xt::backend
