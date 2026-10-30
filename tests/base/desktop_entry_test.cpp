// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>

#include "base/application_registry.h"
#include "base/desktop_entry.h"
#include "base/event_loop.h"
#include "base/file.h"

using namespace cutefish::base;
using namespace std::chrono_literals;

static constexpr const char* kEntry = R"(# A comment
[Desktop Entry]
Type=Application
Name=Text Editor
Name[de]=Texteditor
Name[sr@latin]=Uređivač
Comment=Edit text\sfiles
Icon=accessories-text-editor
Exec=editor --new-window %F --icon=%i --name "%c" 'quoted arg'
Categories=Utility;TextEditor;
Keywords=text;editor;
NotShowIn=Other;
Actions=new;

[Desktop Action new]
Name=New Window
Exec=editor --new
)";

TEST(DesktopEntryTest, ParsesLocalizedEntries)
{
    const auto entry = DesktopEntry::parse("org.example.editor", "/x/org.example.editor.desktop", kEntry, "de_DE");
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->name, "Texteditor");
    EXPECT_EQ(entry->comment, "Edit text files");
    EXPECT_EQ(entry->categories, (std::vector<std::string>{"Utility", "TextEditor"}));
    ASSERT_EQ(entry->actions.size(), 1u);
    EXPECT_EQ(entry->actions[0].name, "New Window");
    EXPECT_EQ(DesktopEntry::parse("a", "a", kEntry, "sr_RS@latin")->name, "Uređivač");
    EXPECT_EQ(DesktopEntry::parse("a", "a", kEntry, "en_US")->name, "Text Editor");
    EXPECT_TRUE(entry->shouldShow());
    EXPECT_FALSE(entry->shouldShow({"Other"}));
    EXPECT_TRUE(entry->startupNotify);
    const char* quiet = "[Desktop Entry]\nType=Application\nName=x\nStartupNotify=false\n";
    EXPECT_FALSE(DesktopEntry::parse("a", "a", quiet, "en_US")->startupNotify);

    EXPECT_FALSE(DesktopEntry::parse("a", "a", "[Desktop Entry]\nType=Link\nName=x\n", "en_US"));
    EXPECT_TRUE(DesktopEntry::parse("a", "a", "[Desktop Entry]\nType=Application\nHidden=true\n", "en_US"));
}

TEST(DesktopEntryTest, ExpandsFieldCodes)
{
    const auto entry = DesktopEntry::parse("e", "/x/e.desktop", kEntry, "en_US");
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->command(), (std::vector<std::string>{"editor", "--new-window", "--icon=", "--name",
                                                          "Text Editor", "quoted arg"}));
    EXPECT_EQ(entry->commandForArguments({"/a", "/b"}),
              (std::vector<std::string>{"editor", "--new-window", "/a", "/b", "--icon=", "--name", "Text Editor",
                                        "quoted arg"}));
    EXPECT_EQ(DesktopEntry::parseExec("app %u %i"), std::vector<std::string>{"app"});
}

TEST(ApplicationRegistryTest, ScansAndResolves)
{
    EventLoop loop;
    ApplicationRegistry& registry = ApplicationRegistry::instance(&loop);
    EXPECT_TRUE(registry.isLoading());
    Subscription subscription = registry.changed.connect([&loop] { loop.quit(); });
    const int timer = loop.startTimer(5s, false, [&loop] { loop.quit(); });
    loop.run();
    loop.stopTimer(timer);
    EXPECT_FALSE(registry.isLoading());
    ASSERT_FALSE(registry.entries().empty());

    const auto& first = registry.entries().front();
    EXPECT_EQ(registry.byId(first->id), first);
    EXPECT_EQ(registry.byPath(first->path), first);
    if (const auto settings = registry.byId("cutefish-settings")) {
        EXPECT_EQ(registry.resolveWindow("", "", "/usr/bin/cutefish-settings", "cutefish-settings"), settings);
        EXPECT_EQ(registry.resolveWindow("cutefish-settings", "", "", ""), settings);
    }
}

// Runs the loop until the registry has read its directories again.
static bool waitForScan(EventLoop& loop, ApplicationRegistry& registry, std::chrono::milliseconds timeout)
{
    bool scanned = false;
    Subscription subscription = registry.changed.connect([&] {
        scanned = true;
        loop.quit();
    });
    const int timer = loop.startTimer(timeout, false, [&loop] { loop.quit(); });
    loop.run();
    loop.stopTimer(timer);
    return scanned;
}

static std::string launcherEntry(const std::string& name, const std::string& windowClass)
{
    return "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=launcher \"/opt/" + name + "/" + name
        + "\" %U\nStartupWMClass=" + windowClass + "\nNoDisplay=true\n";
}

TEST(ApplicationRegistryTest, NamesAWindowOfASharedLauncherByItsAppId)
{
    file::TemporaryDirectory directory(std::filesystem::temp_directory_path().string(), "cutefish-test-");
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(file::writeAtomically(directory.path() + "/org.example.Clip.desktop",
                                      launcherEntry("Clip", "org.example.Clip")));
    ASSERT_TRUE(file::writeAtomically(directory.path() + "/org.example.Edit.desktop",
                                      launcherEntry("Edit", "org.example.Edit")));

    EventLoop loop;
    std::unique_ptr<ApplicationRegistry> registry = ApplicationRegistry::create(loop, {directory.path()});
    ASSERT_TRUE(waitForScan(loop, *registry, 5s));
    ASSERT_EQ(registry->entries().size(), 2u);

    const std::string launcher = "/usr/bin/launcher";
    EXPECT_EQ(registry->resolveWindow("org.example.Edit", "org.example.Edit", launcher, "launcher"),
              registry->byId("org.example.Edit"));
    EXPECT_EQ(registry->resolveWindow("org.example.Clip", "org.example.Clip", launcher, "launcher"),
              registry->byId("org.example.Clip"));
}

TEST(ApplicationRegistryTest, NamesAWindowByTheOnlyEntryForItsProgram)
{
    file::TemporaryDirectory directory(std::filesystem::temp_directory_path().string(), "cutefish-test-");
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(file::writeAtomically(directory.path() + "/org.example.Edit.desktop",
                                      launcherEntry("Edit", "org.example.Edit")));

    EventLoop loop;
    std::unique_ptr<ApplicationRegistry> registry = ApplicationRegistry::create(loop, {directory.path()});
    ASSERT_TRUE(waitForScan(loop, *registry, 5s));
    EXPECT_EQ(registry->resolveWindow("", "", "/usr/bin/launcher", "launcher"), registry->byId("org.example.Edit"));
}

TEST(ApplicationRegistryTest, WatchesADirectoryOnceItCanBeWatched)
{
    file::TemporaryDirectory directory(std::filesystem::temp_directory_path().string(), "cutefish-test-");
    ASSERT_TRUE(directory.isValid());
    namespace fs = std::filesystem;
    // Without read access inotify refuses the watch, as it does when the
    // user's watches have run out.
    fs::permissions(directory.path(), fs::perms::none);

    EventLoop loop;
    std::unique_ptr<ApplicationRegistry> registry = ApplicationRegistry::create(loop, {directory.path()});
    ASSERT_TRUE(waitForScan(loop, *registry, 5s));
    EXPECT_TRUE(registry->entries().empty());

    fs::permissions(directory.path(), fs::perms::owner_all);
    ASSERT_TRUE(file::writeAtomically(directory.path() + "/org.example.Edit.desktop",
                                      launcherEntry("Edit", "org.example.Edit")));
    ASSERT_TRUE(waitForScan(loop, *registry, 10s));
    EXPECT_NE(registry->byId("org.example.Edit"), nullptr);
}

TEST(DesktopEntryTest, KeepsEmptyQuotedArguments)
{
    EXPECT_EQ(DesktopEntry::parseExec(R"(app --title "" '' --next %f)"),
              (std::vector<std::string>{"app", "--title", "", "", "--next"}));
}
