// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/color_provider.h"
#include "ui/menu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

#include "base/event_loop.h"
#include "ui/application.h"
#include "ui/embedded_window.h"
#include "ui/icon.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/popup_glass.h"
#include "ui/scroll_view.h"
#include "ui/theme.h"
#include "ui/view.h"
#include "ui/window.h"

namespace cutefish::ui {

// Row, separator and surface metrics.
static constexpr double kRowHeight = 30;
static constexpr double kRowSpacing = 1;
static constexpr double kSeparatorHeight = 5;
static constexpr double kMinimumRowWidth = 146;
static constexpr double kContentMargin = 3;
static constexpr double kVerticalContentMargin = 5;
// The highlight inside a row.
static constexpr double kHighlightInsetX = 3;
static constexpr double kHighlightInsetY = 1;
static constexpr double kRowPadding = 8;
static constexpr double kLabelPadding = 4;
static constexpr double kItemSpacing = 9;
static constexpr double kShortcutSpacing = 24;
static constexpr double kCheckSize = 12;
static constexpr double kCheckSpacing = 6;
static constexpr double kIconSize = 22;
static constexpr double kGlyphSize = 16;
// A row's chevron slot, and how far a submenu overlaps its menu's frame.
static constexpr double kArrowSize = 20;
static constexpr double kSubmenuOverlap = 2;
// The backstop for the pointer leaving towards nowhere.
static constexpr std::chrono::milliseconds kSubmenuDismissDelay{50};
// A menu keeps this far from the screen's edges.
static constexpr double kScreenMargin = 6;
static constexpr double kDropDownHeight = 240;
// A desktop menu's scroll indicator.
static constexpr double kIndicatorWidth = 3;
static constexpr double kIndicatorMinimumLength = 24;
// Letters typed within this long of each other make up one search.
static constexpr std::chrono::milliseconds kSearchTimeout{1000};

static std::string lowered(const std::string& text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    return result;
}

// The rows, in a ScrollView of the surface: MenuItems and MenuSeparators in
// a column with one pixel between them.
class Menu::Rows : public View
{
public:
    Rows(Menu* menu, bool fromKeyboard)
        : m_menu(menu)
        , m_keyboard(fromKeyboard)
    {
        for (const Item& item : menu->m_items) {
            if (item.visible && item.checked)
                m_checkColumn = true;
        }
        for (size_t i = 0; i < menu->m_items.size(); ++i) {
            const Item& item = menu->m_items[i];
            if (!item.visible)
                continue;
            Row row;
            row.index = i;
            if (!item.separator) {
                row.label = addChild<Label>(item.text);
                row.label->setVerticallyCentered(true);
                row.label->setFontFamily(item.fontFamily);
                if (item.checked)
                    row.check = addChild<Icon>("check", kCheckSize);
                if (!item.glyph.empty())
                    row.glyph = addChild<Icon>(item.glyph, kGlyphSize);
                else if (!item.icon.empty())
                    row.icon = addChild<ThemeIcon>(item.icon, kIconSize);
                if (!item.shortcut.empty()) {
                    row.shortcut = addChild<Label>(item.shortcut);
                    row.shortcut->setVerticallyCentered(true);
                }
                if (item.submenu)
                    row.arrow = addChild<Icon>("angle-right", kArrowSize);
            }
            m_rows.push_back(row);
        }
        // Opened from the keyboard a drop-down lights its chosen row, so
        // Return keeps it and the arrows move on from it.
        if (fromKeyboard && menu->m_dropDown)
            m_current = checkedRow();
        updateColors();
    }

    ~Rows() override
    {
        if (m_searchTimer)
            Application::instance()->loop().stopTimer(m_searchTimer);
    }

    void setScrollView(ScrollView* view) { m_scrollView = view; }

    SizeF sizeHint() const override
    {
        double width = 0;
        double height = 0;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const Row& row = m_rows[i];
            height += (i > 0 ? kRowSpacing : 0) + (row.label ? kRowHeight : kSeparatorHeight);
            if (row.label)
                width = std::max(width, rowWidth(row));
        }
        return {width, height};
    }

    bool acceptsPointer() const override { return true; }

    int checkedRow() const
    {
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (m_rows[i].label && m_menu->m_items[m_rows[i].index].checked)
                return int(i);
        }
        return -1;
    }

    // The row scrolled to the middle of the view, once laid out.
    void centreOnOpen(int row) { m_centreRow = row; }

    void pointerMoved(PointF windowPoint)
    {
        m_pointer = windowPoint;
        m_pointerInside = true;
        const int row = rowAt(mapFromWindow(windowPoint));
        // After a key, motion elsewhere leaves the highlight alone until the
        // pointer comes back over the list.
        if (m_keyboard && row < 0 && m_menu->m_dropDown)
            return;
        m_keyboard = false;
        setCurrent(row);
        // Another row, or none, takes the submenu down; a row with one opens it.
        if (m_menu->m_openSubmenu && row != submenuRow())
            m_menu->closeSubmenu();
        if (row >= 0 && m_menu->m_items[m_rows[size_t(row)].index].submenu)
            openSubmenu(row, false);
    }

    void pointerLeft()
    {
        m_pointerInside = false;
        // A row whose submenu is open stays lit while the pointer goes over.
        if (m_menu->m_openSubmenu) {
            setCurrent(submenuRow());
            m_menu->scheduleSubmenuCheck();
        } else if (!m_keyboard) {
            setCurrent(-1);
        }
        if (m_menu->m_parentMenu)
            m_menu->m_parentMenu->scheduleSubmenuCheck();
    }

    bool pointerInside() const { return m_pointerInside; }
    int current() const { return m_current; }
    bool isKeyboardDriven() const { return m_keyboard; }
    // The lit row's item, or -1.
    int currentItem() const { return m_current >= 0 ? int(m_rows[size_t(m_current)].index) : -1; }
    void setCurrentItem(size_t item, bool keyboard)
    {
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (m_rows[i].index == item && m_rows[i].label) {
                m_keyboard = keyboard;
                setCurrent(int(i));
                updateColors();
                return;
            }
        }
    }

    // The row the open submenu hangs off, or -1.
    int submenuRow() const
    {
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (m_menu->m_openSubmenu && m_menu->m_items[m_rows[i].index].submenu.get() == m_menu->m_openSubmenu)
                return int(i);
        }
        return -1;
    }

    // MenuPopupWindow's submenu placement: against the menu's frame, two
    // pixels over it, its first row level with the row; mirrored to the
    // other side when it does not fit, slid to stay on screen.
    void openSubmenu(int row, bool fromKeyboard)
    {
        const Item& item = m_menu->m_items[m_rows[size_t(row)].index];
        Menu* submenu = item.submenu.get();
        if (!submenu || !item.enabled || (submenu == m_menu->m_openSubmenu && submenu->isOpen()))
            return;
        Window* window = this->window();
        if (!window)
            return;
        m_menu->closeSubmenu();
        const double top = mapToWindow({0, m_rows[size_t(row)].rect.y}).y;
        PopupPlacement placement;
        placement.anchorRect = {kSubmenuOverlap, top - kVerticalContentMargin,
                                std::max(1.0, window->size().width - 2 * kSubmenuOverlap), kRowHeight};
        placement.anchor = PopupAnchor::TopRight;
        placement.gravity = PopupAnchor::BottomRight;
        placement.flipX = true;
        placement.flipY = false;
        placement.slide = true;
        submenu->open(window, placement, fromKeyboard, false);
        if (!submenu->isOpen())
            return;
        m_menu->m_openSubmenu = submenu;
        m_menu->m_submenuFromKeyboard = fromKeyboard;
        if (fromKeyboard) {
            m_keyboard = true;
            if (Rows* rows = submenu->rows())
                rows->step(1);
        }
        setCurrent(row);
        updateColors();
        requestPaint();
    }

    void submenuClosed()
    {
        if (!m_pointerInside && !m_keyboard)
            setCurrent(-1);
        updateColors();
        requestPaint();
    }

    // The view moved under a still pointer.
    void scrolled()
    {
        if (m_pointerInside && !m_keyboard)
            setCurrent(rowAt(mapFromWindow(m_pointer)));
    }

    bool key(const KeyEvent& event)
    {
        // The innermost open menu takes the keys.
        if (m_menu->m_openSubmenu) {
            if (Rows* rows = m_menu->m_openSubmenu->rows())
                return rows->key(event);
        }
        return m_menu->m_dropDown ? dropDownKey(event) : desktopKey(event);
    }

protected:
    void layout() override
    {
        double y = 0;
        const double width = geometry().width;
        for (Row& row : m_rows) {
            if (!row.label) {
                row.rect = {0, y, width, kSeparatorHeight};
                y += kSeparatorHeight + kRowSpacing;
                continue;
            }
            row.rect = {0, y, width, kRowHeight};
            const double lead = labelPadding();
            if (row.check) {
                row.check->setGeometry({row.rect.x + kRowPadding, y + std::round((kRowHeight - kCheckSize) / 2),
                                        kCheckSize, kCheckSize});
            }
            if (row.glyph) {
                row.glyph->setGeometry({row.rect.x + kRowPadding + lead, y + std::round((kRowHeight - kGlyphSize) / 2),
                                        kGlyphSize, kGlyphSize});
            }
            if (row.icon) {
                row.icon->setGeometry({row.rect.x + kRowPadding + lead, y + std::round((kRowHeight - kIconSize) / 2),
                                       kIconSize, kIconSize});
            }
            double right = row.rect.right() - kRowPadding;
            if (row.arrow) {
                row.arrow->setGeometry({right - kArrowSize, y + std::round((kRowHeight - kArrowSize) / 2), kArrowSize,
                                        kArrowSize});
                right -= kArrowSize + kItemSpacing;
            }
            if (row.shortcut) {
                const double shortcutWidth = row.shortcut->sizeHint().width;
                row.shortcut->setGeometry({right - shortcutWidth, y, shortcutWidth, kRowHeight});
                right -= shortcutWidth + kShortcutSpacing;
            }
            const double left = row.rect.x + kRowPadding + lead + glyphPadding(row);
            row.label->setGeometry({left, y, std::max(0.0, right - left), kRowHeight});
            y += kRowHeight + kRowSpacing;
        }

        if (m_centreRow >= 0 && m_scrollView) {
            const RectF& rect = m_rows[size_t(m_centreRow)].rect;
            m_centreRow = -1;
            const double viewport = m_scrollView->geometry().height;
            m_scrollView->setScrollPosition({0, rect.y + rect.height / 2 - viewport / 2});
        }
    }

    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const Row& row = m_rows[i];
            if (!row.label) {
                // One device pixel, centred, kept on the pixel grid.
                const Color line = color(ColorId::MenuSeparator);
                const double height = 1 / painter.scale();
                painter.drawRoundedRect({row.rect.x + theme.largeSpacing, row.rect.y + (kSeparatorHeight - height) / 2,
                                         row.rect.width - 2 * theme.largeSpacing, height},
                                        {0}, line);
                continue;
            }
            if (int(i) == m_current && m_menu->m_items[row.index].enabled) {
                const RectF highlight = row.rect.adjusted(kHighlightInsetX, kHighlightInsetY,
                                                          -kHighlightInsetX, -kHighlightInsetY);
                painter.drawRoundedRect(highlight, {theme.smallRadius, 0, themeCorners()},
                                        color(ColorId::MenuHighlight));
            }
        }
    }

    void pointerMove(const PointerEvent& event) override { pointerMoved(mapToWindow(event.position)); }
    void pointerPress(const PointerEvent& event) override { pointerMoved(mapToWindow(event.position)); }
    void pointerLeave() override { pointerLeft(); }

    void pointerRelease(const PointerEvent& event) override
    {
        // Choosing is on release, over the row the pointer is on then; a row
        // with a submenu only opens it.
        const int row = rowAt(event.position);
        if (row >= 0)
            choose(row, false);
    }

private:
    struct Row
    {
        size_t index = 0;
        Label* label = nullptr;
        Label* shortcut = nullptr;
        Icon* check = nullptr;
        Icon* glyph = nullptr;
        ThemeIcon* icon = nullptr;
        Icon* arrow = nullptr;
        RectF rect;
    };

    void choose(int row, bool fromKeyboard)
    {
        if (m_menu->m_items[m_rows[size_t(row)].index].submenu)
            openSubmenu(row, fromKeyboard);
        else
            m_menu->activate(m_rows[size_t(row)].index);
    }

    double labelPadding() const { return m_checkColumn ? kCheckSize + kCheckSpacing : kLabelPadding; }

    double glyphPadding(const Row& row) const
    {
        if (row.glyph)
            return kGlyphSize + kItemSpacing;
        if (row.icon)
            return kIconSize + kItemSpacing;
        return 0;
    }

    // A row's own width; a drop-down measures its rows in the menu's font.
    double rowWidth(const Row& row) const
    {
        const double maximum = m_menu->m_items[row.index].maximumWidth;
        const double width = naturalRowWidth(row);
        return maximum > 0 ? std::min(width, maximum) : width;
    }

    double naturalRowWidth(const Row& row) const
    {
        double text = row.label->sizeHint().width;
        const Item& item = m_menu->m_items[row.index];
        if (!item.fontFamily.empty()) {
            Label probe(item.text);
            probe.setTextLayoutMode(row.label->textLayoutMode());
            text = probe.sizeHint().width;
        }
        double width = labelPadding() + glyphPadding(row) + text;
        if (row.shortcut)
            width += row.shortcut->sizeHint().width + kShortcutSpacing;
        if (row.arrow)
            width += kArrowSize + kItemSpacing;
        return std::max(kMinimumRowWidth, std::ceil(width) + 2 * kRowPadding);
    }

    bool selectable(int row) const
    {
        return row >= 0 && row < int(m_rows.size()) && m_rows[size_t(row)].label
                && m_menu->m_items[m_rows[size_t(row)].index].enabled;
    }

    int rowAt(PointF point) const
    {
        if (!localRect().contains(point))
            return -1;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (point.y >= m_rows[i].rect.y && point.y < m_rows[i].rect.bottom())
                return m_rows[i].label ? int(i) : -1;
        }
        return -1;
    }

    // A desktop menu's keys: the arrows wrap round the enabled rows.
    bool desktopKey(const KeyEvent& event)
    {
        switch (event.key) {
        case key::Up:
            step(-1);
            return true;
        case key::Down:
            step(1);
            return true;
        case key::Home:
            m_current = -1;
            step(1);
            return true;
        case key::End:
            m_current = -1;
            step(-1);
            return true;
        case key::Return:
        case key::KeypadEnter:
            if (selectable(m_current))
                choose(m_current, true);
            else if (m_current < 0)
                m_menu->rootMenu()->close();
            return true;
        case key::Space:
            if (selectable(m_current))
                choose(m_current, true);
            return true;
        case key::Right:
            if (selectable(m_current) && m_menu->m_items[m_rows[size_t(m_current)].index].submenu)
                openSubmenu(m_current, true);
            else if (!m_menu->m_parentMenu && m_menu->onNavigateRight)
                m_menu->onNavigateRight();
            return true;
        case key::Left:
            if (m_menu->m_parentMenu)
                m_menu->close();
            else if (m_menu->onNavigateLeft)
                m_menu->onNavigateLeft();
            return true;
        case key::Escape:
            m_menu->close();
            return true;
        default:
            return true;
        }
    }

    // A drop-down's keys: the arrows stop at the ends, and typing finds rows.
    bool dropDownKey(const KeyEvent& event)
    {
        switch (event.key) {
        case key::Down:
            moveHighlight(1);
            return true;
        case key::Up:
            moveHighlight(-1);
            return true;
        case key::PageDown:
            moveHighlight(pageStep());
            return true;
        case key::PageUp:
            moveHighlight(-pageStep());
            return true;
        case key::Home:
            setHighlight(selectableFrom(0, 1));
            return true;
        case key::End:
            setHighlight(selectableFrom(int(m_rows.size()) - 1, -1));
            return true;
        case key::Return:
        case key::KeypadEnter:
            activateHighlighted();
            return true;
        case key::Space:
            // Part of a name being typed, such as "Noto Sans".
            if (!m_search.empty())
                search(" ");
            else
                activateHighlighted();
            return true;
        case key::Escape:
        case key::Tab:
            m_menu->close();
            return true;
        default:
            if (!event.text.empty() && static_cast<unsigned char>(event.text[0]) >= 0x20
                && !(event.modifiers & (ControlModifier | AltModifier | SuperModifier)))
                search(event.text);
            return true;
        }
    }

    void step(int direction)
    {
        m_keyboard = true;
        int row = m_current;
        if (row < 0)
            row = direction > 0 ? -1 : int(m_rows.size());
        for (size_t tries = 0; tries < m_rows.size(); ++tries) {
            row = (row + direction + int(m_rows.size()) * 2) % int(m_rows.size());
            if (selectable(row)) {
                setCurrent(row);
                ensureVisible(row);
                return;
            }
        }
    }

    // The first enabled row from a row on in a direction, or -1.
    int selectableFrom(int row, int direction) const
    {
        for (; row >= 0 && row < int(m_rows.size()); row += direction) {
            if (selectable(row))
                return row;
        }
        return -1;
    }

    void setHighlight(int row)
    {
        if (row < 0)
            return;
        m_keyboard = true;
        setCurrent(row);
        ensureVisible(row);
    }

    // Keys move on from the lit row, or from the chosen one while the
    // pointer has lit none, past separators and disabled rows.
    void moveHighlight(int step)
    {
        if (m_rows.empty())
            return;
        const int direction = step > 0 ? 1 : -1;
        const int from = m_current >= 0 ? m_current : checkedRow();
        if (from < 0) {
            setHighlight(selectableFrom(step > 0 ? 0 : int(m_rows.size()) - 1, direction));
            return;
        }
        const int target = std::clamp(from + step, 0, int(m_rows.size()) - 1);
        const int row = selectableFrom(target, direction);
        setHighlight(row >= 0 ? row : selectableFrom(target, -direction));
    }

    int pageStep() const
    {
        const double viewport = m_scrollView ? m_scrollView->geometry().height : 0;
        return std::max(1, int(std::floor(viewport / (kRowHeight + kRowSpacing))) - 1);
    }

    void activateHighlighted()
    {
        if (selectable(m_current))
            m_menu->activate(m_rows[size_t(m_current)].index);
        else
            m_menu->close();
    }

    // Typing lights the next row that starts with what was typed; the same
    // letter again steps through the rows starting with it.
    void search(const std::string& typed)
    {
        m_search += lowered(typed);
        auto& loop = Application::instance()->loop();
        if (m_searchTimer)
            loop.stopTimer(m_searchTimer);
        m_searchTimer = loop.startTimer(kSearchTimeout, false, [this] {
            m_searchTimer = 0;
            m_search.clear();
        });

        const bool repeated = std::all_of(m_search.begin(), m_search.end(),
                                          [this](char c) { return c == m_search.front(); });
        const std::string prefix = repeated ? m_search.substr(0, 1) : m_search;
        const int start = m_current >= 0 ? m_current : checkedRow();
        const int first = start < 0 ? 0 : start + (repeated ? 1 : 0);
        const int count = int(m_rows.size());
        for (int i = 0; i < count; ++i) {
            const int row = (first + i) % count;
            if (selectable(row) && lowered(m_menu->m_items[m_rows[size_t(row)].index].text).starts_with(prefix)) {
                setHighlight(row);
                return;
            }
        }
    }

    void ensureVisible(int row)
    {
        if (m_scrollView && row >= 0)
            m_scrollView->ensureVisible(m_rows[size_t(row)].rect);
    }

    void setCurrent(int row)
    {
        if (row == m_current)
            return;
        m_current = row;
        updateColors();
        requestPaint();
    }

    void updateColors()
    {
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const Row& row = m_rows[i];
            if (!row.label)
                continue;
            const bool enabled = m_menu->m_items[row.index].enabled;
            const bool active = enabled && (int(i) == m_current
                                            || (m_menu->m_openSubmenu
                                                && m_menu->m_items[row.index].submenu.get() == m_menu->m_openSubmenu));
            const std::optional<ColorRecipe> text = !enabled ? std::optional<ColorRecipe>(ColorId::MenuDisabledText)
                                                    : active ? std::optional<ColorRecipe>(ColorId::HighlightedText)
                                                             : std::nullopt;
            row.label->setColor(text);
            if (row.check)
                row.check->setColor(text);
            if (row.glyph)
                row.glyph->setColor(text);
            if (row.shortcut)
                row.shortcut->setColor(!enabled ? ColorId::MenuDisabledText
                                       : active ? ColorId::HighlightedText
                                                : ColorId::MenuShortcutText);
            if (row.arrow)
                row.arrow->setColor(active    ? ColorId::HighlightedText
                                    : enabled ? ColorId::MenuArrow
                                              : ColorId::MenuArrowDisabled);
        }
    }

    Menu* m_menu;
    ScrollView* m_scrollView = nullptr;
    std::vector<Row> m_rows;
    bool m_checkColumn = false;
    int m_current = -1;
    // The highlight follows the keyboard until the pointer moves over a row.
    bool m_keyboard = false;
    bool m_pointerInside = false;
    PointF m_pointer;
    int m_centreRow = -1;
    std::string m_search;
    int m_searchTimer = 0;
};

// A desktop menu's scroll indicator, drawn over the rows of a menu that scrolls.
class ScrollIndicator : public View
{
public:
    explicit ScrollIndicator(const ScrollView* view)
        : m_view(view)
    {
    }

protected:
    void paint(Painter& painter) override
    {
        const RectF view = m_view->geometry();
        const double content = m_view->contentSize().height;
        if (content <= view.height)
            return;
        const double length = std::max(kIndicatorMinimumLength, view.height * view.height / content);
        const RectF bar = {view.right() - kIndicatorWidth,
                           view.y + view.height * (m_view->scrollPosition().y / content), kIndicatorWidth, length};
        painter.drawSmoothRectangle(bar, kIndicatorWidth / 2, color(ColorId::MenuScrollIndicator));
    }

private:
    const ScrollView* m_view;
};

// The popup's content: the background and the scrolling rows, with a
// drop-down's scroll bar or a desktop menu's indicator.
class Menu::Surface : public View
{
public:
    Surface(Menu* menu, bool fromKeyboard)
        : m_menu(menu)
        , m_scroll(addChild<ScrollView>())
    {
        m_rows = m_scroll->setContent<Rows>(menu, fromKeyboard);
        m_rows->setScrollView(m_scroll);
        m_scroll->setElastic(false);
        m_scroll->setVerticalBarPolicy(menu->m_dropDown ? ScrollView::BarPolicy::AlwaysOn
                                                        : ScrollView::BarPolicy::AlwaysOff);
        if (!menu->m_dropDown)
            m_indicator = addChild<ScrollIndicator>(m_scroll);
        m_scroll->onScrolled = [this] {
            m_rows->scrolled();
            if (m_indicator)
                m_indicator->requestPaint();
        };
        // The row under the pointer is lit whether or not a button is held.
        setTracksHover(true);
    }

    // MenuSurface's implicit size: the arrow's length added across its edge,
    // and the edge it stands on at least long enough to show it.
    SizeF sizeHint() const override
    {
        const SizeF rows = m_rows->sizeHint();
        const BubbleShape shape = m_menu->bubble({}, 0);
        const bool across = shape.arrowEdge == BubbleShape::Edge::Left || shape.arrowEdge == BubbleShape::Edge::Right;
        const bool along = shape.arrowEdge == BubbleShape::Edge::Top || shape.arrowEdge == BubbleShape::Edge::Bottom;
        return {std::max({m_menu->m_minimumWidth, rows.width + 2 * kContentMargin + (across ? shape.arrowLength : 0),
                          along ? shape.minimumEdgeLength() : 0}),
                std::max(rows.height + 2 * kVerticalContentMargin + (along ? shape.arrowLength : 0),
                         across ? shape.minimumEdgeLength() : 0)};
    }

    Rows* rows() const { return m_rows; }

    void setArrowPosition(double position) { m_arrowPosition = position; }
    BubbleShape shape() const { return m_menu->bubble(geometry().size(), m_arrowPosition); }
    // Painted as a path rather than a rounded rect.
    bool isShaped() const { return m_menu->m_arrowEdge != BubbleShape::Edge::None || m_menu->m_continuousCorners; }

    bool acceptsPointer() const override { return true; }
    bool acceptsFocus() const override { return true; }

protected:
    void layout() override
    {
        const RectF body = shape().bodyRect();
        m_scroll->setGeometry(body.adjusted(kContentMargin, kVerticalContentMargin, -kContentMargin,
                                            -kVerticalContentMargin));
        if (m_indicator)
            m_indicator->setGeometry(body);
    }

    void paint(Painter& painter) override
    {
        const Theme& theme = Theme::current();
        // A bubble paints its colour only without glass; an
        // embedded host draws the glass, tint included, into blurRect().
        if (Application::instance()->platform() == Application::Platform::Embedded && EmbeddedWindow::host())
            return;
        if (m_glass.update(*window()))
            return;
        // Without the compositor's blur: opaque, in the window radius.
        if (isShaped())
            painter.fillPath(shape().path(), theme.secondBackground);
        else
            painter.fillRoundedRect(localRect(), theme.windowRadius, theme.secondBackground);
    }

    void pointerMove(const PointerEvent& event) override { m_rows->pointerMoved(mapToWindow(event.position)); }
    void hoverMove(const PointerEvent& event) override { m_rows->pointerMoved(mapToWindow(event.position)); }
    void hoverChanged() override
    {
        if (!isHovered())
            m_rows->pointerLeft();
    }
    bool keyPress(const KeyEvent& event) override { return m_rows->key(event); }

private:
    Menu* m_menu;
    ScrollView* m_scroll;
    Rows* m_rows = nullptr;
    ScrollIndicator* m_indicator = nullptr;
    double m_arrowPosition = 0;
    PopupGlass m_glass;
};

Menu::Menu() = default;

Menu::~Menu()
{
    if (m_submenuTimer)
        Application::instance()->loop().stopTimer(m_submenuTimer);
    if (m_parentMenu && m_parentMenu->m_openSubmenu == this)
        m_parentMenu->m_openSubmenu = nullptr;
    m_window.reset();
}

size_t Menu::addItem(std::string text, std::function<void()> action, std::string shortcut)
{
    Item item;
    item.text = std::move(text);
    item.shortcut = std::move(shortcut);
    item.action = std::move(action);
    m_items.push_back(std::move(item));
    changed();
    return m_items.size() - 1;
}

void Menu::addSeparator()
{
    Item item;
    item.separator = true;
    m_items.push_back(std::move(item));
    changed();
}

Menu* Menu::addSubmenu(std::string text)
{
    Item item;
    item.text = std::move(text);
    item.submenu = std::make_unique<Menu>();
    item.submenu->m_parentMenu = this;
    m_items.push_back(std::move(item));
    changed();
    return m_items.back().submenu.get();
}

Menu* Menu::submenu(size_t index) const
{
    return index < m_items.size() ? m_items[index].submenu.get() : nullptr;
}

void Menu::setItemText(size_t index, std::string text)
{
    if (index < m_items.size()) {
        m_items[index].text = std::move(text);
        itemsChanged();
        changed();
    }
}

void Menu::setItemEnabled(size_t index, bool enabled)
{
    if (index < m_items.size() && m_items[index].enabled != enabled) {
        m_items[index].enabled = enabled;
        changed();
    }
}

void Menu::setItemVisible(size_t index, bool visible)
{
    if (index < m_items.size() && m_items[index].visible != visible) {
        m_items[index].visible = visible;
        changed();
    }
}

void Menu::setItemChecked(size_t index, bool checked)
{
    setItemState(index, checked ? MenuItemState::On : MenuItemState::Off);
}

void Menu::setItemState(size_t index, MenuItemState state)
{
    if (index >= m_items.size())
        return;
    Item& item = m_items[index];
    const bool checked = state == MenuItemState::On;
    const bool mixed = state == MenuItemState::Mixed;
    if (item.checked == checked && item.mixed == mixed)
        return;
    item.checked = checked;
    item.mixed = mixed;
    changed();
}

static std::string keyEquivalentText(const std::string& key, uint32_t modifiers)
{
    if (key.empty())
        return {};
    std::string text;
    if (modifiers & ControlModifier)
        text += "Ctrl+";
    if (modifiers & AltModifier)
        text += "Alt+";
    if (modifiers & ShiftModifier)
        text += "Shift+";
    if (modifiers & SuperModifier)
        text += "Super+";
    if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
        text += char(key[0] - 'a' + 'A');
    else
        text += key;
    return text;
}

void Menu::setItemShortcut(size_t index, std::string text)
{
    if (index >= m_items.size() || m_items[index].shortcut == text)
        return;
    m_items[index].shortcut = std::move(text);
    changed();
}

void Menu::setItemKeyEquivalent(size_t index, std::string key, uint32_t modifiers)
{
    if (index >= m_items.size())
        return;
    Item& item = m_items[index];
    if (item.keyEquivalent == key && item.keyModifiers == modifiers)
        return;
    item.shortcut = keyEquivalentText(key, modifiers);
    item.keyEquivalent = std::move(key);
    item.keyModifiers = modifiers;
    changed();
}

void Menu::setItemIcon(size_t index, std::string icon)
{
    if (index < m_items.size() && m_items[index].icon != icon) {
        m_items[index].icon = std::move(icon);
        changed();
    }
}

void Menu::setRole(MenuRole role)
{
    if (role == m_role)
        return;
    m_role = role;
    changed();
}

void Menu::changed()
{
    if (m_changed)
        m_changed();
}

void Menu::setItemGlyph(size_t index, std::string glyph)
{
    if (index < m_items.size())
        m_items[index].glyph = std::move(glyph);
}

void Menu::setItemMaximumWidth(size_t index, double width)
{
    if (index < m_items.size())
        m_items[index].maximumWidth = width;
}

void Menu::setArrow(BubbleShape::Edge edge, PointF target)
{
    m_arrowEdge = edge;
    m_arrowTarget = target;
}

BubbleShape Menu::bubble(SizeF size, double arrowPosition) const
{
    BubbleShape shape;
    shape.width = size.width;
    shape.height = size.height;
    shape.radius = Theme::current().windowRadius;
    shape.continuousCorners = m_continuousCorners;
    shape.arrowEdge = m_arrowEdge;
    shape.arrowPosition = arrowPosition;
    return shape;
}

double Menu::minimumEdgeLength() const
{
    return bubble({}, 0).minimumEdgeLength();
}

SizeF Menu::popupSize(Window* parent, SizeF content) const
{
    SizeF size = {std::ceil(content.width), std::ceil(content.height)};
    // A drop-down is at most half the screen tall, and a desktop menu the
    // screen, so long lists scroll.
    const SizeF screen = parent->screenSize();
    const double height = m_availableGeometry ? m_availableGeometry->height : screen.height;
    if (height > 0) {
        const double available = height - 2 * kScreenMargin;
        const double maximum = m_dropDown ? std::max(std::min(available, kDropDownHeight), std::round(available * 0.5))
                                          : available;
        size.height = std::min(size.height, maximum);
    }
    return size;
}

SizeF Menu::implicitSize(Window* window)
{
    Surface probe(this, false);
    probe.setTextLayoutMode(window->textLayoutMode());
    return popupSize(window, probe.sizeHint());
}

void Menu::popupAt(Window* window, PointF topLeft)
{
    // Once per opening, before measuring, so rows it adds count.
    close();
    if (onAboutToShow)
        onAboutToShow();
    m_aboutToShowDone = true;
    const SizeF size = implicitSize(window);
    // MenuPopupWindow::popupPosition on whole pixels, its right and bottom
    // the last pixel inside, as an integer rect has them.
    double x = std::round(topLeft.x);
    double y = std::round(topLeft.y);
    const SizeF screen = window->screenSize();
    const RectF area = m_availableGeometry.value_or(RectF{0, 0, screen.width, screen.height});
    if (area.width > 0 && area.height > 0) {
        const double right = area.x + area.width - 1;
        const double bottom = area.y + area.height - 1;
        if (x + size.width > right - kScreenMargin)
            x = right - size.width - kScreenMargin;
        if (y + size.height > bottom - kScreenMargin)
            y = bottom - kScreenMargin - size.height;
        x = std::max(area.x + kScreenMargin, std::min(x, right - kScreenMargin - size.width));
        y = std::max(area.y + kScreenMargin, y);
    }
    PopupPlacement placement;
    placement.anchorRect = {x, y, 1, 1};
    placement.anchor = PopupAnchor::TopLeft;
    placement.gravity = PopupAnchor::BottomRight;
    placement.flipX = false;
    placement.flipY = false;
    m_openAt = PointF{x, y};
    open(window, placement, false);
    m_openAt.reset();
    m_aboutToShowDone = false;
}

void Menu::setItemFontFamily(size_t index, std::string family)
{
    if (index < m_items.size())
        m_items[index].fontFamily = std::move(family);
}

void Menu::clear()
{
    closeSubmenu();
    m_items.clear();
    changed();
}

void Menu::popup(Window* window, PointF position)
{
    PopupPlacement placement;
    placement.anchorRect = {position.x, position.y, 1, 1};
    placement.anchor = PopupAnchor::TopLeft;
    placement.gravity = PopupAnchor::BottomRight;
    open(window, placement, false);
}

void Menu::popupFor(View* view, double gap, std::optional<RectF> rect, bool fromKeyboard)
{
    Window* window = view->window();
    if (!window)
        return;
    const RectF local = rect.value_or(view->localRect());
    const PointF topLeft = view->mapToWindow({local.x, local.y});
    // MenuPopupWindow's anchor: the item's rect on whole pixels, gap above
    // and below it; the compositor flips it above when it does not fit.
    const double x = std::round(topLeft.x);
    const double y = std::round(topLeft.y);
    PopupPlacement placement;
    placement.anchorRect = {x, y - gap, std::ceil(local.width), std::ceil(local.height) + 2 * gap};
    placement.anchor = PopupAnchor::BottomLeft;
    placement.gravity = PopupAnchor::BottomRight;
    placement.flipX = false;
    open(window, placement, fromKeyboard);
}

void Menu::open(Window* parent, const PopupPlacement& placement, bool fromKeyboard, bool grab)
{
    close();
    if (onAboutToShow && !m_aboutToShowDone)
        onAboutToShow();
    // A submenu without a visible row would map as an empty frame.
    if (m_parentMenu && !onAboutToShow
        && std::none_of(m_items.begin(), m_items.end(), [](const Item& item) { return item.visible; }))
        return;
    m_window = Window::createPopup(parent, placement, grab);
    auto content = std::make_unique<Surface>(this, fromKeyboard);
    Surface* surface = content.get();
    // Measured in the popup, at its scale: text measured elsewhere at scale 1
    // takes rounded advances and comes out narrower than drawn.
    m_window->setContentView(std::move(content));
    const SizeF size = popupSize(parent, surface->sizeHint());
    // The arrow follows the target from where the menu opens.
    const PointF origin = m_openAt.value_or(PointF{placement.anchorRect.x, placement.anchorRect.y});
    const bool across = m_arrowEdge == BubbleShape::Edge::Left || m_arrowEdge == BubbleShape::Edge::Right;
    surface->setArrowPosition(across ? m_arrowTarget.y - origin.y : m_arrowTarget.x - origin.x);
    if (m_dropDown)
        surface->rows()->centreOnOpen(surface->rows()->checkedRow());

    m_window->resize(size);
    PopupGlass::prepare(*m_window);
    const double arrowPosition = surface->shape().arrowPosition;
    applyBlur(size, arrowPosition);
    m_shownItems = itemsSignature();
    m_window->onCloseRequested = [this] { close(); };
    surface->setFocus(fromKeyboard ? FocusReason::Keyboard : FocusReason::Other);
    m_window->show();
}

void Menu::close()
{
    if (!m_window)
        return;
    closeSubmenu();
    if (m_submenuTimer) {
        Application::instance()->loop().stopTimer(m_submenuTimer);
        m_submenuTimer = 0;
    }
    // A row closes the menu from inside the popup's own event handling; the
    // window goes once that is over, and off screen now.
    std::shared_ptr<Window> window(std::move(m_window));
    window->hide();
    Application::instance()->loop().post([window] {});
    if (m_parentMenu && m_parentMenu->m_openSubmenu == this) {
        m_parentMenu->m_openSubmenu = nullptr;
        if (Rows* rows = m_parentMenu->rows())
            rows->submenuClosed();
    }
    if (onClosed)
        onClosed();
}

void Menu::activate(size_t index)
{
    if (index >= m_items.size() || !m_items[index].enabled)
        return;
    const std::function<void()> action = m_items[index].action;
    // Choosing a row closes the whole chain.
    rootMenu()->close();
    if (action)
        action();
}

void Menu::applyBlur(SizeF size, double arrowPosition)
{
    if (m_arrowEdge != BubbleShape::Edge::None || m_continuousCorners) {
        // The glass is the body, masked by the whole outline.
        const BubbleShape shape = bubble(size, arrowPosition);
        m_window->setBackgroundBlur(shape.bodyRect(), Theme::current().windowRadius);
        m_window->setBackgroundBlurShape(shape.path());
    } else {
        m_window->setBackgroundBlur({0, 0, size.width, size.height}, Theme::current().windowRadius);
    }
}

std::string Menu::itemsSignature() const
{
    std::string signature;
    for (const Item& item : m_items) {
        signature += item.text + '\x1f' + item.shortcut + '\x1f' + item.icon + '\x1f' + item.glyph + '\x1f'
                + item.fontFamily + '\x1f' + std::to_string(reinterpret_cast<uintptr_t>(item.submenu.get())) + '\x1f'
                + std::to_string(item.maximumWidth) + char('0' + item.separator) + char('0' + item.enabled)
                + char('0' + item.visible) + char('0' + item.checked) + '\x1e';
    }
    return signature;
}

void Menu::itemsChanged()
{
    if (!m_window || m_rebuildPosted)
        return;
    m_rebuildPosted = true;
    // Rows may be calling in from their own event handling.
    Application::instance()->loop().post([alive = std::weak_ptr<bool>(m_alive), this] {
        if (alive.expired())
            return;
        m_rebuildPosted = false;
        rebuild();
    });
}

void Menu::rebuild()
{
    if (!m_window || !m_window->contentView())
        return;
    const std::string signature = itemsSignature();
    if (signature == m_shownItems)
        return;
    m_shownItems = signature;
    const auto* previous = static_cast<const Surface*>(m_window->contentView());
    const int currentItem = previous->rows()->currentItem();
    const bool keyboard = previous->rows()->isKeyboardDriven();
    const double arrowPosition = previous->shape().arrowPosition;
    if (m_openSubmenu && std::none_of(m_items.begin(), m_items.end(), [this](const Item& item) {
            return item.visible && item.submenu.get() == m_openSubmenu;
        }))
        closeSubmenu();

    auto content = std::make_unique<Surface>(this, keyboard);
    Surface* surface = content.get();
    surface->setArrowPosition(arrowPosition);
    Window* parent = m_window->parentWindow();
    m_window->setContentView(std::move(content));
    const SizeF size = popupSize(parent ? parent : m_window.get(), surface->sizeHint());
    m_window->resize(size);
    applyBlur(size, arrowPosition);
    surface->setFocus(keyboard ? FocusReason::Keyboard : FocusReason::Other);
    if (currentItem >= 0)
        surface->rows()->setCurrentItem(size_t(currentItem), keyboard);
}

Menu* Menu::rootMenu()
{
    Menu* menu = this;
    while (menu->m_parentMenu)
        menu = menu->m_parentMenu;
    return menu;
}

Menu::Rows* Menu::rows() const
{
    if (!m_window || !m_window->contentView())
        return nullptr;
    return static_cast<Surface*>(m_window->contentView())->rows();
}

void Menu::closeSubmenu()
{
    if (m_openSubmenu)
        m_openSubmenu->close();
    m_openSubmenu = nullptr;
}

void Menu::checkSubmenuHover()
{
    if (!m_openSubmenu || m_submenuFromKeyboard)
        return;
    const Rows* submenuRows = m_openSubmenu->rows();
    if (submenuRows && submenuRows->pointerInside())
        return;
    const Rows* own = rows();
    if (own && own->pointerInside() && own->current() == own->submenuRow())
        return;
    closeSubmenu();
}

void Menu::scheduleSubmenuCheck()
{
    auto& loop = Application::instance()->loop();
    if (m_submenuTimer)
        loop.stopTimer(m_submenuTimer);
    m_submenuTimer = loop.startTimer(kSubmenuDismissDelay, false, [this] {
        m_submenuTimer = 0;
        checkSubmenuHover();
    });
}

} // namespace cutefish::ui
