// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <unistd.h>

#include "base/application_registry.h"
#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/file.h"
#include "base/mime_apps.h"
#include "base/mime_database.h"
#include "base/paths.h"
#include "base/timer.h"

using namespace cutefish::base;
namespace fs = std::filesystem;

namespace {

class MimeAppsTest : public testing::Test
{
protected:
    void SetUp() override
    {
        m_directory = fs::temp_directory_path() / ("cutefish-mimeapps-" + std::to_string(getpid()));
        fs::create_directories(m_directory);
    }
    void TearDown() override { fs::remove_all(m_directory); }

    std::string path(const std::string& name) const { return (m_directory / name).string(); }

    fs::path m_directory;
};

} // namespace

TEST(MimeDatabaseTest, ResolvesAliasesAndParents)
{
    // shared-mime-info has these on every system the desktop runs on.
    EXPECT_EQ(mime::canonicalName("application/x-pdf"), "application/pdf");
    EXPECT_EQ(mime::canonicalName("application/pdf"), "application/pdf");
    EXPECT_EQ(mime::canonicalName("no/such-type"), "");
    const std::vector<std::string> aliases = mime::aliases("application/pdf");
    EXPECT_NE(std::find(aliases.begin(), aliases.end(), "application/x-pdf"), aliases.end());
    EXPECT_EQ(mime::parents("text/x-csrc"), std::vector<std::string>{"text/plain"});
    // Without parents of its own: application/octet-stream, the fallback.
    EXPECT_EQ(mime::parents("image/png"), std::vector<std::string>{"application/octet-stream"});
    EXPECT_TRUE(mime::parents("inode/directory").empty());
    EXPECT_TRUE(mime::parents("application/octet-stream").empty());
}

TEST_F(MimeAppsTest, ListsLiveInTheConfigHome)
{
    EXPECT_EQ(mimeapps::userListPath(), paths::configHome() + "/mimeapps.list");
    const std::vector<std::string> lists = mimeapps::listPaths();
    ASSERT_FALSE(lists.empty());
    EXPECT_TRUE(lists.front().starts_with(paths::configHome()));
    EXPECT_TRUE(lists.front().ends_with("-mimeapps.list"));
}

TEST_F(MimeAppsTest, SetsDefaultAndUpdatesAssociations)
{
    const std::string list = path("mimeapps.list");
    ASSERT_TRUE(file::writeAtomically(list, "# kept\n[Default Applications]\ntext/plain=old.desktop;\n"
                                            "[Added Associations]\ntext/plain=old.desktop;new.desktop;\n"
                                            "[Removed Associations]\ntext/plain=new.desktop;other.desktop;\n"));
    ASSERT_TRUE(mimeapps::setDefault(list, "text/plain", "new.desktop"));
    // What MimeApps::setDefault in cutefish-framework writes, byte for byte: the
    // file's last line break makes an empty line, which the group keeps.
    EXPECT_EQ(file::read(list).value_or(""),
              "# kept\n[Default Applications]\ntext/plain=new.desktop;\n"
              "[Added Associations]\ntext/plain=new.desktop;old.desktop;\n"
              "[Removed Associations]\n\ntext/plain=other.desktop;\n");
    EXPECT_EQ(mimeapps::associations(list, "Added Associations", "text/plain"),
              (std::vector<std::string>{"new.desktop", "old.desktop"}));
    EXPECT_FALSE(mimeapps::setDefault(list, "text/plain", "bad;id"));
    EXPECT_FALSE(fs::exists(list + ".lock"));
}

TEST_F(MimeAppsTest, NamesBundlesByPath)
{
    DesktopEntry entry;
    entry.id = "org.example.editor";
    entry.path = "/usr/share/applications/org.example.editor.desktop";
    EXPECT_EQ(mimeapps::listedId(entry), "org.example.editor.desktop");

    entry.id = entry.path = "/home/user/Applications/Editor.app";
    entry.bundle = true;
    EXPECT_EQ(mimeapps::listedId(entry), "/home/user/Applications/Editor.app");

    const std::string list = path("mimeapps.list");
    ASSERT_TRUE(mimeapps::setDefault(list, "text/plain", mimeapps::listedId(entry)));
    EXPECT_EQ(mimeapps::associations(list, "Default Applications", "text/plain"),
              std::vector<std::string>{"/home/user/Applications/Editor.app"});
}

TEST_F(MimeAppsTest, KeepsTheTerminalList)
{
    const std::string list = path("xdg-terminals.list");
    ASSERT_TRUE(file::writeAtomically(list, "# preferred\n-excluded.desktop\nfirst.desktop:new-window\nsecond.desktop\n"));
    EXPECT_EQ(mimeapps::defaultTerminal(list), "first.desktop");
    ASSERT_TRUE(mimeapps::setDefaultTerminal(list, "second.desktop"));
    EXPECT_EQ(file::read(list).value_or(""),
              "second.desktop\n# preferred\n-excluded.desktop\nfirst.desktop:new-window\n");
    EXPECT_EQ(mimeapps::defaultTerminal(list), "second.desktop");
}

// Reference types measured on the laptop, for these files.
TEST(MimeTest, DetectsTypesOfFiles)
{
    const fs::path directory = fs::temp_directory_path() / ("cutefish-mime-detect-" + std::to_string(getpid()));
    fs::create_directories(directory / "folder");
    const auto write = [&directory](const std::string& name, std::string_view contents) {
        ASSERT_TRUE(file::writeAtomically((directory / name).string(), contents));
    };
    write("notes.txt", "hello\nworld\n");
    write("readme_noext", "plain words\n");
    write("blob", std::string_view("\0\1\2\3binary", 10));
    write("picture", std::string_view("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16));
    write("picture.txt", std::string_view("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16));
    write("empty", "");
    write("code.C", "int main() {}\n");
    write("code.c", "int main() {}\n");
    write("lang.ts", "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE TS>\n<TS version=\"2.1\">\n</TS>\n");
    write("video.ts", std::string_view("G@\0\x10\0\0", 6));
    write("app.desktop", "[Desktop Entry]\nType=Application\n");
    write("doc", "%PDF-1.4\n");
    write("script", "#!/bin/sh\necho hi\n");
    write("Makefile", "all:\n");

    struct Expected
    {
        const char* name;
        const char* type;
        const char* icon;
        const char* genericIcon;
    };
    const Expected expected[] = {
        {"Makefile", "text/x-makefile", "text-x-makefile", "text-x-generic"},
        {"app.desktop", "application/x-desktop", "application-x-desktop", "text-x-generic"},
        {"blob", "application/octet-stream", "application-octet-stream", "application-x-generic"},
        {"code.C", "text/x-c++src", "text-x-c++src", "text-x-generic"},
        {"code.c", "text/x-csrc", "text-x-csrc", "text-x-generic"},
        {"doc", "application/pdf", "application-pdf", "x-office-document"},
        {"empty", "application/x-zerosize", "application-x-zerosize", "application-x-generic"},
        {"folder", "inode/directory", "inode-directory", "folder"},
        {"lang.ts", "text/vnd.trolltech.linguist", "text-vnd.trolltech.linguist", "text-x-generic"},
        {"notes.txt", "text/plain", "text-plain", "text-x-generic"},
        {"picture", "image/png", "image-png", "image-x-generic"},
        {"picture.txt", "text/plain", "text-plain", "text-x-generic"},
        {"readme_noext", "text/x-readme", "text-x-readme", "text-x-generic"},
        {"script", "application/x-shellscript", "application-x-shellscript", "text-x-script"},
        {"video.ts", "text/vnd.trolltech.linguist", "text-vnd.trolltech.linguist", "text-x-generic"},
    };
    for (const Expected& entry : expected) {
        const std::string type = mime::typeForFile((directory / entry.name).string());
        EXPECT_EQ(type, entry.type) << entry.name;
        EXPECT_EQ(mime::iconName(type), entry.icon) << entry.name;
        EXPECT_EQ(mime::genericIconName(type), entry.genericIcon) << entry.name;
    }
    fs::remove_all(directory);

    EXPECT_EQ(mime::typeForFileName("a.tar.gz"), "application/x-compressed-tar");
    EXPECT_EQ(mime::typeForFileName("Makefile"), "text/x-makefile");
    EXPECT_EQ(mime::typeForFileName("x.C"), "text/x-c++src");
    EXPECT_EQ(mime::typeForFileName("x.c"), "text/x-csrc");
    EXPECT_EQ(mime::typeForFileName("X.PNG"), "image/png");
    EXPECT_EQ(mime::typeForFileName("foo"), "application/octet-stream");
    EXPECT_EQ(mime::typeForFileName("a.ts"), "text/vnd.trolltech.linguist");
    EXPECT_TRUE(mime::inherits("text/x-csrc", "text/plain"));
    EXPECT_FALSE(mime::inherits("text/plain", "text/x-csrc"));
}

// A link opens in what the lists choose for its scheme, which the MIME
// database does not know as a type; a scheme nothing takes opens nothing.
// Without the application runtime the command starts here.
TEST_F(MimeAppsTest, OpensAUriWithWhatTakesItsScheme)
{
    EventLoop loop;
    fs::create_directories(path("applications"));
    const std::string out = path("opened");
    const std::string script = path("handler.sh");
    std::ofstream(script) << "#!/bin/sh\nprintf '%s' \"$1\" > " << out << ".part && mv " << out << ".part " << out << "\n";
    fs::permissions(script, fs::perms::owner_all);
    std::ofstream(path("applications/handler.desktop"))
            << "[Desktop Entry]\nType=Application\nName=Handler\nExec=" << script << " %u\n";
    std::ofstream(path("mimeapps.list")) << "[Default Applications]\nx-scheme-handler/https=handler.desktop\n";
    ApplicationRegistry::setInstance(loop, ApplicationRegistry::create(loop, {path("applications")}));

    std::vector<bool> results;
    const auto opened = [&](bool started) {
        results.push_back(started);
        loop.quit();
    };
    Timer timeout(&loop);
    timeout.setSingleShot(true);
    timeout.onTimeout = [&loop] { loop.quit(); };
    mimeapps::openUri("HTTPS://cutefish.org/a b", {}, opened, {path("mimeapps.list")});
    timeout.start(std::chrono::seconds(5));
    loop.run();
    mimeapps::openUri("gopher://cutefish.org", {}, opened, {path("mimeapps.list")});
    mimeapps::openUri("not a uri", {}, opened, {path("mimeapps.list")});
    ASSERT_EQ(results, (std::vector<bool> {true, false, false}));
    for (int i = 0; i < 500 && !fs::exists(out); ++i)
        usleep(10000);
    EXPECT_EQ(file::read(out), "HTTPS://cutefish.org/a b");
}

// With the application runtime on the bus, it starts the handler, given the
// entry, the URI and the activation token.
TEST_F(MimeAppsTest, OpensAUriThroughTheApplicationRuntime)
{
    EventLoop loop;
    std::unique_ptr<dbus::Connection> runtimeBus = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);
    ASSERT_TRUE(runtimeBus);
    std::string appId;
    std::vector<std::string> arguments;
    std::string token;
    dbus::Object runtime(runtimeBus.get(), "/AppRuntime", "org.cutefish.AppRuntime");
    runtime.addMethod(
            "launchApplication",
            [&](const std::string& id, const std::vector<std::string>& args, const std::string& activationToken) {
                appId = id;
                arguments = args;
                token = activationToken;
                return uint32_t(4242);
            },
            dbus::ArgumentNames {{"appId", "arguments", "activationToken"}, {"pid"}});
    runtime.publish();
    ASSERT_TRUE(runtimeBus->requestName("org.cutefish.AppRuntime"));

    fs::create_directories(path("applications"));
    std::ofstream(path("applications/handler.desktop"))
            << "[Desktop Entry]\nType=Application\nName=Handler\nExec=" << path("never-run") << " %u\n";
    std::ofstream(path("mimeapps.list")) << "[Default Applications]\nx-scheme-handler/https=handler.desktop\n";
    ApplicationRegistry::setInstance(loop, ApplicationRegistry::create(loop, {path("applications")}));

    std::optional<bool> started;
    Timer timeout(&loop);
    timeout.setSingleShot(true);
    timeout.onTimeout = [&loop] { loop.quit(); };
    mimeapps::openUri("https://cutefish.org/", "token-1", [&](bool ok) {
        started = ok;
        loop.quit();
    }, {path("mimeapps.list")});
    timeout.start(std::chrono::seconds(5));
    loop.run();
    EXPECT_EQ(started, true);
    EXPECT_EQ(appId, path("applications/handler.desktop"));
    EXPECT_EQ(arguments, std::vector<std::string> {"https://cutefish.org/"});
    EXPECT_EQ(token, "token-1");
}
