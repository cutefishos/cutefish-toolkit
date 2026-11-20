// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/thread_pool.h"
#include "ui/event.h"

struct wl_keyboard;
struct xkb_compose_state;
struct xkb_compose_table;
struct xkb_keymap;
struct xkb_state;

namespace cutefish::base {
class EventLoop;
}

namespace cutefish::ui::wayland {

class ShellSurface;
struct KeyboardListeners;

// The seat's keyboard: the keymap and its state, compose sequences and key
// repeat, delivered to the window with keyboard focus.
class Keyboard
{
public:
    Keyboard(base::EventLoop& loop, wl_keyboard* keyboard);
    ~Keyboard();

    Keyboard(const Keyboard&) = delete;
    Keyboard& operator=(const Keyboard&) = delete;

    uint32_t modifiers() const { return m_modifiers; }
    std::function<void(uint32_t serial)> onKeyPressSerial;
    ShellSurface* focus() const { return m_focus; }
    // The compositor's keymap as it sent it; empty until it has.
    const std::string& keymapText() const { return m_keymapText; }
    void forget(ShellSurface* shell);

private:
    friend struct KeyboardListeners;

    // Keymaps and compose tables take milliseconds to compile, so a thread
    // compiles them; key and modifier events wait for the keymap.
    void compileKeymap(std::string text);
    void setKeymap(xkb_keymap* keymap);
    void handleModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    void handleKey(uint32_t key, uint32_t state);
    // Returns false while a compose sequence swallows the key.
    bool translate(uint32_t keycode, bool compose, KeyEvent* event);
    void deliver(const KeyEvent& event, bool pressed);
    void startRepeat(uint32_t keycode);
    void repeatKey();
    void stopRepeat();

    base::EventLoop& m_loop;
    wl_keyboard* m_keyboard;
    std::string m_keymapText;
    xkb_keymap* m_keymap = nullptr;
    xkb_state* m_state = nullptr;
    xkb_compose_table* m_composeTable = nullptr;
    xkb_compose_state* m_composeState = nullptr;
    ShellSurface* m_focus = nullptr;
    uint32_t m_modifiers = NoModifier;

    int32_t m_repeatRate = 25;
    int32_t m_repeatDelay = 600;
    uint32_t m_repeatKeycode = 0;
    int m_repeatTimer = 0;
    base::Job m_keymapJob;
    base::Job m_composeJob;
    struct Modifiers
    {
        uint32_t depressed;
        uint32_t latched;
        uint32_t locked;
        uint32_t group;
    };
    std::optional<Modifiers> m_pendingModifiers;
    std::vector<std::pair<uint32_t, uint32_t>> m_pendingKeys;
};

} // namespace cutefish::ui::wayland
