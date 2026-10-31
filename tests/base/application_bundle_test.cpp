// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>

#include "base/application_bundle.h"
#include "base/application_registry.h"
#include "base/event_loop.h"
#include "base/file.h"

using namespace cutefish::base;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

static constexpr const char* kHandler = R"([Desktop Entry]
Type=Application
Name=Bundle Runner
Exec=runner --open %f
MimeType=application/x-apple-app;
NoDisplay=true
)";

static std::string infoPlist(const std::string& name, const std::string& icon)
{
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>)" + name + R"(</string>
	<key>CFBundleName</key>
	<string>)" + name + R"(</string>
	<key>CFBundleIconFile</key>
	<string>)" + icon + R"(</string>
</dict>
</plist>
)";
}

static bool makeBundle(const std::string& path, const std::string& name, const std::string& icon = "AppIcon")
{
    std::error_code error;
    fs::create_directories(path + "/Contents/MacOS", error);
    fs::create_directories(path + "/Contents/Resources", error);
    return file::writeAtomically(path + "/Contents/Info.plist", infoPlist(name, icon))
            && file::writeAtomically(path + "/Contents/Resources/AppIcon.icns", "icns");
}

static bool waitForChange(EventLoop& loop, ApplicationRegistry& registry, std::chrono::milliseconds timeout)
{
    bool changed = false;
    Subscription subscription = registry.changed.connect([&] {
        changed = true;
        loop.quit();
    });
    const int timer = loop.startTimer(timeout, false, [&loop] { loop.quit(); });
    loop.run();
    loop.stopTimer(timer);
    return changed;
}

TEST(ApplicationBundleTest, FindsTheBundleOfAPath)
{
    EXPECT_EQ(bundle::bundlePath("/a/Editor.app/"), "/a/Editor.app");
    EXPECT_EQ(bundle::bundlePath("/a/b/../Editor.app"), "/a/Editor.app");
    EXPECT_EQ(bundle::bundlePath("/a/.app"), "");
    EXPECT_EQ(bundle::bundlePath("/a/Editor.apps"), "");
    EXPECT_EQ(bundle::bundleOfProgram("/a/Editor.app/Contents/MacOS/Editor"), "/a/Editor.app");
    EXPECT_EQ(bundle::bundleOfProgram("/a/Editor.app/Contents/Helpers/Tool"), "");
    EXPECT_EQ(bundle::bundleOfProgram("/a/Contents/MacOS/Editor"), "");
    EXPECT_EQ(bundle::bundleOfProgram("/usr/bin/editor"), "");
}

TEST(ApplicationBundleTest, ReadsABundleAsAnEntry)
{
    file::TemporaryDirectory directory(fs::temp_directory_path().string(), "cutefish-test-");
    ASSERT_TRUE(directory.isValid());
    const std::string path = directory.path() + "/My Editor.app";
    ASSERT_TRUE(makeBundle(path, "Editor 100%"));

    const auto handler = DesktopEntry::parse("runner", "/x/runner.desktop", kHandler, "en_US");
    ASSERT_TRUE(handler);
    const auto entry = bundle::read(path + "/", *handler);
    ASSERT_TRUE(entry);
    EXPECT_TRUE(entry->bundle);
    EXPECT_EQ(entry->id, path);
    EXPECT_EQ(entry->path, path);
    EXPECT_EQ(entry->name, "Editor 100%");
    EXPECT_EQ(entry->icon, path + "/Contents/Resources/AppIcon.icns");
    EXPECT_EQ(entry->command(), (std::vector<std::string>{"runner", "--open", path}));
    EXPECT_EQ(entry->commandForArguments({"/a/b.txt", "/a/c d.txt"}),
              (std::vector<std::string>{"runner", "--open", path, "/a/b.txt", "/a/c d.txt"}));
    EXPECT_TRUE(entry->mimeTypes.empty());
    EXPECT_TRUE(entry->shouldShow());
    EXPECT_TRUE(entry->startupNotify);

    // An agent application starts without launch feedback.
    ASSERT_TRUE(file::writeAtomically(path + "/Contents/Info.plist", R"(<plist version="1.0">
<dict>
	<key>CFBundleName</key>
	<string>Agent</string>
	<key>LSUIElement</key>
	<true/>
</dict>
</plist>
)"));
    EXPECT_FALSE(bundle::read(path, *handler)->startupNotify);

    // A missing icon is none, and a bundle without a list is no application.
    ASSERT_TRUE(makeBundle(path, "Editor", "Missing.icns"));
    EXPECT_EQ(bundle::read(path, *handler)->icon, "");
    fs::remove(path + "/Contents/Info.plist");
    EXPECT_FALSE(bundle::read(path, *handler));
}

TEST(ApplicationBundleTest, OpensItsDocumentTypes)
{
    file::TemporaryDirectory directory(fs::temp_directory_path().string(), "cutefish-test-");
    ASSERT_TRUE(directory.isValid());
    const std::string path = directory.path() + "/Viewer.app";
    ASSERT_TRUE(makeBundle(path, "Viewer"));
    ASSERT_TRUE(file::writeAtomically(path + "/Contents/Info.plist", R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>CFBundleName</key>
	<string>Viewer</string>
	<key>CFBundleDocumentTypes</key>
	<array>
		<dict>
			<key>LSItemContentTypes</key>
			<array>
				<string>public.plain-text</string>
				<string>public.image</string>
				<string>public.data</string>
				<string>org.example.sketch</string>
			</array>
			<key>CFBundleTypeExtensions</key>
			<array><string>csv</string></array>
		</dict>
		<dict>
			<key>CFBundleTypeExtensions</key>
			<array><string>pdf</string><string>*</string></array>
			<key>CFBundleTypeMIMETypes</key>
			<string>application/x-pdf</string>
		</dict>
		<dict>
			<key>CFBundleTypeRole</key>
			<string>None</string>
			<key>LSItemContentTypes</key>
			<array><string>public.audio</string></array>
		</dict>
		<dict>
			<key>LSHandlerRank</key>
			<string>None</string>
			<key>CFBundleTypeExtensions</key>
			<array><string>zip</string></array>
		</dict>
	</array>
	<key>UTExportedTypeDeclarations</key>
	<array>
		<dict>
			<key>UTTypeIdentifier</key>
			<string>org.example.sketch</string>
			<key>UTTypeConformsTo</key>
			<array><string>public.data</string><string>public.text</string></array>
			<key>UTTypeTagSpecification</key>
			<dict>
				<key>public.filename-extension</key>
				<array><string>md</string><string>zip</string></array>
				<key>public.mime-type</key>
				<string>application/x-no-such-type</string>
			</dict>
		</dict>
	</array>
</dict>
</plist>
)"));

    const auto handler = DesktopEntry::parse("runner", "/x/runner.desktop", kHandler, "en_US");
    ASSERT_TRUE(handler);
    const auto entry = bundle::read(path, *handler);
    ASSERT_TRUE(entry);
    // shared-mime-info has these on every system the desktop runs on.
    EXPECT_EQ(entry->mimeTypes,
              (std::vector<std::string>{"text/plain", "image/*", "text/markdown", "text/csv", "application/pdf"}));

    EXPECT_TRUE(bundle::opensType(*entry, "text/plain"));
    EXPECT_TRUE(bundle::opensType(*entry, "text/x-csrc"));
    EXPECT_TRUE(bundle::opensType(*entry, "image/png"));
    EXPECT_TRUE(bundle::opensType(*entry, "application/x-pdf"));
    EXPECT_FALSE(bundle::opensType(*entry, "audio/mpeg"));
    EXPECT_FALSE(bundle::opensType(*entry, "application/zip"));
    EXPECT_FALSE(bundle::opensType(*entry, "application/octet-stream"));
    EXPECT_FALSE(bundle::opensType(*entry, "imagex/png"));
}

TEST(ApplicationBundleTest, ListsInstalledBundlesWhenAnEntryRunsThem)
{
    file::TemporaryDirectory applications(fs::temp_directory_path().string(), "cutefish-test-");
    file::TemporaryDirectory bundles(fs::temp_directory_path().string(), "cutefish-test-");
    ASSERT_TRUE(applications.isValid() && bundles.isValid());
    const std::string installed = bundles.path() + "/Editor.app";
    ASSERT_TRUE(makeBundle(installed, "Editor"));
    ASSERT_TRUE(makeBundle(bundles.path() + "/Folder/Nested.app", "Nested"));

    EventLoop loop;
    std::unique_ptr<ApplicationRegistry> registry =
            ApplicationRegistry::create(loop, {applications.path()}, bundles.path());
    ASSERT_TRUE(waitForChange(loop, *registry, 5s));
    EXPECT_TRUE(registry->entries().empty());

    ASSERT_TRUE(file::writeAtomically(applications.path() + "/runner.desktop", kHandler));
    ASSERT_TRUE(waitForChange(loop, *registry, 5s));
    ASSERT_EQ(registry->applications().size(), 1u);
    const auto entry = registry->applications().front();
    EXPECT_EQ(entry->path, installed);
    EXPECT_EQ(registry->byId(installed), entry);
    EXPECT_EQ(registry->installedById(installed), entry);
    EXPECT_EQ(registry->byPath(installed + "/"), entry);

    // Its program is the bundle's, whatever the window calls itself, and no
    // other window is.
    EXPECT_EQ(registry->resolveWindow("org.example.Editor", "", installed + "/Contents/MacOS/Editor", "Editor"),
              entry);
    EXPECT_EQ(registry->resolveWindow("Editor", "Editor", "/usr/bin/editor", "editor"), nullptr);

    fs::rename(installed, bundles.path() + "/Renamed.app");
    ASSERT_TRUE(waitForChange(loop, *registry, 5s));
    ASSERT_EQ(registry->applications().size(), 1u);
    EXPECT_EQ(registry->applications().front()->path, bundles.path() + "/Renamed.app");
    EXPECT_FALSE(registry->installedById(installed));

    // Taken out of ~/Applications, as into the Trash, it is installed no more.
    fs::rename(bundles.path() + "/Renamed.app", applications.path() + "/Renamed.app");
    ASSERT_TRUE(waitForChange(loop, *registry, 5s));
    EXPECT_TRUE(registry->applications().empty());
    EXPECT_FALSE(registry->installedById(bundles.path() + "/Renamed.app"));
    EXPECT_FALSE(registry->installedById(applications.path() + "/Renamed.app"));
}

TEST(ApplicationBundleTest, ReadsOtherBundlesOnAThread)
{
    file::TemporaryDirectory applications(fs::temp_directory_path().string(), "cutefish-test-");
    file::TemporaryDirectory elsewhere(fs::temp_directory_path().string(), "cutefish-test-");
    ASSERT_TRUE(applications.isValid() && elsewhere.isValid());
    ASSERT_TRUE(file::writeAtomically(applications.path() + "/runner.desktop", kHandler));
    const std::string other = elsewhere.path() + "/Image/Editor.app";
    ASSERT_TRUE(makeBundle(other, "Editor"));

    EventLoop loop;
    std::unique_ptr<ApplicationRegistry> registry =
            ApplicationRegistry::create(loop, {applications.path()}, elsewhere.path() + "/Applications");
    ASSERT_TRUE(waitForChange(loop, *registry, 5s));
    EXPECT_TRUE(registry->applications().empty());

    const std::string program = other + "/Contents/MacOS/Editor";
    EXPECT_EQ(registry->resolveWindow("org.example.Editor", "", program, "Editor"), nullptr);
    ASSERT_TRUE(waitForChange(loop, *registry, 5s));
    const auto entry = registry->resolveWindow("org.example.Editor", "", program, "Editor");
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->path, other);
    EXPECT_TRUE(entry->noDisplay);
    EXPECT_EQ(registry->byId(other), entry);
    EXPECT_FALSE(registry->installedById(other));
    EXPECT_TRUE(registry->applications().empty());

    // Read now when asked to; gone once it is, which the registry sees.
    EXPECT_EQ(registry->readBundle(other)->path, other);
    fs::remove_all(other);
    EXPECT_FALSE(registry->readBundle(other));
    EXPECT_FALSE(registry->readBundle(elsewhere.path()));
    ASSERT_TRUE(waitForChange(loop, *registry, 5s));
    EXPECT_FALSE(registry->byPath(other));
}
