// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <memory>
#include <utility>

#include "ui/view.h"

namespace cutefish::ui {

class HeaderButton;

// A window header: a bar that moves the window, toggles maximize on a
// double click, and holds the window buttons at its start (close, minimize,
// maximize), with room for a title or tools after them.
class WindowHeader : public View
{
public:
    explicit WindowHeader(double height = 40);

    // Fills the bar after the buttons.
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

    void setMinimizeVisible(bool visible);
    void setMaximizeVisible(bool visible);
    void setCloseVisible(bool visible);
    // From the top and the start, 4 by default and 8 in dialogs.
    void setButtonMargin(double margin);
    // The buttons centred in the bar's height, with no top margin, keeping the
    // start margin.
    void setButtonsCentered(bool centered);

    // The content spans the whole bar and lies under the buttons, for windows
    // whose sidebar runs beneath them.
    void setContentUnderButtons(bool under);
    // The width taken by the margin and the buttons, as of the last layout.
    double leadingInset() const { return m_leadingInset; }

    SizeF sizeHint() const override { return {0, m_height}; }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override;
    void pointerPress(const PointerEvent& event) override;

private:
    double m_height;
    double m_buttonMargin = 4;
    bool m_buttonsCentered = false;
    bool m_contentUnderButtons = false;
    double m_leadingInset = 0;
    View* m_content = nullptr;
    View* m_zone;
    HeaderButton* m_close;
    HeaderButton* m_minimize;
    HeaderButton* m_maximize;
};

} // namespace cutefish::ui
