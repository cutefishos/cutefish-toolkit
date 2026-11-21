// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "base/event_loop.h"
#include "ui/animation.h"
#include "ui/application.h"
#include "ui/button.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/list_view.h"
#include "ui/painter.h"
#include "ui/text_field.h"
#include "ui/theme.h"
#include "ui/view.h"
#include "ui/window.h"

using namespace cutefish::ui;

static constexpr double kTitleBarHeight = 44;
static constexpr double kCloseButtonSize = 28;
static constexpr double kTrackHeight = 80;
static constexpr double kTrackRadius = 20;
static constexpr double kCardInset = 8;
static constexpr double kCardSize = kTrackHeight - 2 * kCardInset;
static constexpr Color kAccent = Color::rgb(0x0176D3);
static constexpr Color kOutline = Color::rgb(0xE3E4E8);

class CloseButton : public View
{
public:
    std::function<void()> onClicked;

    SizeF sizeHint() const override { return {kCloseButtonSize, kCloseButtonSize}; }
    bool acceptsPointer() const override { return true; }

protected:
    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        if (m_hovered)
            painter.fillEllipse(localRect(), theme.alternateBackground.darker(1.05));
        const double centre = geometry().width / 2;
        const double arm = 4.5;
        const double stroke = 1.5 * painter.scale();
        painter.drawLine({centre - arm, centre - arm}, {centre + arm, centre + arm}, theme.text,
                         stroke);
        painter.drawLine({centre + arm, centre - arm}, {centre - arm, centre + arm}, theme.text,
                         stroke);
    }

    void pointerEnter() override
    {
        m_hovered = true;
        requestPaint();
    }

    void pointerLeave() override
    {
        m_hovered = false;
        requestPaint();
    }

    void pointerRelease(const PointerEvent& event) override
    {
        if (localRect().contains(event.position) && onClicked)
            onClicked();
    }

private:
    bool m_hovered = false;
};

class TitleBar : public Row
{
public:
    TitleBar()
    {
        const double inset = (kTitleBarHeight - kCloseButtonSize) / 2;
        setMargins({inset, 0, inset, 0});
        // Keeps the title centred against the close button.
        addChild<View>()->setLayoutHints({.preferredWidth = kCloseButtonSize});
        m_title = addChild<Label>("Toolkit Demo");
        m_title->setWeight(600);
        m_title->setTextAlignment(Label::TextAlignment::Center);
        m_title->setLayoutHints({.fillWidth = true});
        m_close = addChild<CloseButton>();
        m_close->onClicked = [] { Application::instance()->quit(); };
        setLayoutHints({.fillWidth = true, .preferredHeight = kTitleBarHeight});
    }

    bool acceptsPointer() const override { return true; }

protected:
    void pointerPress(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left)
            window()->startMove();
    }

private:
    Label* m_title;
    CloseButton* m_close;
};

class Card : public View
{
protected:
    void paint(Painter& painter) override
    {
        // Concentric with the track: the track radius less the inset.
        painter.fillRoundedRect(localRect(), kTrackRadius - kCardInset, kAccent);
    }
};

class Track : public View
{
public:
    Track()
        : m_card(addChild<Card>())
    {
        setLayoutHints({.fillWidth = true, .preferredHeight = kTrackHeight});
    }

    void setProgress(double progress)
    {
        m_progress = progress;
        placeCard();
    }

protected:
    void layout() override { placeCard(); }

    void paint(Painter& painter) override
    {
        painter.fillRoundedRect(localRect(), kTrackRadius, Theme::current().secondBackground);
        painter.strokeRoundedRect(localRect(), kTrackRadius, kOutline);
    }

private:
    void placeCard()
    {
        const double travel = geometry().width - 2 * kCardInset - kCardSize;
        m_card->setGeometry({kCardInset + travel * m_progress, kCardInset, kCardSize, kCardSize});
    }

    Card* m_card;
    double m_progress = 0;
};

class ListRow : public View
{
public:
    ListRow()
        : m_label(addChild<Label>())
    {
        m_label->setVerticallyCentered(true);
    }

    void setText(std::string text) { m_label->setText(std::move(text)); }

protected:
    void layout() override { m_label->setGeometry(localRect().adjusted(12, 0, -12, 0)); }

    void paint(Painter& painter) override
    {
        painter.drawHorizontalLine(12, geometry().height - 1, geometry().width - 24, kOutline);
    }

private:
    Label* m_label;
};

// One-device-pixel lines, to check edges at every scale.
class HairlineBox : public View
{
public:
    HairlineBox() { setLayoutHints({.fillWidth = true, .preferredHeight = 16}); }

protected:
    void paint(Painter& painter) override
    {
        painter.strokeRoundedRect(localRect(), 0, kOutline);
        painter.drawHorizontalLine(0, geometry().height / 2, geometry().width, kOutline);
    }
};

class DemoView : public Column
{
public:
    explicit DemoView(Window* window)
        : m_animation(window)
    {
        addChild<TitleBar>();

        auto* content = addChild<Column>();
        content->setMargins({24, 16, 24, 0});
        content->setSpacing(12);
        content->setLayoutHints({.fillWidth = true});

        auto* heading = content->addChild<Label>("cutefish-toolkit");
        heading->setFontSize(24);
        heading->setWeight(600);

        auto* body = content->addChild<Label>(
            "A prototype UI toolkit on Skia: a Wayland client, fractional scaling, text "
            "layout and a frame-clock animation, drawn by the same code at every scale.");
        body->setWrap(true);
        body->setLayoutHints({.fillWidth = true});

        m_track = content->addChild<Track>();

        auto* field = content->addChild<TextField>();
        field->setPlaceholder("Type here");
        field->setLayoutHints({.fillWidth = true});

        auto* controls = content->addChild<Row>();
        controls->setSpacing(16);
        controls->setLayoutHints({.fillWidth = true});
        m_button = controls->addChild<Button>("Pause");
        m_button->setToolTip("Pause or resume the animation");
        m_status = controls->addChild<Label>();
        m_status->setColor(Theme::current().disabledText);
        m_status->setLayoutHints({.fillWidth = true});

        auto* list = addChild<ListView>();
        list->setLayoutHints({.fillWidth = true, .fillHeight = true});
        list->setMargins({24, 12, 24, 12});
        list->setRowHeight(32);
        list->setRowCount(200);
        list->createItem = [] { return std::make_unique<ListRow>(); };
        list->bindItem = [](View* item, size_t row) {
            static_cast<ListRow*>(item)->setText("Row " + std::to_string(row + 1));
        };
        auto* footer = addChild<Column>();
        footer->setMargins({24, 0, 24, 24});
        footer->setLayoutHints({.fillWidth = true});
        footer->addChild<HairlineBox>();

        m_animation.setDuration(std::chrono::milliseconds(1200));
        m_animation.setEasing(Animation::Easing::InOutCubic);
        m_animation.setLoops(Animation::Infinite);
        m_animation.setAlternating(true);
        m_animation.onUpdate = [this](double value) {
            m_track->setProgress(value);
            if (m_fullRepaint && this->window())
                this->window()->requestPaint();
        };

        m_button->onClicked = [this] {
            if (m_paused)
                m_animation.resume();
            else
                m_animation.pause();
            m_paused = !m_paused;
            m_button->setText(m_paused ? "Resume" : "Pause");
        };
    }

    void startAnimation() { m_animation.start(); }
    // Repaints the whole window every frame, to compare with damage tracking.
    void setFullRepaint(bool fullRepaint) { m_fullRepaint = fullRepaint; }

protected:
    void layout() override
    {
        const Window* window = this->window();
        char status[96];
        snprintf(status, sizeof(status), "Scale %.2f, buffer %ld x %ld", window->scale(),
                 std::lround(geometry().width * window->scale()),
                 std::lround(geometry().height * window->scale()));
        m_status->setText(status);
        Column::layout();
    }

private:
    Track* m_track;
    Button* m_button;
    Label* m_status;
    Animation m_animation;
    bool m_paused = false;
    bool m_fullRepaint = false;
};

int main(int argc, char** argv)
{
    Application application(argc, argv);

    bool statistics = false;
    double quitAfter = 0;
    bool animate = true;
    bool fullRepaint = false;
    for (const std::string& argument : application.arguments()) {
        if (argument == "--stats")
            statistics = true;
        else if (argument == "--no-animation")
            animate = false;
        else if (argument == "--full-repaint")
            fullRepaint = true;
        else if (argument.starts_with("--quit-after="))
            quitAfter = std::stod(argument.substr(13));
    }

    Window window;
    window.addShortcut(ControlModifier, key::letter('q'), [&application] { application.quit(); });
    window.setTitle("Toolkit Demo");
    window.setMinimumSize({480, 360});
    window.resize({720, 640});

    auto content = std::make_unique<DemoView>(&window);
    DemoView* demo = content.get();
    demo->setFullRepaint(fullRepaint);
    window.setContentView(std::move(content));
    if (animate)
        demo->startAnimation();
    window.show();

    if (quitAfter > 0) {
        application.loop().startTimer(
            std::chrono::nanoseconds(int64_t(quitAfter * 1e9)), false,
            [&application] { application.quit(); });
    }

    const int exitCode = application.run();

    if (statistics) {
        const FrameStatistics& frames = window.frameStatistics();
        printf("first-swap-ns %llu\n", static_cast<unsigned long long>(frames.firstSwapNs));
        printf("first-presented-ns %llu\n",
               static_cast<unsigned long long>(frames.firstPresentedNs));
        printf("frames-rendered %llu\n", static_cast<unsigned long long>(frames.rendered));
        printf("frames-presented %llu\n", static_cast<unsigned long long>(frames.presented));
        printf("frames-discarded %llu\n", static_cast<unsigned long long>(frames.discarded));
    }
    return exitCode;
}
