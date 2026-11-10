// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/about_dialog.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"

#include "base/event_loop.h"
#include "base/process.h"
#include "ui/application.h"
#include "ui/color_provider.h"
#include "ui/icon_loader.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/window.h"
#include "ui/window_header.h"

namespace cutefish::ui {

// As wide as it is; as tall as its contents, which makes it taller than wide.
static constexpr double kWidth = 280;
static constexpr double kHeaderHeight = 40;
static constexpr double kIconTop = 40;
static constexpr double kIconSize = 112;
static constexpr double kIconGap = 20;
static constexpr double kNameGap = 8;
// The description wraps at this width.
static constexpr double kDescriptionWidth = 200;
static constexpr double kLinkGap = 20;
static constexpr double kBottomMargin = 40;

namespace {

// The link, opened on a click anywhere on it, as a MouseArea filling it.
class LinkLabel : public Label
{
public:
    using Label::Label;

    bool acceptsPointer() const override { return true; }
    CursorShape cursorShape() const override { return CursorShape::Pointer; }

    std::function<void()> onClicked;

protected:
    void pointerPress(const PointerEvent& event) override { m_pressed = event.button == MouseButton::Left; }

    void pointerRelease(const PointerEvent& event) override
    {
        const bool clicked = m_pressed && localRect().contains(event.position);
        m_pressed = false;
        if (clicked && onClicked)
            onClicked();
    }

private:
    bool m_pressed = false;
};

// The application's icon, a theme name or a file, the size in logical
// pixels at any scale.
class AboutIcon : public View
{
public:
    AboutIcon(std::string source, double size)
        : m_source(std::move(source))
        , m_size(size)
    {
    }

    SizeF sizeHint() const override { return {m_size, m_size}; }

protected:
    void paint(Painter& painter) override
    {
        if (m_source.empty())
            return;
        const double scale = painter.scale();
        const int side = int(std::lround(m_size * scale));
        sk_sp<SkImage> image = m_source.front() == '/'
                ? IconLoader::instance().pixmap(m_source, side)
                : IconLoader::instance().themeIcon(m_source, int(m_size), scale);
        if (!image)
            return;
        // Where it lies, filtered.
        const PointF origin = painter.offset();
        SkPaint paint;
        paint.setAlphaf(float(painter.opacity()));
        painter.canvas()->drawImageRect(image, SkRect::MakeXYWH(SkScalar(origin.x * scale), SkScalar(origin.y * scale),
                                                                 SkScalar(side), SkScalar(side)),
                                        SkSamplingOptions(SkFilterMode::kLinear), &paint);
    }

private:
    std::string m_source;
    double m_size;
};

} // namespace

class AboutDialog::Content : public View
{
public:
    explicit Content(AboutDialog* dialog)
        : m_header(addChild<WindowHeader>(kHeaderHeight))
        , m_icon(addChild<AboutIcon>(dialog->m_icon, kIconSize))
        , m_name(addChild<Label>(dialog->m_name))
        , m_description(addChild<Label>(dialog->m_description))
        , m_link(addChild<LinkLabel>())
    {
        // The controls' first centre 16 in from the top and the start.
        m_header->setButtonMargin(0);
        m_header->setMinimizeEnabled(false);
        // Fixed in both directions.
        m_header->setMaximizeEnabled(false);

        m_name->setFontSize(22);
        m_name->setWeight(700);
        m_name->setTextAlignment(Label::TextAlignment::Center);
        m_description->setFontSize(12);
        m_description->setColor(withAlpha(ColorId::Text, 0.7));
        m_description->setTextAlignment(Label::TextAlignment::Center);
        m_description->setWrap(true);
        m_link->setSpans({{dialog->m_link, 0, ColorId::Highlight, false, true, dialog->m_link}});
        m_link->onClicked = [dialog] {
            const std::string url = dialog->m_link;
            if (dialog->onLinkActivated)
                dialog->onLinkActivated(url);
            else
                base::Process::startDetached("xdg-open", {url});
        };
    }

    double heightForWidth(double width) const override
    {
        return arrange(width, false);
    }

    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const RectF bounds = localRect();
        m_header->setGeometry({0, 0, bounds.width, kHeaderHeight});
        arrange(bounds.width, true);
    }

private:
    // Places the views from the top when asked to; returns the height they take.
    double arrange(double width, bool place) const
    {
        const double middle = width / 2;
        const auto centred = [&](View* view, double y, double viewWidth, double height) {
            if (place)
                view->setGeometry({std::round(middle - viewWidth / 2), y, viewWidth, height});
        };
        double y = kHeaderHeight + kIconTop;
        centred(m_icon, y, kIconSize, kIconSize);
        y += kIconSize + kIconGap;
        const double nameHeight = m_name->sizeHint().height;
        centred(m_name, y, width, nameHeight);
        y += nameHeight + kNameGap;
        const double descriptionWidth = std::min(kDescriptionWidth, width);
        const double descriptionHeight = m_description->heightForWidth(descriptionWidth);
        centred(m_description, y, descriptionWidth, descriptionHeight);
        y += descriptionHeight + kLinkGap;
        // Only the link's own width takes clicks.
        const SizeF link = m_link->sizeHint();
        centred(m_link, y, std::min(link.width, width), link.height);
        y += link.height + kBottomMargin;
        return y;
    }

    WindowHeader* m_header;
    View* m_icon;
    Label* m_name;
    Label* m_description;
    LinkLabel* m_link;
};

AboutDialog::AboutDialog(Window* parent)
    : m_parent(parent)
{
}

AboutDialog::~AboutDialog() = default;

void AboutDialog::show()
{
    if (m_window) {
        m_window->activate();
        return;
    }
    m_window = std::make_unique<Window>();
    m_window->setTransientParent(m_parent, true);
    m_window->setContentView(std::make_unique<Content>(this));
    m_window->resize({kWidth, 0});
    m_window->setFitsContentHeight(true);
    m_window->setUserResizable(false);
    m_window->onCloseRequested = [this] { close(); };
    m_window->addShortcut(NoModifier, key::Escape, [this] { close(); });
    m_window->show();
}

void AboutDialog::close()
{
    if (!m_window)
        return;
    std::shared_ptr<Window> window(std::move(m_window));
    window->hide();
    Application::instance()->loop().post([window] {});
    if (onClosed)
        onClosed();
}

} // namespace cutefish::ui
