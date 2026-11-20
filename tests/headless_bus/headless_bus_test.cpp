// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "base/dbus.h"
#include "ui/application.h"

using namespace cutefish;

// A headless application asked for the session bus has the loop's shared
// connection, which reaches the bus the process was started on.
TEST(HeadlessSessionBusTest, TheApplicationHasTheSessionBus)
{
    ui::Application* application = ui::Application::instance();
    ASSERT_TRUE(application);
    base::dbus::Connection* bus = application->sessionBus();
    ASSERT_TRUE(bus);
    base::dbus::Message message =
            bus->newMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetId");
    const base::dbus::Reply reply = bus->call(message, std::chrono::seconds(5));
    std::string id;
    EXPECT_FALSE(reply.isError());
    EXPECT_TRUE(reply.read(id));
    EXPECT_FALSE(id.empty());
}

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    ui::Application::setHeadlessSessionBus(true);
    ui::Application application(argc, argv, ui::Application::Platform::Headless);
    return RUN_ALL_TESTS();
}
