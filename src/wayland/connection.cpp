// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "wayland/connection.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/epoll.h>
#include <utility>
#include <wayland-client.h>

#include <cursor-shape-v1-client-protocol.h>
#include <cutefish-main-menu-v1-client-protocol.h>
#include <cutefish-shell-v1-client-protocol.h>
#include <cutefish-status-item-v1-client-protocol.h>
#include <cutefish-surface-blur-v1-client-protocol.h>
#include <cutefish-window-hints-v1-client-protocol.h>
#include <cutefish-window-placement-v1-client-protocol.h>
#include <fractional-scale-v1-client-protocol.h>
#include <pointer-gestures-unstable-v1-client-protocol.h>
#include <text-input-unstable-v3-client-protocol.h>
#include <presentation-time-client-protocol.h>
#include <primary-selection-unstable-v1-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <xdg-activation-v1-client-protocol.h>
#include <xdg-dialog-v1-client-protocol.h>
#include <xdg-foreign-unstable-v2-client-protocol.h>
#include <xdg-output-unstable-v1-client-protocol.h>
#include <xdg-shell-client-protocol.h>
#include <xdg-toplevel-drag-v1-client-protocol.h>

#include "base/event_loop.h"
#include "wayland/data_device.h"
#include "wayland/keyboard.h"
#include "wayland/main_menu.h"
#include "wayland/primary_selection.h"
#include "wayland/text_input.h"
#include "wayland/shell_surface.h"

namespace cutefish::ui::wayland {

static std::vector<Connection*> s_connections;

static ShellSurface* shellSurfaceFor(wl_surface* surface)
{
    // Null when the surface was destroyed while the event was in flight.
    return surface ? static_cast<ShellSurface*>(wl_surface_get_user_data(surface)) : nullptr;
}

template<typename T>
static T* bind(wl_registry* registry, uint32_t name, const wl_interface* interface,
               uint32_t offered, uint32_t wanted)
{
    return static_cast<T*>(wl_registry_bind(registry, name, interface, std::min(offered, wanted)));
}

struct ConnectionListeners
{
    static void global(void* data, wl_registry* registry, uint32_t name, const char* interface,
                       uint32_t version)
    {
        auto* self = static_cast<Connection*>(data);
        if (strcmp(interface, wl_compositor_interface.name) == 0) {
            self->m_compositor = bind<wl_compositor>(registry, name, &wl_compositor_interface,
                                                     version, 4);
        } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
            self->m_subcompositor = bind<wl_subcompositor>(registry, name, &wl_subcompositor_interface,
                                                           version, 1);
        } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
            self->m_wmBase = bind<xdg_wm_base>(registry, name, &xdg_wm_base_interface, version, 5);
            xdg_wm_base_add_listener(self->m_wmBase, &wmBase, self);
        } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
            self->m_viewporter = bind<wp_viewporter>(registry, name, &wp_viewporter_interface,
                                                     version, 1);
        } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
            self->m_fractionalScaleManager = bind<wp_fractional_scale_manager_v1>(
                registry, name, &wp_fractional_scale_manager_v1_interface, version, 1);
        } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
            self->m_presentation = bind<wp_presentation>(registry, name, &wp_presentation_interface,
                                                         version, 1);
        } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
            self->m_cursorShapeManager = bind<wp_cursor_shape_manager_v1>(
                registry, name, &wp_cursor_shape_manager_v1_interface, version, 1);
        } else if (strcmp(interface, zwp_pointer_gestures_v1_interface.name) == 0
                   && version >= ZWP_POINTER_GESTURES_V1_GET_HOLD_GESTURE_SINCE_VERSION) {
            self->m_pointerGestures = bind<zwp_pointer_gestures_v1>(
                registry, name, &zwp_pointer_gestures_v1_interface, version, 3);
        } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
            self->m_dataDeviceManager = bind<wl_data_device_manager>(
                registry, name, &wl_data_device_manager_interface, version, 3);
        } else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
            self->m_primarySelectionManager = bind<zwp_primary_selection_device_manager_v1>(
                registry, name, &zwp_primary_selection_device_manager_v1_interface, version, 1);
        } else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
            self->m_textInputManager = bind<zwp_text_input_manager_v3>(
                registry, name, &zwp_text_input_manager_v3_interface, version, 1);
        } else if (strcmp(interface, zxdg_exporter_v2_interface.name) == 0) {
            self->m_exporter = bind<zxdg_exporter_v2>(registry, name, &zxdg_exporter_v2_interface, version, 1);
        } else if (strcmp(interface, zxdg_importer_v2_interface.name) == 0) {
            self->m_importer = bind<zxdg_importer_v2>(registry, name, &zxdg_importer_v2_interface, version, 1);
        } else if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
            self->m_activation = bind<xdg_activation_v1>(registry, name, &xdg_activation_v1_interface, version, 1);
        } else if (strcmp(interface, xdg_wm_dialog_v1_interface.name) == 0) {
            self->m_dialogManager = bind<xdg_wm_dialog_v1>(registry, name,
                                                          &xdg_wm_dialog_v1_interface, version, 1);
        } else if (strcmp(interface, cutefish_surface_blur_v1_interface.name) == 0) {
            self->m_surfaceBlur = bind<cutefish_surface_blur_v1>(
                registry, name, &cutefish_surface_blur_v1_interface, version, 1);
        } else if (strcmp(interface, cutefish_window_hints_v1_interface.name) == 0) {
            self->m_windowHints = bind<cutefish_window_hints_v1>(
                registry, name, &cutefish_window_hints_v1_interface, version, 1);
            cutefish_window_hints_v1_add_listener(self->m_windowHints, &windowHints, self);
        } else if (strcmp(interface, cutefish_window_placement_v1_interface.name) == 0) {
            self->m_windowPlacement = bind<cutefish_window_placement_v1>(
                registry, name, &cutefish_window_placement_v1_interface, version, 1);
            cutefish_window_placement_v1_add_listener(self->m_windowPlacement, &windowPlacement, self);
        } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
            self->m_outputManager = bind<zxdg_output_manager_v1>(
                registry, name, &zxdg_output_manager_v1_interface, version, 3);
            for (Connection::Output& output : self->m_outputs)
                self->watchOutput(output);
        } else if (strcmp(interface, wl_output_interface.name) == 0) {
            auto* output = bind<wl_output>(registry, name, &wl_output_interface, version, 3);
            wl_output_add_listener(output, &outputListener, self);
            self->m_outputs.push_back({name, output, nullptr, {}});
            self->watchOutput(self->m_outputs.back());
        } else if (strcmp(interface, cutefish_status_item_manager_v1_interface.name) == 0) {
            self->m_statusItems = bind<cutefish_status_item_manager_v1>(
                registry, name, &cutefish_status_item_manager_v1_interface, version, 1);
        } else if (strcmp(interface, cutefish_main_menu_manager_v1_interface.name) == 0) {
            self->m_mainMenus = bind<cutefish_main_menu_manager_v1>(
                registry, name, &cutefish_main_menu_manager_v1_interface, version, 1);
        } else if (strcmp(interface, cutefish_shell_v1_interface.name) == 0) {
            self->m_shell = bind<cutefish_shell_v1>(registry, name, &cutefish_shell_v1_interface, version, 1);
        } else if (strcmp(interface, wl_shm_interface.name) == 0) {
            self->m_shm = bind<wl_shm>(registry, name, &wl_shm_interface, version, 1);
        } else if (strcmp(interface, xdg_toplevel_drag_manager_v1_interface.name) == 0) {
            self->m_toplevelDragManager = bind<xdg_toplevel_drag_manager_v1>(
                registry, name, &xdg_toplevel_drag_manager_v1_interface, version, 1);
        } else if (strcmp(interface, wl_seat_interface.name) == 0 && !self->m_seat) {
            self->m_seat = bind<wl_seat>(registry, name, &wl_seat_interface, version, 8);
            wl_seat_add_listener(self->m_seat, &seat, self);
        }
    }

    static void globalRemove(void* data, wl_registry*, uint32_t name)
    {
        auto* self = static_cast<Connection*>(data);
        const size_t before = self->m_outputs.size();
        std::erase_if(self->m_outputs, [name](const Connection::Output& output) {
            if (output.name != name)
                return false;
            Connection::releaseOutput(output);
            return true;
        });
        if (self->m_outputs.size() != before && self->onOutputsChanged)
            self->onOutputsChanged();
    }

    static void outputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
                               const char*, int32_t)
    {
    }

    static void outputMode(void* data, wl_output* wlOutput, uint32_t flags, int32_t width, int32_t height, int32_t)
    {
        auto* self = static_cast<Connection*>(data);
        if (!(flags & WL_OUTPUT_MODE_CURRENT))
            return;
        for (Connection::Output& output : self->m_outputs) {
            if (output.output == wlOutput)
                output.pixelSize = {double(width), double(height)};
        }
    }

    // wl_output.done closes every update, xdg_output's included from its
    // version 3 on.
    static void outputDone(void* data, wl_output*)
    {
        auto* self = static_cast<Connection*>(data);
        if (self->onOutputsChanged)
            self->onOutputsChanged();
    }

    static void outputScale(void*, wl_output*, int32_t) {}

    static void xdgOutputPosition(void* data, zxdg_output_v1* xdgOutput, int32_t x, int32_t y)
    {
        auto* self = static_cast<Connection*>(data);
        for (Connection::Output& output : self->m_outputs) {
            if (output.xdgOutput == xdgOutput)
                output.position = {double(x), double(y)};
        }
    }

    static void xdgOutputSize(void* data, zxdg_output_v1* xdgOutput, int32_t width, int32_t height)
    {
        auto* self = static_cast<Connection*>(data);
        for (Connection::Output& output : self->m_outputs) {
            if (output.xdgOutput == xdgOutput)
                output.logicalSize = {double(width), double(height)};
        }
    }

    // Below version 3 only, where wl_output.done may not follow.
    static void xdgOutputDone(void* data, zxdg_output_v1* xdgOutput)
    {
        auto* self = static_cast<Connection*>(data);
        if (zxdg_output_v1_get_version(xdgOutput) < 3 && self->onOutputsChanged)
            self->onOutputsChanged();
    }

    static void xdgOutputName(void* data, zxdg_output_v1* xdgOutput, const char* name)
    {
        auto* self = static_cast<Connection*>(data);
        for (Connection::Output& output : self->m_outputs) {
            if (output.xdgOutput == xdgOutput)
                output.outputName = name ? name : "";
        }
    }
    static void xdgOutputDescription(void*, zxdg_output_v1*, const char*) {}

    static void interactiveResize(void*, cutefish_window_hints_v1*, wl_surface* surface, uint32_t active)
    {
        if (ShellSurface* shell = shellSurfaceFor(surface))
            shell->delegate()->interactiveResize(active != 0);
    }

    static void fullscreenReveal(void*, cutefish_window_hints_v1*, wl_surface* surface, int32_t offset,
                                 uint32_t duration)
    {
        if (ShellSurface* shell = shellSurfaceFor(surface))
            shell->delegate()->fullScreenReveal(offset, int(duration));
    }

    static void workArea(void* data, cutefish_window_hints_v1*, const char* output, int32_t x, int32_t y,
                         int32_t width, int32_t height, uint32_t primary)
    {
        auto* self = static_cast<Connection*>(data);
        self->m_workAreas[output ? output : ""] = {{double(x), double(y), double(width), double(height)},
                                                   primary != 0};
        if (self->onOutputsChanged)
            self->onOutputsChanged();
    }

    static void windowPosition(void*, cutefish_window_placement_v1*, wl_surface* surface, int32_t x, int32_t y)
    {
        if (ShellSurface* shell = shellSurfaceFor(surface))
            shell->delegate()->screenPosition({double(x), double(y)});
    }

    static void ping(void*, xdg_wm_base* wmBase, uint32_t serial)
    {
        xdg_wm_base_pong(wmBase, serial);
    }

    static void capabilities(void* data, wl_seat* seat, uint32_t capabilities)
    {
        auto* self = static_cast<Connection*>(data);
        const bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
        if (hasPointer && !self->m_pointer) {
            self->m_pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(self->m_pointer, &pointer, self);
            if (self->m_cursorShapeManager)
                self->m_cursorShapeDevice = wp_cursor_shape_manager_v1_get_pointer(
                    self->m_cursorShapeManager, self->m_pointer);
            if (self->m_pointerGestures) {
                self->m_hold = zwp_pointer_gestures_v1_get_hold_gesture(self->m_pointerGestures,
                                                                        self->m_pointer);
                zwp_pointer_gesture_hold_v1_add_listener(self->m_hold, &hold, self);
            }
        } else if (!hasPointer && self->m_pointer) {
            if (self->m_hold) {
                zwp_pointer_gesture_hold_v1_destroy(self->m_hold);
                self->m_hold = nullptr;
            }
            if (self->m_cursorShapeDevice) {
                wp_cursor_shape_device_v1_destroy(self->m_cursorShapeDevice);
                self->m_cursorShapeDevice = nullptr;
            }
            wl_pointer_release(self->m_pointer);
            self->m_pointer = nullptr;
            // The compositor need not send a leave for a pointer the seat lost.
            if (ShellSurface* focus = std::exchange(self->m_pointerFocus, nullptr))
                focus->delegate()->pointerLeave();
        }

        const bool hasTouch = capabilities & WL_SEAT_CAPABILITY_TOUCH;
        if (hasTouch && !self->m_touch) {
            self->m_touch = wl_seat_get_touch(seat);
            wl_touch_add_listener(self->m_touch, &touch, self);
        } else if (!hasTouch && self->m_touch) {
            wl_touch_release(self->m_touch);
            self->m_touch = nullptr;
            touchCancel(self, nullptr);
        }

        const bool hasKeyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
        if (hasKeyboard && !self->m_keyboard) {
            self->m_keyboard = std::make_unique<Keyboard>(self->m_loop, wl_seat_get_keyboard(seat));
            self->m_keyboard->onKeyPressSerial = [self](uint32_t serial) {
                self->m_inputSerial = serial;
            };
        } else if (!hasKeyboard && self->m_keyboard) {
            ShellSurface* focus = self->m_keyboard->focus();
            self->m_keyboard.reset();
            if (focus)
                focus->delegate()->keyboardLeave();
        }
    }

    static void seatName(void*, wl_seat*, const char*)
    {
    }

    static void pointerEnter(void* data, wl_pointer*, uint32_t serial, wl_surface* surface,
                             wl_fixed_t x, wl_fixed_t y)
    {
        auto* self = static_cast<Connection*>(data);
        self->m_pointerFocus = shellSurfaceFor(surface);
        self->m_enterSerial = serial;
        // Without a cursor of its own the client's surface shows none.
        if (self->m_cursorShapeDevice)
            wp_cursor_shape_device_v1_set_shape(self->m_cursorShapeDevice, serial,
                                                WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
        if (self->m_pointerFocus)
            self->m_pointerFocus->delegate()->pointerEnter(wl_fixed_to_double(x),
                                                           wl_fixed_to_double(y));
    }

    static void pointerLeave(void* data, wl_pointer*, uint32_t, wl_surface*)
    {
        auto* self = static_cast<Connection*>(data);
        if (self->m_pointerFocus)
            self->m_pointerFocus->delegate()->pointerLeave();
        self->m_pointerFocus = nullptr;
    }

    static void pointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y)
    {
        auto* self = static_cast<Connection*>(data);
        if (self->m_pointerFocus)
            self->m_pointerFocus->delegate()->pointerMotion(wl_fixed_to_double(x),
                                                            wl_fixed_to_double(y));
    }

    static void pointerButton(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button,
                              uint32_t state)
    {
        auto* self = static_cast<Connection*>(data);
        const bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
        if (pressed) {
            self->m_buttonSerial = serial;
            self->m_inputSerial = serial;
        }
        MouseButton mouseButton;
        switch (button) {
        case BTN_LEFT:
            mouseButton = MouseButton::Left;
            break;
        case BTN_RIGHT:
            mouseButton = MouseButton::Right;
            break;
        case BTN_MIDDLE:
            mouseButton = MouseButton::Middle;
            break;
        // The side and extra buttons go back and forward.
        case BTN_SIDE:
        case BTN_BACK:
            mouseButton = MouseButton::Back;
            break;
        case BTN_EXTRA:
        case BTN_FORWARD:
            mouseButton = MouseButton::Forward;
            break;
        default:
            return;
        }
        const uint32_t modifiers = self->m_keyboard ? self->m_keyboard->modifiers() : NoModifier;
        if (self->m_pointerFocus)
            self->m_pointerFocus->delegate()->pointerButton(mouseButton, pressed, modifiers);
    }

    static void pointerAxis(void* data, wl_pointer*, uint32_t time, uint32_t axis, wl_fixed_t value)
    {
        auto* self = static_cast<Connection*>(data);
        self->m_scroll.time = time;
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            self->m_scroll.deltaY += wl_fixed_to_double(value);
        else
            self->m_scroll.deltaX += wl_fixed_to_double(value);
        self->m_scrollPending = true;
    }

    static void pointerFrame(void* data, wl_pointer*)
    {
        auto* self = static_cast<Connection*>(data);
        if (!self->m_scrollPending)
            return;
        ScrollEvent event = self->m_scroll;
        event.modifiers = self->m_keyboard ? self->m_keyboard->modifiers() : NoModifier;
        self->m_scroll = {};
        self->m_scrollPending = false;
        if (self->m_pointerFocus)
            self->m_pointerFocus->delegate()->pointerScroll(event);
    }

    static void pointerAxisSource(void* data, wl_pointer*, uint32_t source)
    {
        auto* self = static_cast<Connection*>(data);
        self->m_scroll.precise = source == WL_POINTER_AXIS_SOURCE_FINGER
                || source == WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    }

    static void pointerAxisStop(void* data, wl_pointer*, uint32_t time, uint32_t)
    {
        auto* self = static_cast<Connection*>(data);
        self->m_scroll.time = time;
        self->m_scroll.stop = true;
        self->m_scrollPending = true;
    }

    // Superseded by axis_value120 at the version bound.
    static void pointerAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t)
    {
    }

    static void pointerAxisValue120(void* data, wl_pointer*, uint32_t axis, int32_t value120)
    {
        auto* self = static_cast<Connection*>(data);
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            self->m_scroll.value120Y += value120;
        else
            self->m_scroll.value120X += value120;
        self->m_scrollPending = true;
    }

    static ShellSurface* touchFocus(Connection* self, int32_t id)
    {
        for (const auto& [pointId, shell] : self->m_touchFocus) {
            if (pointId == id)
                return shell;
        }
        return nullptr;
    }

    static void touchDown(void* data, wl_touch*, uint32_t serial, uint32_t, wl_surface* surface,
                          int32_t id, wl_fixed_t x, wl_fixed_t y)
    {
        auto* self = static_cast<Connection*>(data);
        ShellSurface* shell = shellSurfaceFor(surface);
        if (!shell)
            return;
        self->m_buttonSerial = serial;
        self->m_inputSerial = serial;
        self->m_touchFocus.emplace_back(id, shell);
        shell->delegate()->touchDown(id, wl_fixed_to_double(x), wl_fixed_to_double(y));
    }

    static void touchUp(void* data, wl_touch*, uint32_t, uint32_t, int32_t id)
    {
        auto* self = static_cast<Connection*>(data);
        ShellSurface* shell = touchFocus(self, id);
        std::erase_if(self->m_touchFocus, [id](const auto& point) { return point.first == id; });
        if (shell)
            shell->delegate()->touchUp(id);
    }

    static void touchMotion(void* data, wl_touch*, uint32_t, int32_t id, wl_fixed_t x, wl_fixed_t y)
    {
        auto* self = static_cast<Connection*>(data);
        if (ShellSurface* shell = touchFocus(self, id))
            shell->delegate()->touchMotion(id, wl_fixed_to_double(x), wl_fixed_to_double(y));
    }

    static void touchFrame(void*, wl_touch*)
    {
    }

    static void touchCancel(void* data, wl_touch*)
    {
        auto* self = static_cast<Connection*>(data);
        std::vector<ShellSurface*> cancelled;
        for (const auto& point : self->m_touchFocus) {
            if (std::find(cancelled.begin(), cancelled.end(), point.second) == cancelled.end())
                cancelled.push_back(point.second);
        }
        self->m_touchFocus.clear();
        for (ShellSurface* shell : cancelled)
            shell->delegate()->touchCancel();
    }

    static void touchShape(void*, wl_touch*, int32_t, wl_fixed_t, wl_fixed_t)
    {
    }

    static void touchOrientation(void*, wl_touch*, int32_t, wl_fixed_t)
    {
    }

    static void holdBegin(void*, zwp_pointer_gesture_hold_v1*, uint32_t, uint32_t,
                          wl_surface* surface, uint32_t)
    {
        if (ShellSurface* shell = shellSurfaceFor(surface))
            shell->delegate()->pointerHold();
    }

    static void holdEnd(void*, zwp_pointer_gesture_hold_v1*, uint32_t, uint32_t, int32_t)
    {
    }

    static constexpr wl_registry_listener registry = {global, globalRemove};
    static constexpr wl_output_listener outputListener = {outputGeometry, outputMode, outputDone, outputScale};
    static constexpr zxdg_output_v1_listener xdgOutput = {xdgOutputPosition, xdgOutputSize, xdgOutputDone,
                                                          xdgOutputName, xdgOutputDescription};
    static constexpr zwp_pointer_gesture_hold_v1_listener hold = {holdBegin, holdEnd};
    static constexpr xdg_wm_base_listener wmBase = {ping};
    static constexpr cutefish_window_hints_v1_listener windowHints = {interactiveResize, workArea, fullscreenReveal};
    static constexpr cutefish_window_placement_v1_listener windowPlacement = {windowPosition};
    static constexpr wl_seat_listener seat = {capabilities, seatName};
    static constexpr wl_pointer_listener pointer = {
        pointerEnter,
        pointerLeave,
        pointerMotion,
        pointerButton,
        pointerAxis,
        pointerFrame,
        pointerAxisSource,
        pointerAxisStop,
        pointerAxisDiscrete,
        pointerAxisValue120,
        nullptr,
    };
    static constexpr wl_touch_listener touch = {
        touchDown,
        touchUp,
        touchMotion,
        touchFrame,
        touchCancel,
        touchShape,
        touchOrientation,
    };
};

std::unique_ptr<Connection> Connection::open(base::EventLoop& loop)
{
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        fprintf(stderr, "cutefish-toolkit: cannot connect to the Wayland compositor\n");
        return nullptr;
    }
    std::unique_ptr<Connection> connection(new Connection(loop, display));
    if (!connection->initialize())
        return nullptr;
    return connection;
}

std::unique_ptr<Connection> Connection::openFd(base::EventLoop& loop, int fd)
{
    // wl_display_connect_to_fd leaves the flags alone: a program this one
    // starts must not inherit a privileged connection.
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    wl_display* display = wl_display_connect_to_fd(fd);
    if (!display) {
        fprintf(stderr, "cutefish-toolkit: cannot connect over the descriptor given\n");
        return nullptr;
    }
    std::unique_ptr<Connection> connection(new Connection(loop, display));
    if (!connection->initialize())
        return nullptr;
    return connection;
}

std::vector<Connection::OutputInfo> Connection::outputs() const
{
    std::vector<OutputInfo> list;
    for (const Output& output : m_outputs) {
        OutputInfo info {output.outputName, output.position, output.logicalSize,
                         {output.position.x, output.position.y, output.logicalSize.width,
                          output.logicalSize.height}};
        // Either side of the output, whichever way it is turned.
        const double logical = std::max(output.logicalSize.width, output.logicalSize.height);
        if (logical > 0 && output.pixelSize.width > 0)
            info.scale = std::max(output.pixelSize.width, output.pixelSize.height) / logical;
        if (const auto area = m_workAreas.find(output.outputName); area != m_workAreas.end()) {
            info.workArea = area->second.area;
            info.primary = area->second.primary;
        }
        list.push_back(std::move(info));
    }
    return list;
}

Connection::Connection(base::EventLoop& loop, wl_display* display)
    : m_loop(loop)
    , m_display(display)
{
    s_connections.push_back(this);
}

Connection* Connection::withKeyboardFocus()
{
    for (Connection* connection : s_connections) {
        if (connection->m_keyboard && connection->m_keyboard->focus())
            return connection;
    }
    return nullptr;
}

void Connection::releaseOutput(const Output& output)
{
    if (output.xdgOutput)
        zxdg_output_v1_destroy(output.xdgOutput);
    if (wl_output_get_version(output.output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
        wl_output_release(output.output);
    else
        wl_output_destroy(output.output);
}

void Connection::watchOutput(Output& output)
{
    if (!m_outputManager || output.xdgOutput)
        return;
    output.xdgOutput = zxdg_output_manager_v1_get_xdg_output(m_outputManager, output.output);
    zxdg_output_v1_add_listener(output.xdgOutput, &ConnectionListeners::xdgOutput, this);
}

SizeF Connection::outputSize(wl_output* output) const
{
    for (const Output& entry : m_outputs) {
        if (entry.output == output)
            return entry.logicalSize;
    }
    return {};
}

SizeF Connection::primaryOutputSize() const
{
    return m_outputs.empty() ? SizeF{} : m_outputs.front().logicalSize;
}

Connection::~Connection()
{
    std::erase(s_connections, this);
    if (m_watch)
        m_loop.unwatch(m_watch);
    if (m_hook)
        m_loop.removePrepareHook(m_hook);
    for (const Output& output : m_outputs)
        releaseOutput(output);
    if (m_outputManager)
        zxdg_output_manager_v1_destroy(m_outputManager);
    m_textInput.reset();
    m_dataDevice.reset();
    m_primarySelection.reset();
    if (m_primarySelectionManager)
        zwp_primary_selection_device_manager_v1_destroy(m_primarySelectionManager);
    if (m_textInputManager)
        zwp_text_input_manager_v3_destroy(m_textInputManager);
    if (m_dataDeviceManager)
        wl_data_device_manager_destroy(m_dataDeviceManager);
    m_keyboard.reset();
    if (m_hold)
        zwp_pointer_gesture_hold_v1_destroy(m_hold);
    if (m_pointerGestures)
        zwp_pointer_gestures_v1_destroy(m_pointerGestures);
    if (m_cursorShapeDevice)
        wp_cursor_shape_device_v1_destroy(m_cursorShapeDevice);
    if (m_pointer)
        wl_pointer_release(m_pointer);
    if (m_touch)
        wl_touch_release(m_touch);
    if (m_seat)
        wl_seat_release(m_seat);
    if (m_windowHints)
        cutefish_window_hints_v1_destroy(m_windowHints);
    if (m_windowPlacement)
        cutefish_window_placement_v1_destroy(m_windowPlacement);
    if (m_surfaceBlur)
        cutefish_surface_blur_v1_destroy(m_surfaceBlur);
    if (m_statusItems)
        cutefish_status_item_manager_v1_destroy(m_statusItems);
    if (m_mainMenus)
        cutefish_main_menu_manager_v1_destroy(m_mainMenus);
    if (m_dialogManager)
        xdg_wm_dialog_v1_destroy(m_dialogManager);
    for (const auto& pending : m_activations)
        xdg_activation_token_v1_destroy(pending.token);
    for (const Export& e : m_exports)
        zxdg_exported_v2_destroy(e.exported);
    if (m_exporter)
        zxdg_exporter_v2_destroy(m_exporter);
    if (m_importer)
        zxdg_importer_v2_destroy(m_importer);
    if (m_activation)
        xdg_activation_v1_destroy(m_activation);
    if (m_cursorShapeManager)
        wp_cursor_shape_manager_v1_destroy(m_cursorShapeManager);
    if (m_presentation)
        wp_presentation_destroy(m_presentation);
    if (m_fractionalScaleManager)
        wp_fractional_scale_manager_v1_destroy(m_fractionalScaleManager);
    if (m_viewporter)
        wp_viewporter_destroy(m_viewporter);
    if (m_wmBase)
        xdg_wm_base_destroy(m_wmBase);
    if (m_toplevelDragManager)
        xdg_toplevel_drag_manager_v1_destroy(m_toplevelDragManager);
    if (m_shell)
        cutefish_shell_v1_destroy(m_shell);
    if (m_shm)
        wl_shm_destroy(m_shm);
    if (m_subcompositor)
        wl_subcompositor_destroy(m_subcompositor);
    if (m_compositor)
        wl_compositor_destroy(m_compositor);
    if (m_registry)
        wl_registry_destroy(m_registry);
    wl_display_flush(m_display);
    wl_display_disconnect(m_display);
}

bool Connection::initialize()
{
    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &ConnectionListeners::registry, this);
    // The first round trip binds the globals, the second brings the seat's
    // capabilities.
    wl_display_roundtrip(m_display);
    wl_display_roundtrip(m_display);

    if (!m_compositor || !m_wmBase || !m_viewporter || !m_fractionalScaleManager) {
        fprintf(stderr, "cutefish-toolkit: the compositor lacks a required global\n");
        return false;
    }
    if (m_seat && m_dataDeviceManager)
        m_dataDevice = std::make_unique<DataDevice>(*this, m_loop, m_dataDeviceManager, m_seat);
    if (m_seat && m_primarySelectionManager) {
        m_primarySelection = std::make_unique<PrimarySelection>(*this, m_loop, m_primarySelectionManager,
                                                                m_seat);
    }
    if (m_seat && m_textInputManager)
        m_textInput = std::make_unique<TextInput>(m_textInputManager, m_seat);

    // Reads without blocking: a drawing thread waiting on a queue of its own
    // may have read what woke the loop already.
    m_watch = m_loop.watch(wl_display_get_fd(m_display), EPOLLIN, [this](uint32_t) {
        // A failed display keeps its queued events, so preparing again
        // would never succeed.
        while (wl_display_prepare_read(m_display) != 0) {
            if (wl_display_dispatch_pending(m_display) < 0) {
                handleConnectionLost();
                return;
            }
        }
        if (wl_display_read_events(m_display) < 0 || wl_display_dispatch_pending(m_display) < 0)
            handleConnectionLost();
    });
    // Events another queue's reader left on ours would otherwise wait for
    // the next wake-up.
    m_hook = m_loop.addPrepareHook([this] {
        wl_display_dispatch_pending(m_display);
        wl_display_flush(m_display);
    });
    return true;
}

// The descriptor of a closed connection stays readable for good: the loop
// stops watching it, so the loss is reported once, and is asked to end.
void Connection::handleConnectionLost()
{
    m_loop.unwatch(m_watch);
    m_watch = 0;
    m_loop.removePrepareHook(m_hook);
    m_hook = 0;
    m_lost = true;
    fprintf(stderr, "cutefish-toolkit: lost the Wayland connection\n");
    m_loop.quit(1);
}

uint32_t Connection::keyboardModifiers() const
{
    return m_keyboard ? m_keyboard->modifiers() : 0;
}

void Connection::setCursorShape(ShellSurface* shell, CursorShape shape)
{
    if (shell != m_pointerFocus)
        return;
    // No surface hides the pointer over the window.
    if (shape == CursorShape::Blank) {
        if (m_pointer)
            wl_pointer_set_cursor(m_pointer, m_enterSerial, nullptr, 0, 0);
        return;
    }
    if (!m_cursorShapeDevice)
        return;
    uint32_t wire = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    switch (shape) {
    case CursorShape::Default:
        break;
    case CursorShape::Pointer:
        wire = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
        break;
    case CursorShape::Text:
        wire = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
        break;
    case CursorShape::Move:
        wire = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
        break;
    case CursorShape::Grab:
        wire = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
        break;
    case CursorShape::Grabbing:
        wire = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
        break;
    case CursorShape::NotAllowed:
        wire = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
        break;
    case CursorShape::ResizeHorizontal:
        wire = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
        break;
    case CursorShape::ResizeVertical:
        wire = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
        break;
    case CursorShape::Blank:
        break;
    }
    wp_cursor_shape_device_v1_set_shape(m_cursorShapeDevice, m_enterSerial, wire);
}

static void syncDone(void* data, wl_callback* callback, uint32_t)
{
    auto* done = static_cast<std::function<void()>*>(data);
    wl_callback_destroy(callback);
    (*done)();
    delete done;
}

static const wl_callback_listener syncListener = {syncDone};

void Connection::sync(std::function<void()> done)
{
    wl_callback* callback = wl_display_sync(m_display);
    wl_callback_add_listener(callback, &syncListener, new std::function<void()>(std::move(done)));
    flush();
}

std::shared_ptr<MainMenuMirror> Connection::mainMenu(Menu* menu)
{
    if (!m_mainMenus || !menu)
        return nullptr;
    std::weak_ptr<MainMenuMirror>& entry = m_mainMenuMirrors[menu];
    std::shared_ptr<MainMenuMirror> mirror = entry.lock();
    // A menu at the address of one destroyed since.
    if (mirror && !mirror->isAlive())
        mirror.reset();
    if (!mirror) {
        mirror = std::make_shared<MainMenuMirror>(*this, menu);
        entry = mirror;
    }
    std::erase_if(m_mainMenuMirrors, [](const auto& e) { return e.second.expired(); });
    return mirror;
}

static void tokenDone(void* data, xdg_activation_token_v1* token, const char* tokenString)
{
    auto* self = static_cast<Connection*>(data);
    self->activationDone(token, tokenString);
}

static const xdg_activation_token_v1_listener activationTokenListener = {tokenDone};

void Connection::activate(ShellSurface* shell, const std::string& appId)
{
    if (!m_activation || !shell)
        return;
    xdg_activation_token_v1* token = xdg_activation_v1_get_activation_token(m_activation);
    if (m_inputSerial && m_seat)
        xdg_activation_token_v1_set_serial(token, m_inputSerial, m_seat);
    if (ShellSurface* focus = m_keyboard ? m_keyboard->focus() : nullptr)
        xdg_activation_token_v1_set_surface(token, focus->surface());
    if (!appId.empty())
        xdg_activation_token_v1_set_app_id(token, appId.c_str());
    xdg_activation_token_v1_add_listener(token, &activationTokenListener, this);
    xdg_activation_token_v1_commit(token);
    m_activations.push_back({token, shell, nullptr});
    flush();
}

void Connection::activateWithToken(ShellSurface* shell, const std::string& token)
{
    if (!m_activation || !shell)
        return;
    xdg_activation_v1_activate(m_activation, token.c_str(), shell->surface());
    flush();
}

void Connection::requestActivationToken(ShellSurface* shell, std::function<void(const std::string&)> done)
{
    if (!m_activation) {
        done({});
        return;
    }
    xdg_activation_token_v1* token = xdg_activation_v1_get_activation_token(m_activation);
    if (m_inputSerial && m_seat)
        xdg_activation_token_v1_set_serial(token, m_inputSerial, m_seat);
    if (shell)
        xdg_activation_token_v1_set_surface(token, shell->surface());
    xdg_activation_token_v1_add_listener(token, &activationTokenListener, this);
    xdg_activation_token_v1_commit(token);
    m_activations.push_back({token, nullptr, std::move(done)});
    flush();
}

void Connection::activationDone(xdg_activation_token_v1* token, const char* tokenString)
{
    const auto found = std::find_if(m_activations.begin(), m_activations.end(),
                                    [token](const auto& pending) { return pending.token == token; });
    std::function<void(const std::string&)> done;
    if (found != m_activations.end()) {
        if (found->shell)
            xdg_activation_v1_activate(m_activation, tokenString, found->shell->surface());
        done = std::move(found->done);
        m_activations.erase(found);
    }
    xdg_activation_token_v1_destroy(token);
    flush();
    if (done)
        done(tokenString ? tokenString : "");
}

static void exportedHandle(void* data, zxdg_exported_v2* exported, const char* handle)
{
    static_cast<Connection*>(data)->exported(exported, handle);
}

static const zxdg_exported_v2_listener exportedListener = {exportedHandle};

void Connection::exportToplevel(ShellSurface* shell, std::function<void(const std::string& handle)> done)
{
    if (!m_exporter || !shell) {
        done({});
        return;
    }
    const auto found =
        std::find_if(m_exports.begin(), m_exports.end(), [shell](const Export& e) { return e.shell == shell; });
    if (found != m_exports.end()) {
        if (found->handle.empty())
            found->waiting.push_back(std::move(done));
        else
            done(found->handle);
        return;
    }
    zxdg_exported_v2* exported = zxdg_exporter_v2_export_toplevel(m_exporter, shell->surface());
    zxdg_exported_v2_add_listener(exported, &exportedListener, this);
    m_exports.push_back({shell, exported, {}, {}});
    m_exports.back().waiting.push_back(std::move(done));
    flush();
}

void Connection::exported(zxdg_exported_v2* exported, const char* handle)
{
    const auto found = std::find_if(m_exports.begin(), m_exports.end(),
                                    [exported](const Export& e) { return e.exported == exported; });
    if (found == m_exports.end())
        return;
    found->handle = std::string("wayland:") + handle;
    std::vector<std::function<void(const std::string&)>> waiting = std::move(found->waiting);
    const std::string name = found->handle;
    for (const auto& done : waiting)
        done(name);
}

void Connection::forget(ShellSurface* shell)
{
    std::erase_if(m_exports, [shell](const Export& e) {
        if (e.shell != shell)
            return false;
        zxdg_exported_v2_destroy(e.exported);
        return true;
    });
    for (auto& pending : m_activations) {
        if (pending.shell == shell)
            pending.shell = nullptr;
    }
    if (m_pointerFocus == shell)
        m_pointerFocus = nullptr;
    std::erase_if(m_touchFocus, [shell](const auto& point) { return point.second == shell; });
    if (m_keyboard)
        m_keyboard->forget(shell);
    if (m_dataDevice)
        m_dataDevice->forget(shell);
    if (m_textInput)
        m_textInput->forget(shell);
}

void Connection::flush()
{
    wl_display_flush(m_display);
}

} // namespace cutefish::ui::wayland
