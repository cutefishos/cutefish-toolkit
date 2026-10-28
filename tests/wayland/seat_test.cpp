// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <linux/input-event-codes.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wayland-client.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#include "fractional-scale-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include "base/event_loop.h"
#include "wayland/connection.h"
#include "wayland/shell_surface.h"

using namespace cutefish;
using namespace cutefish::ui;

namespace {

// Every global a ShellSurface and the seat need, on a thread of its own.
// Requests are not answered, but each object they create gets a resource,
// so events can name it.
class FakeCompositor
{
public:
    FakeCompositor()
    {
        int fds[2];
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds);
        clientFd = fds[1];
        m_display = wl_display_create();
        m_wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        m_wakeSource = wl_event_loop_add_fd(wl_display_get_event_loop(m_display), m_wakeFd, WL_EVENT_READABLE,
                                            runTasks, this);
        global(&wl_compositor_interface, 4);
        global(&xdg_wm_base_interface, 1);
        global(&wp_viewporter_interface, 1);
        global(&wp_fractional_scale_manager_v1_interface, 1);
        global(&wl_seat_interface, 7);
        global(&wl_data_device_manager_interface, 3);
        m_client = wl_client_create(m_display, fds[0]);
        m_thread = std::thread([this] {
            wl_event_loop* loop = wl_display_get_event_loop(m_display);
            while (!m_stop) {
                wl_event_loop_dispatch(loop, 10);
                wl_display_flush_clients(m_display);
            }
        });
    }

    ~FakeCompositor()
    {
        m_stop = true;
        m_thread.join();
        wl_display_destroy_clients(m_display);
        wl_event_source_remove(m_wakeSource);
        wl_display_destroy(m_display);
        close(m_wakeFd);
    }

    // Runs on the compositor's thread, with the resources made so far.
    void post(std::function<void(FakeCompositor&)> task)
    {
        {
            std::lock_guard lock(m_mutex);
            m_tasks.push_back(std::move(task));
        }
        const uint64_t one = 1;
        (void)!write(m_wakeFd, &one, sizeof one);
    }

    wl_resource* last(const wl_interface& interface) const
    {
        const auto it = m_resources.find(interface.name);
        return it == m_resources.end() || it->second.empty() ? nullptr : it->second.back();
    }

    wl_resource* create(const wl_interface& interface, int version, uint32_t id)
    {
        wl_resource* resource = wl_resource_create(m_client, &interface, version, id);
        wl_resource_set_dispatcher(resource, dispatch, nullptr, this, nullptr);
        m_resources[interface.name].push_back(resource);
        return resource;
    }

    int clientFd = -1;

private:
    struct Global
    {
        FakeCompositor* self;
        const wl_interface* interface;
    };

    void global(const wl_interface* interface, int version)
    {
        m_globals.push_back(std::make_unique<Global>(Global{this, interface}));
        wl_global_create(m_display, interface, version, m_globals.back().get(), bind);
    }

    static void bind(wl_client*, void* data, uint32_t version, uint32_t id)
    {
        const auto* global = static_cast<const Global*>(data);
        global->self->create(*global->interface, int(version), id);
    }

    static int dispatch(const void*, void* object, uint32_t, const wl_message* message, wl_argument* arguments)
    {
        auto* resource = static_cast<wl_resource*>(object);
        static_cast<FakeCompositor*>(wl_resource_get_user_data(resource))->createObjects(resource, message, arguments);
        return 0;
    }

    void createObjects(wl_resource* resource, const wl_message* message, wl_argument* arguments)
    {
        int index = 0;
        for (const char* c = message->signature; *c; ++c) {
            if (*c == '?' || (*c >= '0' && *c <= '9'))
                continue;
            if (*c == 'n' && message->types[index])
                create(*message->types[index], wl_resource_get_version(resource), arguments[index].n);
            ++index;
        }
    }

    static int runTasks(int fd, uint32_t, void* data)
    {
        auto* self = static_cast<FakeCompositor*>(data);
        uint64_t count;
        (void)!read(fd, &count, sizeof count);
        std::vector<std::function<void(FakeCompositor&)>> tasks;
        {
            std::lock_guard lock(self->m_mutex);
            tasks.swap(self->m_tasks);
        }
        for (const auto& task : tasks)
            task(*self);
        return 0;
    }

    wl_display* m_display = nullptr;
    wl_client* m_client = nullptr;
    int m_wakeFd = -1;
    wl_event_source* m_wakeSource = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop = false;
    std::mutex m_mutex;
    std::vector<std::function<void(FakeCompositor&)>> m_tasks;
    std::vector<std::unique_ptr<Global>> m_globals;
    std::map<std::string, std::vector<wl_resource*>> m_resources;
};

class InputTarget final : public wayland::ShellSurface::Delegate
{
public:
    void configure(int, int, const WindowStates&) override {}
    void scaleChanged(double) override {}
    void closeRequested() override {}
    void frameDone() override {}
    void presented(uint64_t, uint32_t) override {}
    void discarded() override {}
    void pointerEnter(double, double) override { ++pointerEntered; }
    void pointerLeave() override { ++pointerLeft; }
    void pointerMotion(double, double) override {}
    void pointerButton(MouseButton, bool, uint32_t) override {}
    void pointerScroll(const ScrollEvent&) override {}
    void pointerHold() override {}
    void touchDown(int32_t, double, double) override { ++touchesDown; }
    void touchCancel() override { ++touchesCancelled; }
    void keyboardEnter() override { ++keyboardEntered; }
    void keyboardLeave() override { ++keyboardLeft; }
    void key(const KeyEvent&, bool pressed) override
    {
        if (pressed)
            ++keysPressed;
    }
    void textInput(const TextInputEvent&) override {}

    int pointerEntered = 0;
    int pointerLeft = 0;
    int touchesDown = 0;
    int touchesCancelled = 0;
    int keyboardEntered = 0;
    int keyboardLeft = 0;
    int keysPressed = 0;
};

// Waits until the compositor ran what was posted and the client read what
// it sent.
void settle(FakeCompositor& compositor, wl_display* display)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    compositor.post([done](FakeCompositor&) { *done = true; });
    for (int i = 0; i < 100 && !*done; ++i)
        wl_display_roundtrip(display);
    // The handlers of the events read here may send requests; the second
    // roundtrip has the compositor read them.
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
}


static void runFor(base::EventLoop& loop, std::chrono::milliseconds duration)
{
    loop.startTimer(duration, false, [&loop] { loop.quit(); });
    loop.run();
}

} // namespace

// A seat that loses its pointer, keyboard or touch ends their focus on the window.
TEST(SeatTest, LosingADeviceEndsItsFocus)
{
    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);

    InputTarget target;
    auto shell = std::make_unique<wayland::ShellSurface>(*connection, &target);
    compositor.post([](FakeCompositor& self) {
        wl_seat_send_capabilities(self.last(wl_seat_interface), WL_SEAT_CAPABILITY_POINTER
                                  | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH);
    });
    connection->flush();
    settle(compositor, connection->display());

    compositor.post([](FakeCompositor& self) {
        wl_resource* surface = self.last(wl_surface_interface);
        wl_resource* pointer = self.last(wl_pointer_interface);
        wl_resource* keyboard = self.last(wl_keyboard_interface);
        wl_resource* touch = self.last(wl_touch_interface);
        if (!surface || !pointer || !keyboard || !touch)
            return;
        wl_pointer_send_enter(pointer, 1, surface, wl_fixed_from_int(10), wl_fixed_from_int(10));
        wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(keyboard, 2, surface, &keys);
        wl_array_release(&keys);
        wl_touch_send_down(touch, 3, 0, surface, 0, wl_fixed_from_int(10), wl_fixed_from_int(10));
    });
    settle(compositor, connection->display());
    ASSERT_EQ(target.pointerEntered, 1);
    ASSERT_EQ(target.keyboardEntered, 1);
    ASSERT_EQ(target.touchesDown, 1);

    compositor.post([](FakeCompositor& self) { wl_seat_send_capabilities(self.last(wl_seat_interface), 0); });
    settle(compositor, connection->display());
    EXPECT_EQ(target.pointerLeft, 1);
    EXPECT_EQ(target.keyboardLeft, 1);
    EXPECT_EQ(target.touchesCancelled, 1);
}

// A repeat rate of zero stops the repeat of a key held down.
TEST(SeatTest, ZeroRepeatRateStopsTheRepeat)
{
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    ASSERT_TRUE(context);
    xkb_keymap* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    ASSERT_TRUE(keymap);
    char* text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    const std::string keymapText = text;
    free(text);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);

    InputTarget target;
    auto shell = std::make_unique<wayland::ShellSurface>(*connection, &target);
    compositor.post([](FakeCompositor& self) {
        wl_seat_send_capabilities(self.last(wl_seat_interface), WL_SEAT_CAPABILITY_KEYBOARD);
    });
    connection->flush();
    settle(compositor, connection->display());

    compositor.post([keymapText](FakeCompositor& self) {
        wl_resource* surface = self.last(wl_surface_interface);
        wl_resource* keyboard = self.last(wl_keyboard_interface);
        if (!surface || !keyboard)
            return;
        const int fd = memfd_create("keymap", MFD_CLOEXEC);
        (void)!write(fd, keymapText.c_str(), keymapText.size() + 1);
        wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, uint32_t(keymapText.size() + 1));
        close(fd);
        wl_keyboard_send_repeat_info(keyboard, 25, 50);
        wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(keyboard, 1, surface, &keys);
        wl_array_release(&keys);
    });
    settle(compositor, connection->display());
    // The keymap compiles on another thread.
    runFor(loop, std::chrono::milliseconds(300));
    ASSERT_EQ(target.keyboardEntered, 1);

    compositor.post([](FakeCompositor& self) {
        wl_resource* keyboard = self.last(wl_keyboard_interface);
        wl_keyboard_send_key(keyboard, 2, 0, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_keyboard_send_repeat_info(keyboard, 0, 50);
    });
    settle(compositor, connection->display());
    ASSERT_EQ(target.keysPressed, 1);
    runFor(loop, std::chrono::milliseconds(200));
    EXPECT_EQ(target.keysPressed, 1);
}
