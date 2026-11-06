// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wayland-client.h>
#include <wayland-server.h>

#include "cutefish-window-hints-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include "base/event_loop.h"
#include "wayland/connection.h"

using namespace cutefish;
using namespace cutefish::ui;

namespace {

// The globals a Connection needs and one output with xdg-output version 3,
// whose updates end with wl_output.done alone, run on a thread of its own.
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
        wl_global_create(m_display, &wl_output_interface, 3, this, bindOutput);
        wl_global_create(m_display, &zxdg_output_manager_v1_interface, 3, this, bindOutputManager);
        wl_global_create(m_display, &cutefish_window_hints_v1_interface, 1, this, bindWindowHints);
        wl_client_create(m_display, fds[0]);
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

    // Sends the output's logical size, as a scale change does.
    void resizeOutput(int width, int height)
    {
        post([this, width, height] {
            m_width = width;
            m_height = height;
            for (wl_resource* xdgOutput : m_xdgOutputs)
                zxdg_output_v1_send_logical_size(xdgOutput, width, height);
            for (wl_resource* output : m_outputs) {
                wl_output_send_scale(output, 1);
                wl_output_send_done(output);
            }
        });
    }

    // cutefish_window_hints_v1.work_area for the output.
    void sendWorkArea(int x, int y, int width, int height, bool primary)
    {
        post([this, x, y, width, height, primary] {
            for (wl_resource* hints : m_windowHints)
                cutefish_window_hints_v1_send_work_area(hints, "eDP-1", x, y, width, height, primary ? 1 : 0);
        });
    }

    int clientFd = -1;

private:
    void global(const wl_interface* interface, int version)
    {
        wl_global_create(m_display, interface, version, const_cast<wl_interface*>(interface), bindIgnored);
    }

    // Requests on these are never answered; the connection makes none that
    // need an object back while the test runs.
    static int ignore(const void*, void*, uint32_t, const wl_message*, wl_argument*) { return 0; }

    static void bindIgnored(wl_client* client, void* data, uint32_t version, uint32_t id)
    {
        wl_resource* resource = wl_resource_create(client, static_cast<const wl_interface*>(data), int(version), id);
        wl_resource_set_dispatcher(resource, ignore, nullptr, nullptr, nullptr);
    }

    static void bindOutput(wl_client* client, void* data, uint32_t version, uint32_t id)
    {
        auto* self = static_cast<FakeCompositor*>(data);
        wl_resource* output = wl_resource_create(client, &wl_output_interface, int(version), id);
        wl_resource_set_dispatcher(output, ignore, nullptr, nullptr, nullptr);
        self->m_outputs.push_back(output);
        wl_output_send_geometry(output, 0, 0, 300, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN, "", "", WL_OUTPUT_TRANSFORM_NORMAL);
        wl_output_send_mode(output, WL_OUTPUT_MODE_CURRENT, 2160, 1440, 60000);
        wl_output_send_scale(output, 2);
        wl_output_send_done(output);
    }

    static int dispatchOutputManager(const void*, void* object, uint32_t opcode, const wl_message*,
                                     wl_argument* arguments)
    {
        auto* manager = static_cast<wl_resource*>(object);
        auto* self = static_cast<FakeCompositor*>(wl_resource_get_user_data(manager));
        // get_xdg_output; 0 is destroy.
        if (opcode != 1)
            return 0;
        wl_resource* xdgOutput = wl_resource_create(wl_resource_get_client(manager), &zxdg_output_v1_interface,
                                                    wl_resource_get_version(manager), arguments[0].n);
        wl_resource_set_dispatcher(xdgOutput, ignore, nullptr, nullptr, nullptr);
        self->m_xdgOutputs.push_back(xdgOutput);
        zxdg_output_v1_send_logical_position(xdgOutput, 0, 0);
        zxdg_output_v1_send_logical_size(xdgOutput, self->m_width, self->m_height);
        zxdg_output_v1_send_name(xdgOutput, "eDP-1");
        for (wl_resource* output : self->m_outputs)
            wl_output_send_done(output);
        return 0;
    }

    static void bindWindowHints(wl_client* client, void* data, uint32_t version, uint32_t id)
    {
        auto* self = static_cast<FakeCompositor*>(data);
        wl_resource* hints = wl_resource_create(client, &cutefish_window_hints_v1_interface, int(version), id);
        wl_resource_set_dispatcher(hints, ignore, nullptr, nullptr, nullptr);
        self->m_windowHints.push_back(hints);
    }

    static void bindOutputManager(wl_client* client, void* data, uint32_t version, uint32_t id)
    {
        wl_resource* manager = wl_resource_create(client, &zxdg_output_manager_v1_interface, int(version), id);
        wl_resource_set_dispatcher(manager, dispatchOutputManager, nullptr, data, nullptr);
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
    int m_wakeFd = -1;
    wl_event_source* m_wakeSource = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop = false;
    std::mutex m_mutex;
    std::vector<std::function<void()>> m_tasks;
    std::vector<wl_resource*> m_outputs;
    std::vector<wl_resource*> m_xdgOutputs;
    std::vector<wl_resource*> m_windowHints;
    int m_width = 1080;
    int m_height = 720;
};

} // namespace

// wl_output.done closes an xdg-output v3 update: the connection reports it
// with the new logical size, as after a scale change.
TEST(OutputTest, ChangesAreReportedOnOutputDone)
{
    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);
    wl_display_roundtrip(connection->display());
    std::vector<wayland::Connection::OutputInfo> outputs = connection->outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs[0].name, "eDP-1");
    EXPECT_EQ(outputs[0].logicalSize, (SizeF{1080, 720}));

    int changes = 0;
    SizeF reported;
    connection->onOutputsChanged = [&] {
        ++changes;
        reported = connection->outputs()[0].logicalSize;
    };
    compositor.resizeOutput(2160, 1440);
    for (int i = 0; i < 100 && changes == 0; ++i)
        wl_display_roundtrip(connection->display());
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(reported, (SizeF{2160, 1440}));
}

// An output's scale comes from its mode over its logical size, and its work
// area and primary flag from the window hints, the whole output until then.
TEST(OutputTest, ScaleAndWorkAreaReachTheOutputs)
{
    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);
    wl_display_roundtrip(connection->display());
    std::vector<wayland::Connection::OutputInfo> outputs = connection->outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_DOUBLE_EQ(outputs[0].scale, 2);
    EXPECT_EQ(outputs[0].workArea, (RectF{0, 0, 1080, 720}));
    EXPECT_FALSE(outputs[0].primary);

    int changes = 0;
    connection->onOutputsChanged = [&] {
        ++changes;
    };
    compositor.sendWorkArea(0, 28, 1080, 692, true);
    for (int i = 0; i < 100 && changes == 0; ++i)
        wl_display_roundtrip(connection->display());
    EXPECT_EQ(changes, 1);
    outputs = connection->outputs();
    EXPECT_EQ(outputs[0].workArea, (RectF{0, 28, 1080, 692}));
    EXPECT_TRUE(outputs[0].primary);
}

// At a fractional scale the logical size is rounded to whole units; the
// scale is still the one the compositor uses, in 120ths.
TEST(OutputTest, FractionalScaleSurvivesTheRoundedLogicalSize)
{
    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);
    wl_display_roundtrip(connection->display());
    int changes = 0;
    connection->onOutputsChanged = [&] {
        ++changes;
    };
    // 2160 pixels at 1.75 are 1234.3 units.
    compositor.resizeOutput(1234, 823);
    for (int i = 0; i < 100 && changes == 0; ++i)
        wl_display_roundtrip(connection->display());
    const std::vector<wayland::Connection::OutputInfo> outputs = connection->outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_DOUBLE_EQ(outputs[0].scale, 1.75);
}

// The adopted descriptor is close-on-exec: a program the trusted client
// starts must not inherit its privileged connection.
TEST(OutputTest, AdoptedConnectionIsNotInherited)
{
    FakeCompositor compositor;
    fcntl(compositor.clientFd, F_SETFD, 0);
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection = wayland::Connection::openFd(loop, compositor.clientFd);
    ASSERT_TRUE(connection);
    const int fd = wl_display_get_fd(connection->display());
    EXPECT_TRUE(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    // system() forks and execs a shell, which keeps whatever is inherited.
    const std::string command = "test -e /proc/self/fd/" + std::to_string(fd);
    EXPECT_NE(std::system(command.c_str()), 0);
}
