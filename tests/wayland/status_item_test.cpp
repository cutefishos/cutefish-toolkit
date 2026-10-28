// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wayland-client.h>
#include <wayland-server.h>

#include "cutefish-status-item-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include "base/event_loop.h"
#include "wayland/connection.h"
#include "wayland/shell_surface.h"

using namespace cutefish;
using namespace cutefish::ui;

namespace {

// The globals a status item and its popups need, on a thread of its own.
// Requests are written down as "interface.request" with their integer
// arguments and 0 or 1 for each object argument; events go out on request.
class FakeCompositor
{
public:
    struct Request
    {
        std::string name;
        std::vector<int64_t> arguments;
    };

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
        global(&xdg_wm_base_interface, 3);
        global(&wp_viewporter_interface, 1);
        global(&wp_fractional_scale_manager_v1_interface, 1);
        global(&cutefish_status_item_manager_v1_interface, 1);
        global(&wl_seat_interface, 5);
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

    std::vector<Request> requests()
    {
        std::lock_guard lock(m_mutex);
        return m_requests;
    }

    // On the compositor's thread.
    wl_resource* item() const { return m_item; }

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
        if (&interface == &cutefish_status_item_v1_interface)
            m_item = resource;
    }

    static int dispatch(const void*, void* object, uint32_t, const wl_message* message, wl_argument* arguments)
    {
        auto* resource = static_cast<wl_resource*>(object);
        auto* self = static_cast<FakeCompositor*>(wl_resource_get_user_data(resource));
        Request request{std::string(wl_resource_get_class(resource)) + "." + message->name, {}};
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
            else if (*c == 'o')
                request.arguments.push_back(arguments[index].o ? 1 : 0);
            ++index;
        }
        if (std::string(message->name) == "destroy")
            wl_resource_destroy(resource);
        std::lock_guard lock(self->m_mutex);
        self->m_requests.push_back(std::move(request));
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
    wl_resource* m_item = nullptr;
    int m_wakeFd = -1;
    wl_event_source* m_wakeSource = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop = false;
    std::mutex m_mutex;
    std::vector<std::function<void()>> m_tasks;
    std::vector<std::unique_ptr<Global>> m_globals;
    std::vector<Request> m_requests;
};

class Target final : public wayland::ShellSurface::Delegate
{
public:
    void configure(int width, int height, const WindowStates& states) override
    {
        configures.push_back({width, height});
        activated = states.activated;
    }
    void scaleChanged(double) override {}
    void closeRequested() override { ++closes; }
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
    void statusItemLimits(int width) override { maxWidth = width; }
    void statusItemAppearance(bool isDark, uint32_t color) override
    {
        dark = isDark;
        foreground = color;
    }
    void statusItemHighlight(bool isHighlighted) override { highlighted = isHighlighted; }
    void statusItemPlaced(const RectF& rect) override { place = rect; }

    std::vector<std::pair<int, int>> configures;
    bool activated = false;
    int closes = 0;
    int maxWidth = -1;
    bool dark = false;
    uint32_t foreground = 0;
    bool highlighted = false;
    RectF place;
};

// Waits until the compositor has read everything the client sent and the
// client has read everything the compositor sent.
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

std::vector<FakeCompositor::Request> only(const std::vector<FakeCompositor::Request>& requests,
                                          const std::string& prefix)
{
    std::vector<FakeCompositor::Request> matching;
    for (const FakeCompositor::Request& request : requests) {
        if (request.name.rfind(prefix, 0) == 0)
            matching.push_back(request);
    }
    return matching;
}

} // namespace

// The item is a role of its own, without an xdg_surface; the bar's events
// reach the delegate, and the commit answering a configure acknowledges it.
TEST(StatusItemProtocolTest, AnItemHearsTheBarAndAcknowledgesItsConfigure)
{
    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);
    ASSERT_TRUE(connection->statusItems());

    Target target;
    auto shell = std::make_unique<wayland::ShellSurface>(*connection, &target, wayland::ShellSurface::StatusItemRole {});
    EXPECT_TRUE(shell->isStatusItem());
    shell->setStatusItemIdentifier("clock");
    shell->setStatusItemHighlight(true);
    connection->flush();
    settle(compositor, connection->display());

    compositor.post([&compositor] {
        cutefish_status_item_v1_send_appearance(compositor.item(), CUTEFISH_STATUS_ITEM_V1_APPEARANCE_DARK, 0xffffffff);
        cutefish_status_item_v1_send_highlight(compositor.item(), 1);
        cutefish_status_item_v1_send_placed(compositor.item(), 700, 0, 40, 32);
        cutefish_status_item_v1_send_configure(compositor.item(), 7, 32, 300);
    });
    settle(compositor, connection->display());
    EXPECT_TRUE(target.dark);
    EXPECT_EQ(0xffffffffu, target.foreground);
    EXPECT_TRUE(target.highlighted);
    EXPECT_EQ((RectF{700, 0, 40, 32}), target.place);
    EXPECT_EQ(300, target.maxWidth);
    ASSERT_EQ(1u, target.configures.size());
    // The width is the item's own.
    EXPECT_EQ(0, target.configures.back().first);
    EXPECT_EQ(32, target.configures.back().second);
    EXPECT_FALSE(target.activated);

    shell->prepareCommit(40, 32, false);
    connection->flush();
    settle(compositor, connection->display());

    const std::vector<FakeCompositor::Request> requests = compositor.requests();
    EXPECT_TRUE(only(requests, "xdg_wm_base.get_xdg_surface").empty());
    ASSERT_EQ(1u, only(requests, "cutefish_status_item_manager_v1.get_status_item").size());
    const std::vector<FakeCompositor::Request> item = only(requests, "cutefish_status_item_v1.");
    ASSERT_EQ(3u, item.size());
    EXPECT_EQ("cutefish_status_item_v1.set_identifier", item[0].name);
    EXPECT_EQ("cutefish_status_item_v1.set_highlight", item[1].name);
    EXPECT_EQ((std::vector<int64_t>{1}), item[1].arguments);
    EXPECT_EQ("cutefish_status_item_v1.ack_configure", item[2].name);
    EXPECT_EQ((std::vector<int64_t>{7}), item[2].arguments);

    shell.reset();
    connection->flush();
    settle(compositor, connection->display());
    EXPECT_EQ("cutefish_status_item_v1.destroy", only(compositor.requests(), "cutefish_status_item_v1.").back().name);
}

// A popup of the item is made without an xdg parent, hung from the item, and
// takes its grab only then.
TEST(StatusItemProtocolTest, APopupHangsFromTheItemBeforeItsGrab)
{
    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);

    Target itemTarget;
    auto item = std::make_unique<wayland::ShellSurface>(*connection, &itemTarget,
                                                        wayland::ShellSurface::StatusItemRole {});
    Target popupTarget;
    PopupPlacement placement;
    placement.anchorRect = {0, 0, 40, 32};
    placement.anchor = PopupAnchor::Bottom;
    placement.gravity = PopupAnchor::Bottom;
    auto popup = std::make_unique<wayland::ShellSurface>(*connection, &popupTarget, item.get(), placement, 200,
                                                         120, true);
    connection->flush();
    settle(compositor, connection->display());

    std::vector<FakeCompositor::Request> requests = compositor.requests();
    std::erase_if(requests, [](const FakeCompositor::Request& request) {
        return request.name != "xdg_surface.get_popup" && request.name != "cutefish_status_item_v1.get_popup"
                && request.name != "xdg_popup.grab";
    });
    ASSERT_EQ(3u, requests.size());
    EXPECT_EQ("xdg_surface.get_popup", requests[0].name);
    // A null parent, then the positioner.
    EXPECT_EQ((std::vector<int64_t>{0, 1}), requests[0].arguments);
    EXPECT_EQ("cutefish_status_item_v1.get_popup", requests[1].name);
    EXPECT_EQ("xdg_popup.grab", requests[2].name);
}
