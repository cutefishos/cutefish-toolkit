// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wayland-client.h>
#include <wayland-server.h>

#include "fractional-scale-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include "base/event_loop.h"
#include "wayland/connection.h"
#include "wayland/shell_surface.h"

using namespace cutefish;
using namespace cutefish::ui;

namespace {

// Every global a ShellSurface and a DataDevice need, on a thread of its own.
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

class DragTarget final : public wayland::ShellSurface::Delegate
{
public:
    void configure(int, int, const WindowStates&) override {}
    void scaleChanged(double) override {}
    void closeRequested() override {}
    void frameDone() override {}
    void presented(uint64_t, uint32_t) override {}
    void discarded() override {}
    void pointerEnter(double, double) override {}
    void pointerLeave() override {}
    void pointerMotion(double, double) override {}
    void pointerButton(MouseButton, bool, uint32_t) override {}
    void pointerScroll(const ScrollEvent&) override {}
    void pointerHold() override {}
    void keyboardEnter() override {}
    void keyboardLeave() override {}
    void key(const KeyEvent&, bool) override {}
    void textInput(const TextInputEvent&) override {}

    uint32_t dragEnter(double, double, DropOffer*, uint32_t) override
    {
        ++entered;
        return 0;
    }
    uint32_t dragMotion(double, double, uint32_t) override
    {
        ++moved;
        return 0;
    }
    void dragLeave() override { ++left; }

    int entered = 0;
    int moved = 0;
    int left = 0;
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

} // namespace

// A window destroyed while a drag is over it hears nothing more of the drag.
TEST(DataDeviceTest, DragForgetsAWindowThatGoes)
{
    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);
    ASSERT_TRUE(connection->dataDevice());

    DragTarget target;
    auto shell = std::make_unique<wayland::ShellSurface>(*connection, &target);
    connection->flush();
    settle(compositor, connection->display());

    compositor.post([](FakeCompositor& self) {
        wl_resource* device = self.last(wl_data_device_interface);
        wl_resource* surface = self.last(wl_surface_interface);
        if (!device || !surface)
            return;
        wl_resource* offer = self.create(wl_data_offer_interface, wl_resource_get_version(device), 0);
        wl_data_device_send_data_offer(device, offer);
        wl_data_device_send_enter(device, 1, surface, wl_fixed_from_int(10), wl_fixed_from_int(10), offer);
    });
    settle(compositor, connection->display());
    ASSERT_EQ(target.entered, 1);

    shell.reset();
    compositor.post([](FakeCompositor& self) {
        wl_resource* device = self.last(wl_data_device_interface);
        wl_data_device_send_motion(device, 2, wl_fixed_from_int(20), wl_fixed_from_int(20));
        wl_data_device_send_leave(device);
    });
    settle(compositor, connection->display());
    EXPECT_EQ(target.moved, 0);
    EXPECT_EQ(target.left, 0);
}
