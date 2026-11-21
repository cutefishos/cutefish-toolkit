// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "base/event_loop.h"
#include "ui/animation.h"
#include "ui/application.h"
#include "ui/button.h"
#include "ui/check_box.h"
#include "ui/combo_box.h"
#include "ui/dialog_window.h"
#include "ui/icon.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/progress_bar.h"
#include "ui/scroll_view.h"
#include "ui/segmented_control.h"
#include "ui/slider.h"
#include "ui/switch.h"
#include "ui/text_area.h"
#include "ui/text_field.h"
#include "ui/theme.h"
#include "ui/window.h"
#include "ui/window_header.h"

using namespace cutefish::ui;

// A fixed frame time, so captures show animations at one phase.
static constexpr uint64_t kCaptureFrameNs = 1000000000;

static Label* heading(Column* column, const std::string& text)
{
    Label* label = column->addChild<Label>(text);
    label->setWeight(600);
    return label;
}

static Row* row(Column* column)
{
    Row* row = column->addChild<Row>();
    row->setSpacing(Theme::current().largeSpacing);
    row->setLayoutHints({.fillWidth = true});
    return row;
}

namespace {

// Every control of the toolkit on one page, for looking at and for pixel
// regression captures.
class Gallery
{
public:
    explicit Gallery(Window& window);

    std::unique_ptr<View> takeRoot() { return std::move(m_root); }
    // The header and the whole page, unscrolled.
    double contentHeight(double width) const;

private:
    Window& m_window;
    ButtonGroup m_group;
    Animation m_motion;
    std::unique_ptr<ConfirmDialog> m_confirm;
    std::unique_ptr<DialogWindow> m_details;
    std::unique_ptr<View> m_root;
    WindowHeader* m_header = nullptr;
    Column* m_page = nullptr;
};

} // namespace

Gallery::Gallery(Window& window)
    : m_window(window)
    , m_motion(&window)
{
    auto root = std::make_unique<Column>();
    m_header = root->addChild<WindowHeader>();
    m_header->setLayoutHints({.fillWidth = true});
    Label* title = m_header->setContent<Label>("Toolkit Gallery");
    title->setWeight(600);
    title->setTextAlignment(Label::TextAlignment::Center);
    title->setVerticallyCentered(true);
    ScrollView* scroll = root->addChild<ScrollView>();
    scroll->setLayoutHints({.fillWidth = true, .fillHeight = true});
    scroll->setBottomCornerRadius(Theme::current().windowRadius);
    Column* page = scroll->setContent<Column>();
    m_page = page;
    page->setMargins(Margins::all(24));
    page->setSpacing(14);

    heading(page, "Buttons");
    Row* buttons = row(page);
    buttons->addChild<Button>("Button");
    Button* disabled = buttons->addChild<Button>("Disabled");
    disabled->setEnabled(false);
    buttons->addChild<Button>("With a tool tip")->setToolTip("A tool tip over the button");

    heading(page, "Toggles");
    Row* toggles = row(page);
    toggles->addChild<Switch>("Switch")->setChecked(true);
    toggles->addChild<Switch>("Off");
    toggles->addChild<CheckBox>("Check box")->setChecked(true);
    toggles->addChild<CheckBox>("Unchecked");
    Row* radios = row(page);
    for (const char* text : {"First", "Second", "Third"}) {
        RadioButton* radio = radios->addChild<RadioButton>(text);
        radio->setGroup(&m_group);
    }
    static_cast<RadioButton*>(radios->children()[0].get())->setChecked(true);

    heading(page, "Values");
    Slider* slider = page->addChild<Slider>();
    slider->setValue(0.4);
    slider->setLayoutHints({.fillWidth = true});
    Slider* stepped = page->addChild<Slider>();
    stepped->setRange(0, 10);
    stepped->setStepSize(1);
    stepped->setStepped(true);
    stepped->setValue(6);
    stepped->setLayoutHints({.fillWidth = true});
    ProgressBar* progress = page->addChild<ProgressBar>();
    progress->setValue(0.65);
    progress->setLayoutHints({.fillWidth = true});
    slider->onMoved = [progress](double value) { progress->setValue(value); };
    Row* busy = row(page);
    busy->addChild<BusyIndicator>();
    busy->addChild<Label>("Working");

    heading(page, "Choices");
    Row* choices = row(page);
    choices->addChild<SegmentedControl>(std::vector<std::string>{"Day", "Week", "Month"});
    choices->addChild<ComboBox>(std::vector<std::string>{"Automatic", "Light", "Dark"});
    TabBar* tabs = page->addChild<TabBar>(std::vector<std::string>{"General", "Display", "Sound"});
    tabs->setLayoutHints({.fillWidth = true});

    heading(page, "Text");
    TextField* field = page->addChild<TextField>();
    field->setPlaceholder("Search");
    field->setLayoutHints({.fillWidth = true});
    TextArea* area = page->addChild<TextArea>("Notes that run on to a second line\nand a third");
    area->setPlaceholder("Notes");
    area->setLayoutHints({.fillWidth = true});

    heading(page, "Dialogs");
    Row* dialogs = row(page);
    Button* confirmButton = dialogs->addChild<Button>("Confirm dialog");
    confirmButton->onClicked = [this] {
        m_confirm = std::make_unique<ConfirmDialog>(&m_window, "Move the file to the trash?",
                                                    "It can be put back from the trash later.");
        m_confirm->setConfirmText("Move to Trash");
        m_confirm->show();
    };
    Button* detailsButton = dialogs->addChild<Button>("Dialog window");
    detailsButton->onClicked = [this] {
        m_details = std::make_unique<DialogWindow>(&m_window, "Network details", 360);
        auto content = std::make_unique<Column>();
        content->setSpacing(8);
        for (const char* line : {"Address 192.168.1.20", "Gateway 192.168.1.1", "DNS 1.1.1.1"})
            content->addChild<Label>(line);
        m_details->setContentView(std::move(content));
        m_details->show();
    };

    heading(page, "Layers");
    Row* layers = row(page);
    Button* animate = layers->addChild<Button>("Animate");
    // Slides, shrinks and fades as one texture, without repainting.
    Row* card = layers->addChild<Row>();
    card->setSpacing(Theme::current().mediumSpacing);
    card->addChild<ThemeIcon>("folder", 32);
    card->addChild<Label>("Moves as one texture");
    m_motion.setDuration(std::chrono::milliseconds(600));
    m_motion.setEasing(Animation::Easing::InOutCubic);
    m_motion.setLoops(2);
    m_motion.setAlternating(true);
    m_motion.onUpdate = [card](double value) {
        card->setTransform({{160 * value, 0}, 1 - 0.1 * value});
        card->setOpacity(1 - 0.4 * value);
    };
    animate->onClicked = [this] { m_motion.start(); };

    heading(page, "Icons");
    Row* glyphs = row(page);
    for (const char* name : {"check", "chevron-down", "chevron-right", "close", "search", "refresh",
                             "info", "lock", "folder", "network-wireless-connected-100"})
        glyphs->addChild<Icon>(name, 16);
    Row* themeIcons = row(page);
    for (const char* name : {"system-file-manager", "utilities-terminal", "preferences-system",
                             "folder", "folder-documents", "user-trash"})
        themeIcons->addChild<ThemeIcon>(name, 32);

    m_root = std::move(root);
}

double Gallery::contentHeight(double width) const
{
    return m_header->heightForWidth(width) + m_page->heightForWidth(width);
}

// Renders the gallery headless, light and dark, at the scales the desktop
// offers, into PNGs to compare before and after a change.
static int capture(const std::string& directory)
{
    constexpr double width = 760;
    const Theme light = Theme::current();
    int failures = 0;
    for (const auto& [name, theme] : {std::pair{"light", light}, std::pair{"dark", Theme::dark()}}) {
        Theme::setCurrent(theme);
        for (const double scale : {1.0, 1.25, 1.5, 2.0}) {
            Window window;
            Gallery gallery(window);
            window.setScale(scale);
            window.setContentView(gallery.takeRoot());
            // Measured in the window, at its scale.
            window.resize({width, std::ceil(gallery.contentHeight(width))});
            window.show();
            char file[64];
            snprintf(file, sizeof(file), "/gallery-%s-%g.png", name, scale);
            if (!window.grab(kCaptureFrameNs).savePng(directory + file)) {
                fprintf(stderr, "cutefish-toolkit-gallery: cannot write %s%s\n", directory.c_str(), file);
                ++failures;
            }
        }
    }
    Theme::setCurrent(light);
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    std::string captureDirectory;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.starts_with("--capture="))
            captureDirectory = argument.substr(10);
    }
    if (!captureDirectory.empty()) {
        Application application(argc, argv, Application::Platform::Headless);
        return capture(captureDirectory);
    }

    Application application(argc, argv);
    double quitAfter = 0;
    for (const std::string& argument : application.arguments()) {
        if (argument.starts_with("--quit-after="))
            quitAfter = std::stod(argument.substr(13));
        else if (argument == "--dark")
            Theme::setCurrent(Theme::dark());
    }

    Window window;
    window.setTitle("Toolkit Gallery");
    window.resize({760, 720});
    window.addShortcut(ControlModifier, key::letter('q'), [&application] { application.quit(); });
    Gallery gallery(window);
    window.setContentView(gallery.takeRoot());
    window.show();

    if (quitAfter > 0) {
        application.loop().startTimer(std::chrono::nanoseconds(int64_t(quitAfter * 1e9)), false,
                                      [&application] { application.quit(); });
    }
    return application.run();
}
