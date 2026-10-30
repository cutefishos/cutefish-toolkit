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

#include "cutefish-main-menu-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include "base/event_loop.h"
#include "ui/menu.h"
#include "wayland/connection.h"
#include "wayland/main_menu.h"

using namespace cutefish;
using namespace cutefish::ui;

namespace {

// The main menu global and those a connection needs, on a thread of its own. Requests are written down as
// "interface.request" with their integer arguments, a string argument as its
// own field and the object number for each object argument; every object the
// client makes is kept by its number, for events.
class FakeCompositor
{
public:
    struct Request
    {
        std::string name;
        uint32_t object = 0;
        std::vector<int64_t> arguments;
        std::string text;
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
        global(&cutefish_main_menu_manager_v1_interface, 1);
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

    void clearRequests()
    {
        std::lock_guard lock(m_mutex);
        m_requests.clear();
    }

    // On the compositor's thread.
    wl_resource* object(uint32_t id) const { return wl_client_get_object(m_client, id); }

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
    }

    static int dispatch(const void*, void* object, uint32_t, const wl_message* message, wl_argument* arguments)
    {
        auto* resource = static_cast<wl_resource*>(object);
        auto* self = static_cast<FakeCompositor*>(wl_resource_get_user_data(resource));
        Request request{std::string(wl_resource_get_class(resource)) + "." + message->name,
                        wl_resource_get_id(resource), {}, {}};
        int index = 0;
        for (const char* c = message->signature; *c; ++c) {
            if (*c == '?' || (*c >= '0' && *c <= '9'))
                continue;
            if (*c == 'n' && message->types[index]) {
                self->create(*message->types[index], wl_resource_get_version(resource), arguments[index].n);
                request.arguments.push_back(arguments[index].n);
            } else if (*c == 'i') {
                request.arguments.push_back(arguments[index].i);
            } else if (*c == 'u') {
                request.arguments.push_back(arguments[index].u);
            } else if (*c == 's') {
                request.text = arguments[index].s ? arguments[index].s : "";
            } else if (*c == 'o') {
                auto* argument = reinterpret_cast<wl_resource*>(arguments[index].o);
                request.arguments.push_back(argument ? wl_resource_get_id(argument) : 0);
            }
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
    int m_wakeFd = -1;
    wl_event_source* m_wakeSource = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop = false;
    std::mutex m_mutex;
    std::vector<std::function<void()>> m_tasks;
    std::vector<std::unique_ptr<Global>> m_globals;
    std::vector<Request> m_requests;
};

// Waits until the compositor has read everything the client sent and the
// client has read everything the compositor sent.
void settle(FakeCompositor& compositor, wayland::Connection& connection)
{
    connection.flush();
    auto done = std::make_shared<std::atomic<bool>>(false);
    compositor.post([done] { *done = true; });
    for (int i = 0; i < 100 && !*done; ++i)
        wl_display_roundtrip(connection.display());
    // The handlers of the events read here may send requests; the second
    // roundtrip has the compositor read them.
    wl_display_roundtrip(connection.display());
    wl_display_roundtrip(connection.display());
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

// The object made by the request, the first argument of create_menu and
// create_item.
uint32_t made(const FakeCompositor::Request& request)
{
    return uint32_t(request.arguments.at(0));
}

class MainMenuTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        connection = wayland::Connection::openFd(loop, compositor.clientFd);
        ASSERT_TRUE(connection);
        ASSERT_TRUE(connection->mainMenus());
    }

    void TearDown() override
    {
        connection.reset();
    }

    FakeCompositor compositor;
    base::EventLoop loop;
    std::unique_ptr<wayland::Connection> connection;
};

} // namespace

// Rows become items in order, separators as their kind, submenus as menus of
// their own with their role.
TEST_F(MainMenuTest, RowsBecomeItemsAndSubmenusMenus)
{
    Menu menu;
    Menu* application = menu.addSubmenu("Keka");
    application->setRole(MenuRole::Application);
    application->addItem("About Keka", [] {});
    application->addSeparator();
    const size_t quit = application->addItem("Quit Keka", [] {});
    application->setItemKeyEquivalent(quit, "q", ControlModifier);
    const size_t file = menu.addItem("File", [] {});
    menu.setItemEnabled(file, false);

    std::shared_ptr<wayland::MainMenuMirror> mirror = connection->mainMenu(&menu);
    ASSERT_TRUE(mirror);
    settle(compositor, *connection);

    const std::vector<FakeCompositor::Request> requests = compositor.requests();
    const std::vector<FakeCompositor::Request> menus = only(requests, "cutefish_main_menu_manager_v1.create_menu");
    ASSERT_EQ(2u, menus.size());
    const uint32_t root = made(menus[0]);
    const uint32_t submenu = made(menus[1]);

    const std::vector<FakeCompositor::Request> items = only(requests, "cutefish_main_menu_manager_v1.create_item");
    ASSERT_EQ(5u, items.size());
    // The root's first row, then the submenu's rows, then the root's second.
    EXPECT_EQ(CUTEFISH_MENU_ITEM_V1_KIND_NORMAL, items[0].arguments[1]);
    EXPECT_EQ(CUTEFISH_MENU_ITEM_V1_KIND_SEPARATOR, items[2].arguments[1]);

    const std::vector<FakeCompositor::Request> roles = only(requests, "cutefish_menu_v1.set_role");
    ASSERT_EQ(1u, roles.size());
    EXPECT_EQ(submenu, roles[0].object);
    EXPECT_EQ(CUTEFISH_MENU_V1_ROLE_APPLICATION, roles[0].arguments[0]);

    std::vector<std::string> titles;
    for (const FakeCompositor::Request& request : only(requests, "cutefish_menu_item_v1.set_title"))
        titles.push_back(request.text);
    EXPECT_EQ((std::vector<std::string>{"Keka", "About Keka", "Quit Keka", "File"}), titles);

    std::vector<FakeCompositor::Request> inserts = only(requests, "cutefish_menu_v1.insert_item");
    std::erase_if(inserts, [root](const FakeCompositor::Request& request) { return request.object != root; });
    ASSERT_EQ(2u, inserts.size());
    EXPECT_EQ(made(items[0]), inserts[0].arguments[0]);
    EXPECT_EQ(0, inserts[0].arguments[1]);
    EXPECT_EQ(made(items[4]), inserts[1].arguments[0]);
    EXPECT_EQ(1, inserts[1].arguments[1]);

    const std::vector<FakeCompositor::Request> submenus = only(requests, "cutefish_menu_item_v1.set_submenu");
    ASSERT_EQ(1u, submenus.size());
    EXPECT_EQ(made(items[0]), submenus[0].object);
    EXPECT_EQ(submenu, submenus[0].arguments[0]);

    std::vector<FakeCompositor::Request> enabled = only(requests, "cutefish_menu_item_v1.set_enabled");
    std::erase_if(enabled, [&](const FakeCompositor::Request& request) { return request.object != made(items[4]); });
    ASSERT_EQ(1u, enabled.size());
    EXPECT_EQ(0, enabled[0].arguments[0]);

    std::vector<FakeCompositor::Request> keys = only(requests, "cutefish_menu_item_v1.set_key_equivalent");
    std::erase_if(keys, [&](const FakeCompositor::Request& request) { return request.object != made(items[3]); });
    ASSERT_EQ(1u, keys.size());
    EXPECT_EQ("q", keys[0].text);
    EXPECT_EQ(CUTEFISH_MENU_ITEM_V1_MODIFIER_CONTROL, keys[0].arguments[0]);

    // One export for every window showing the menu.
    EXPECT_EQ(mirror, connection->mainMenu(&menu));
}

// A changed row sends only what changed, on the next loop turn; a new row
// sends the rows again.
TEST_F(MainMenuTest, ChangesFollowOnTheNextLoopTurn)
{
    Menu menu;
    const size_t open = menu.addItem("Open", [] {});
    menu.addItem("Close", [] {});
    std::shared_ptr<wayland::MainMenuMirror> mirror = connection->mainMenu(&menu);
    settle(compositor, *connection);
    const uint32_t openItem = made(only(compositor.requests(), "cutefish_main_menu_manager_v1.create_item")[0]);
    compositor.clearRequests();

    menu.setItemText(open, "Open…");
    menu.setItemState(open, MenuItemState::Mixed);
    settle(compositor, *connection);
    EXPECT_TRUE(compositor.requests().empty());

    loop.dispatch();
    settle(compositor, *connection);
    std::vector<FakeCompositor::Request> requests = compositor.requests();
    ASSERT_EQ(2u, requests.size());
    EXPECT_EQ("cutefish_menu_item_v1.set_title", requests[0].name);
    EXPECT_EQ(openItem, requests[0].object);
    EXPECT_EQ("Open…", requests[0].text);
    EXPECT_EQ("cutefish_menu_item_v1.set_state", requests[1].name);
    EXPECT_EQ(CUTEFISH_MENU_ITEM_V1_STATE_MIXED, requests[1].arguments[0]);
    compositor.clearRequests();

    menu.addSeparator();
    loop.dispatch();
    settle(compositor, *connection);
    requests = compositor.requests();
    EXPECT_EQ(2u, only(requests, "cutefish_menu_item_v1.destroy").size());
    EXPECT_EQ(3u, only(requests, "cutefish_main_menu_manager_v1.create_item").size());
    EXPECT_EQ(3u, only(requests, "cutefish_menu_v1.insert_item").size());
}

// Before the bar opens a menu, its onAboutToShow runs and what it adds is
// sent before the menu says it is ready.
TEST_F(MainMenuTest, AMenuIsFilledBeforeItIsReady)
{
    Menu menu;
    Menu* windows = menu.addSubmenu("Window");
    int shown = 0;
    windows->onAboutToShow = [&] {
        ++shown;
        windows->clear();
        windows->addItem("Document 1", [] {});
    };
    int closed = 0;
    windows->onClosed = [&] { ++closed; };
    std::shared_ptr<wayland::MainMenuMirror> mirror = connection->mainMenu(&menu);
    settle(compositor, *connection);
    const uint32_t submenu = made(only(compositor.requests(), "cutefish_main_menu_manager_v1.create_menu")[1]);
    compositor.clearRequests();

    compositor.post([&] { cutefish_menu_v1_send_will_open(compositor.object(submenu), 42); });
    settle(compositor, *connection);
    EXPECT_EQ(1, shown);
    std::vector<FakeCompositor::Request> requests = compositor.requests();
    ASSERT_FALSE(requests.empty());
    EXPECT_EQ("cutefish_menu_v1.ready", requests.back().name);
    EXPECT_EQ(submenu, requests.back().object);
    EXPECT_EQ(42, requests.back().arguments[0]);
    std::vector<FakeCompositor::Request> titles = only(requests, "cutefish_menu_item_v1.set_title");
    ASSERT_EQ(1u, titles.size());
    EXPECT_EQ("Document 1", titles[0].text);

    compositor.post([&] { cutefish_menu_v1_send_closed(compositor.object(submenu)); });
    settle(compositor, *connection);
    EXPECT_EQ(1, closed);
}

// Choosing an item runs its row's action; a disabled row's does not run.
TEST_F(MainMenuTest, ActivatingAnItemRunsItsAction)
{
    Menu menu;
    int opened = 0;
    int closed = 0;
    menu.addItem("Open", [&] { ++opened; });
    const size_t close = menu.addItem("Close", [&] { ++closed; });
    menu.setItemEnabled(close, false);
    std::shared_ptr<wayland::MainMenuMirror> mirror = connection->mainMenu(&menu);
    settle(compositor, *connection);
    const std::vector<FakeCompositor::Request> items =
        only(compositor.requests(), "cutefish_main_menu_manager_v1.create_item");
    ASSERT_EQ(2u, items.size());

    compositor.post([&] {
        cutefish_menu_item_v1_send_activated(compositor.object(made(items[0])), 41);
        cutefish_menu_item_v1_send_activated(compositor.object(made(items[1])), 42);
    });
    settle(compositor, *connection);
    EXPECT_EQ(1, opened);
    EXPECT_EQ(0, closed);
    // The choice counts as input, so the clipboard may be set after it.
    EXPECT_EQ(42u, connection->inputSerial());
}

// The export goes with the last window showing it, and with it its objects.
TEST_F(MainMenuTest, TheLastHolderDestroysTheExport)
{
    auto menu = std::make_unique<Menu>();
    menu->addSubmenu("File")->addItem("Open", [] {});
    std::shared_ptr<wayland::MainMenuMirror> mirror = connection->mainMenu(menu.get());
    settle(compositor, *connection);
    compositor.clearRequests();

    mirror.reset();
    settle(compositor, *connection);
    const std::vector<FakeCompositor::Request> requests = compositor.requests();
    EXPECT_EQ(2u, only(requests, "cutefish_menu_v1.destroy").size());
    EXPECT_EQ(2u, only(requests, "cutefish_menu_item_v1.destroy").size());

    // A menu destroyed before its export leaves the export nothing to touch.
    mirror = connection->mainMenu(menu.get());
    EXPECT_TRUE(mirror->isAlive());
    menu.reset();
    EXPECT_FALSE(mirror->isAlive());
    loop.dispatch();
    mirror.reset();
    settle(compositor, *connection);
}
