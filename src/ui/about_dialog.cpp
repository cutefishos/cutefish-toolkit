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
#include "ui/cell_layout.h"
#include "ui/color_provider.h"
#include "ui/icon_loader.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/theme.h"
#include "ui/window.h"
#include "ui/window_header.h"

namespace cutefish::ui {

static constexpr double kWidth = 300;
static constexpr double kHeaderHeight = 40;
static constexpr double kIconSize = 64;

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

// An Image of an image://icontheme source at a sourceSize, as FishUI's
// provider answers it: a theme icon comes as a pixmap for sourceSize × the
// scale at the whole pixel ratio above it, and the Image takes that pixmap's
// size at the window's scale, so a 64 icon is 128 logical at 150%; a file
// comes at the requested size itself.
class ProviderImage : public View
{
public:
    ProviderImage(std::string source, double sourceSize)
        : m_source(std::move(source))
        , m_sourceSize(sourceSize)
    {
    }

    SizeF sizeHint() const override
    {
        const double scale = textLayoutMode().scale;
        const double side = pixels(scale) / scale;
        return {side, side};
    }

protected:
    void paint(Painter& painter) override
    {
        if (m_source.empty())
            return;
        const double scale = painter.scale();
        const int requested = int(std::lround(m_sourceSize * scale));
        sk_sp<SkImage> image = isFile() ? IconLoader::instance().pixmap(m_source, requested)
                                        : IconLoader::instance().themeIcon(m_source, requested, wholeScale(scale),
                                                                           pixels(scale));
        if (!image)
            return;
        // A plain Image: where it lies, filtered.
        const PointF origin = painter.offset();
        const int side = pixels(scale);
        SkPaint paint;
        paint.setAlphaf(float(painter.opacity()));
        painter.canvas()->drawImageRect(image, SkRect::MakeXYWH(SkScalar(origin.x * scale), SkScalar(origin.y * scale),
                                                                 SkScalar(side), SkScalar(side)),
                                        SkSamplingOptions(SkFilterMode::kLinear), &paint);
    }

private:
    bool isFile() const { return !m_source.empty() && m_source.front() == '/'; }
    static int wholeScale(double scale) { return std::max(1, int(std::ceil(scale - 1e-9))); }
    int pixels(double scale) const
    {
        const int requested = int(std::lround(m_sourceSize * scale));
        return isFile() ? requested : requested * wholeScale(scale);
    }

    std::string m_source;
    double m_sourceSize;
};

} // namespace

class AboutDialog::Content : public View
{
public:
    explicit Content(AboutDialog* dialog)
        : m_header(addChild<WindowHeader>(kHeaderHeight))
        , m_column(addChild<ColumnLayout>())
    {
        m_header->setMinimizeVisible(false);
        // Fixed in both directions: no maximize button.
        m_header->setMaximizeVisible(false);

        View* icon = m_column->addChild<ProviderImage>(dialog->m_icon, kIconSize);
        LayoutHints centred;
        centred.horizontalAlignment = Alignment::Center;
        icon->setLayoutHints(centred);

        View* gap = m_column->addChild<View>();
        LayoutHints gapHints;
        gapHints.fallbackHeight = Theme::current().largeSpacing;
        gap->setLayoutHints(gapHints);

        Label* name = m_column->addChild<Label>(dialog->m_name);
        name->setFontSize(14.0 * 4 / 3);
        name->setLayoutHints(centred);
        Label* description = m_column->addChild<Label>(dialog->m_description);
        description->setLayoutHints(centred);

        LinkLabel* link = m_column->addChild<LinkLabel>();
        link->setSpans({{dialog->m_link, 0, ColorId::Highlight, false, true, dialog->m_link}});
        link->setLayoutHints(centred);
        link->onClicked = [dialog] {
            const std::string url = dialog->m_link;
            if (dialog->onLinkActivated)
                dialog->onLinkActivated(url);
            else
                base::Process::startDetached("xdg-open", {url});
        };

        View* filler = m_column->addChild<View>();
        LayoutHints fill;
        fill.fillHeight = true;
        filler->setLayoutHints(fill);
    }

    // The column's implicit height and the header's height twice, as
    // FishUI's contentHeight.
    double heightForWidth(double width) const override
    {
        return m_column->heightForWidth(width) + 2 * kHeaderHeight;
    }

    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const RectF bounds = localRect();
        m_header->setGeometry({0, 0, bounds.width, kHeaderHeight});
        // The column fills the window under the header, less a header's
        // height at the bottom.
        m_column->setGeometry({0, kHeaderHeight, bounds.width, std::max(0.0, bounds.height - 2 * kHeaderHeight)});
        // The blur follows the size the content gives the window.
        if (Window* window = this->window(); window && bounds.size() != m_blurSize) {
            m_blurSize = bounds.size();
            window->setBackgroundBlur(bounds, Theme::current().windowRadius);
        }
    }

    void paint(Painter& painter) override
    {
        // Over the compositor's blur, as FishUI.Window's background.
        painter.fillRect(localRect(), color(ColorId::AboutDialogBackground));
    }

private:
    WindowHeader* m_header;
    ColumnLayout* m_column;
    SizeF m_blurSize;
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
    m_window->setBackgroundColor(Color::rgb(0x000000).withAlpha(0));
    m_window->resize({kWidth, 0});
    m_window->setFitsContentHeight(true);
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
