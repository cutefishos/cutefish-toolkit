// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "base/signal.h"
#include "ui/view.h"

namespace cutefish::ui {

class ScrollBar;

// A viewport onto a larger content view, moved by the touchpad, the wheel and
// its scroll bars, with cutefish-framework's touchpad physics. It scrolls
// vertically by default: the content then gets the viewport's width and the
// height it asks for at that width.
class ScrollView : public View
{
public:
    ScrollView();
    ~ScrollView() override;

    void setContentView(std::unique_ptr<View> view);
    template<typename T, typename... Args>
    T* setContent(Args&&... args)
    {
        auto view = std::make_unique<T>(std::forward<Args>(args)...);
        T* content = view.get();
        setContentView(std::move(view));
        return content;
    }
    View* contentView() const { return m_content; }

    void setScrollDirections(bool horizontal, bool vertical);
    // Past the ends the content stretches and springs back.
    void setElastic(bool elastic);
    // Settles on whole viewport-sized pages.
    void setPaged(bool paged);
    // The vertical bar's track starts and ends this far in, as a ScrollBar's
    // topPadding and bottomPadding, e.g. to stop under a floating header.
    void setVerticalBarPadding(double top, double bottom);
    // Clips to corners of this radius, those of the rounded plate it fills,
    // and keeps the bars' handles clear of them.
    void setCornerRadius(double radius);
    // The content takes presses over the bars, as a ScrollBar with z: -1,
    // for rows whose handles reach into the bar's strip.
    void setBarsBelowContent(bool below);
    // As ScrollBar's policy: AsNeeded shows while scrolling.
    enum class BarPolicy {
        AsNeeded,
        AlwaysOn,
        AlwaysOff,
    };
    void setVerticalBarPolicy(BarPolicy policy);

    // The content's offset under the viewport's top left.
    PointF scrollPosition() const { return m_position; }
    // Clamped to the content; stops a coast.
    void setScrollPosition(PointF position);
    // Scrolls as little as needed to show the rect, in content coordinates.
    void ensureVisible(const RectF& rect);
    SizeF contentSize() const { return m_contentSize; }

    std::function<void()> onScrolled;
    // The same, for any number of listeners.
    base::Signal<> scrolled;

protected:
    void layout() override;
    bool scroll(const ScrollEvent& event) override;
    // After every change of the position, before the frame is painted.
    virtual void positionChanged() {}

private:
    struct Private;

    void applyPosition(PointF position);
    void updateBars();
    PointF maximumPosition() const;

    std::unique_ptr<Private> d;
    View* m_content = nullptr;
    ScrollBar* m_verticalBar;
    ScrollBar* m_horizontalBar;
    PointF m_position;
    SizeF m_contentSize;
    bool m_horizontal = false;
    bool m_vertical = true;
    bool m_barsBelow = false;
    double m_cornerRadius = 0;
    double m_verticalBarTop;
    double m_verticalBarBottom;
    BarPolicy m_verticalPolicy = BarPolicy::AsNeeded;
};

} // namespace cutefish::ui
