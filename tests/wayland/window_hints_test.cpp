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

#include "cutefish-window-hints-v1-server-protocol.h"
#include "cutefish-window-placement-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include "base/event_loop.h"
#include "wayland/connection.h"
#include "ui/path.h"
#include "wayland/shell_surface.h"

using namespace cutefish;
using namespace cutefish::ui;

namespace {

// The globals a toplevel ShellSurface needs, on a thread of its own, with
// window placement when asked. Nothing is answered; the window hints and
// placement requests are written down with their integer arguments.
class FakeCompositor
{
public:
    struct Request
    {
        std::string name;
        std::vector<int64_t> arguments;
    };

    explicit FakeCompositor(bool placement = false)
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
        global(&cutefish_window_hints_v1_interface, 1);
        if (placement)
            global(&cutefish_window_placement_v1_interface, 1);
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

    void post(std::function<void()> task)
    {
        {
            std::lock_guard lock(m_mutex);
            m_tasks.push_back(std::move(task));
        }
        const uint64_t one = 1;
        (void)!write(m_wakeFd, &one, sizeof one);
    }

    std::vector<Request> hints()
    {
        std::lock_guard lock(m_mutex);
        return m_hints;
    }

    // cutefish_window_placement_v1.position for the surface made last.
    void sendPosition(int x, int y)
    {
        post([this, x, y] {
            for (wl_resource* placement : m_placements)
                cutefish_window_placement_v1_send_position(placement, m_surfaces.back(), x, y);
        });
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

    void create(const wl_interface& interface, int version, uint32_t id)
    {
        wl_resource* resource = wl_resource_create(m_client, &interface, version, id);
        wl_resource_set_dispatcher(resource, dispatch, nullptr, this, nullptr);
        if (&interface == &cutefish_window_placement_v1_interface)
            m_placements.push_back(resource);
        else if (&interface == &wl_surface_interface)
            m_surfaces.push_back(resource);
    }

    static int dispatch(const void*, void* object, uint32_t, const wl_message* message, wl_argument* arguments)
    {
        auto* resource = static_cast<wl_resource*>(object);
        auto* self = static_cast<FakeCompositor*>(wl_resource_get_user_data(resource));
        const bool hints = wl_resource_instance_of(resource, &cutefish_window_hints_v1_interface, nullptr)
                           || wl_resource_instance_of(resource, &cutefish_window_placement_v1_interface, nullptr);
        Request request{message->name, {}};
        int index = 0;
        for (const char* c = message->signature; *c; ++c) {
            if (*c == '?' || (*c >= '0' && *c <= '9'))
                continue;
            if (*c == 'n' && message->types[index])
                self->create(*message->types[index], wl_resource_get_version(resource), arguments[index].n);
            else if (*c == 'i')
                request.arguments.push_back(arguments[index].i);
            else if (*c == 'u')
                request.arguments.push_back(arguments[index].u);
            else if (*c == 'f')
                request.arguments.push_back(arguments[index].f);
            ++index;
        }
        if (hints) {
            std::lock_guard lock(self->m_mutex);
            self->m_hints.push_back(std::move(request));
        }
        return 0;
    }

    static int runTasks(int fd, uint32_t, void* data)
    {
        auto* self = static_cast<FakeCompositor*>(data);
        uint64_t count;
        (void)!read(fd, &count, sizeof count);
        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard lock(self->m_mutex);
            tasks.swap(self->m_tasks);
        }
        for (const auto& task : tasks)
            task();
        return 0;
    }

    wl_display* m_display = nullptr;
    wl_client* m_client = nullptr;
    int m_wakeFd = -1;
    wl_event_source* m_wakeSource = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop = false;
    std::mutex m_mutex;
    std::vector<std::function<void()>> m_tasks;
    std::vector<std::unique_ptr<Global>> m_globals;
    std::vector<Request> m_hints;
    std::vector<wl_resource*> m_placements;
    std::vector<wl_resource*> m_surfaces;
};

class Target final : public wayland::ShellSurface::Delegate
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
    void touchDown(int32_t, double, double) override {}
    void touchCancel() override {}
    void keyboardEnter() override {}
    void keyboardLeave() override {}
    void key(const KeyEvent&, bool) override {}
    void textInput(const TextInputEvent&) override {}
    void screenPosition(PointF position) override { positions.push_back(position); }

    std::vector<PointF> positions;
};

// Waits until the compositor has read everything the client sent.
void settle(FakeCompositor& compositor, wl_display* display)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    compositor.post([done] { *done = true; });
    for (int i = 0; i < 100 && !*done; ++i)
        wl_display_roundtrip(display);
    // The handlers of the events read here may send requests; the second
    // roundtrip has the compositor read them.
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
}

} // namespace

// The anchor goes out rounded to whole logical pixels, with
// the toolkit's anchor and gravity as the protocol's placement edges.
TEST(WindowHintsProtocolTest, PlacementAnchorIsSentAndUnset)
{
    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);
    ASSERT_TRUE(connection->windowHints());

    Target target;
    auto shell = std::make_unique<wayland::ShellSurface>(*connection, &target);
    shell->setPlacementAnchor(PlacementAnchor{{700.4, 12.6, 0, 0}, PopupAnchor::Bottom, PopupAnchor::Bottom});
    shell->setPlacementAnchor(
            PlacementAnchor{{10.5, 20, 30, 40.5}, PopupAnchor::TopLeft, PopupAnchor::BottomRight});
    shell->setPlacementAnchor(std::nullopt);
    connection->flush();
    settle(compositor, connection->display());

    // Only the placement requests: a toplevel sets other hints of its own.
    std::vector<FakeCompositor::Request> hints = compositor.hints();
    std::erase_if(hints, [](const FakeCompositor::Request& request) {
        return request.name.find("placement_anchor") == std::string::npos;
    });
    ASSERT_EQ(hints.size(), 3u);
    EXPECT_EQ(hints[0].name, "set_placement_anchor");
    EXPECT_EQ(hints[0].arguments, (std::vector<int64_t>{700, 13, 0, 0, CUTEFISH_WINDOW_HINTS_V1_PLACEMENT_EDGE_BOTTOM,
                                                        CUTEFISH_WINDOW_HINTS_V1_PLACEMENT_EDGE_BOTTOM}));
    EXPECT_EQ(hints[1].arguments,
              (std::vector<int64_t>{11, 20, 30, 41, CUTEFISH_WINDOW_HINTS_V1_PLACEMENT_EDGE_TOP_LEFT,
                                    CUTEFISH_WINDOW_HINTS_V1_PLACEMENT_EDGE_BOTTOM_RIGHT}));
    EXPECT_EQ(hints[2].name, "unset_placement_anchor");
}

// A body shape goes out as the hint with its outline, and an empty one unsets
// it; the resize edges can be turned off.
TEST(WindowHintsProtocolTest, BodyShapeIsSentAndUnset)
{
    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);

    Target target;
    auto shell = std::make_unique<wayland::ShellSurface>(*connection, &target);
    Path bubble;
    bubble.moveTo(10, 0);
    bubble.lineTo(20, 10);
    bubble.lineTo(0, 10);
    bubble.close();
    shell->setBodyShape(bubble);
    shell->setBodyShape(Path());
    shell->setServerResize(false);
    connection->flush();
    settle(compositor, connection->display());

    // A toplevel's edges are the compositor's until it says otherwise.
    std::vector<FakeCompositor::Request> resize = compositor.hints();
    std::erase_if(resize, [](const FakeCompositor::Request& request) {
        return request.name != "set_server_resize";
    });
    ASSERT_EQ(resize.size(), 2u);
    EXPECT_EQ(resize[0].arguments, (std::vector<int64_t>{1}));
    EXPECT_EQ(resize[1].arguments, (std::vector<int64_t>{0}));

    std::vector<FakeCompositor::Request> hints = compositor.hints();
    std::erase_if(hints, [](const FakeCompositor::Request& request) {
        return request.name.find("body_shape") == std::string::npos;
    });
    ASSERT_EQ(hints.size(), 2u);
    EXPECT_EQ(hints[0].name, "set_body_shape");
    EXPECT_EQ(hints[1].name, "unset_body_shape");
}

// Opacity goes out as the hint; a toplevel places itself only where the
// compositor offers placement, rounded to whole logical pixels, and hears
// where it went.
TEST(WindowHintsProtocolTest, OpacityAndScreenPositionAreSent)
{
    {
        FakeCompositor compositor;
        base::EventLoop loop;
        std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
        ASSERT_TRUE(connection);
        EXPECT_FALSE(connection->windowPlacement());
        Target target;
        auto shell = std::make_unique<wayland::ShellSurface>(*connection, &target);
        EXPECT_FALSE(shell->setScreenPosition({10, 20}));
    }

    FakeCompositor compositor(true);
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);
    ASSERT_TRUE(connection->windowPlacement());
    Target target;
    auto shell = std::make_unique<wayland::ShellSurface>(*connection, &target);
    shell->setOpacity(0.5);
    EXPECT_TRUE(shell->setScreenPosition({10.4, 20.6}));
    connection->flush();
    settle(compositor, connection->display());

    std::vector<FakeCompositor::Request> requests = compositor.hints();
    std::erase_if(requests, [](const FakeCompositor::Request& request) {
        return request.name != "set_opacity" && request.name != "set_position";
    });
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].name, "set_opacity");
    EXPECT_EQ(requests[0].arguments, (std::vector<int64_t>{wl_fixed_from_double(0.5)}));
    EXPECT_EQ(requests[1].name, "set_position");
    EXPECT_EQ(requests[1].arguments, (std::vector<int64_t>{10, 21}));

    compositor.sendPosition(30, 40);
    for (int i = 0; i < 100 && target.positions.empty(); ++i)
        wl_display_roundtrip(connection->display());
    ASSERT_EQ(target.positions.size(), 1u);
    EXPECT_EQ(target.positions[0], (PointF{30, 40}));
}
