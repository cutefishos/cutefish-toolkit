// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/list_view.h"

#include <algorithm>
#include <cmath>

namespace cutefish::ui {

// The scrolled content: as tall as all rows, holding only those in sight.
class ListView::Rows : public View
{
public:
    explicit Rows(ListView* list)
        : m_list(list)
    {
    }

    SizeF sizeHint() const override { return {0, heightForWidth(0)}; }

    double heightForWidth(double) const override
    {
        const ListView* list = m_list;
        const double rows = double(list->m_rowCount);
        const double spacing = list->m_rowCount > 1 ? list->m_spacing * (rows - 1) : 0;
        return list->m_margins.vertical() + rows * list->m_rowHeight + spacing;
    }

protected:
    void layout() override
    {
        for (const auto& [row, item] : m_list->m_items)
            item->setGeometry(m_list->rowRect(row));
    }

private:
    ListView* m_list;
};

ListView::ListView()
    : m_rows(setContent<Rows>(this))
{
}

ListView::~ListView() = default;

void ListView::setRowCount(size_t count)
{
    m_rowCount = count;
    m_rows->requestLayout();
    updateItems(true);
}

void ListView::setRowHeight(double height)
{
    if (height == m_rowHeight)
        return;
    // The row at the top stays there, as far into it as before.
    const double pitch = m_rowHeight + m_spacing;
    const double top = pitch > 0 ? std::max(0.0, scrollPosition().y - m_margins.top) / pitch : 0;
    m_rowHeight = height;
    m_rows->requestLayout();
    // The content's new height, before the position is set within it.
    ScrollView::layout();
    if (top > 0)
        setScrollPosition({scrollPosition().x, m_margins.top + top * (m_rowHeight + m_spacing)});
}

void ListView::setSpacing(double spacing)
{
    if (spacing == m_spacing)
        return;
    m_spacing = spacing;
    m_rows->requestLayout();
}

void ListView::setMargins(const Margins& margins)
{
    if (margins == m_margins)
        return;
    m_margins = margins;
    m_rows->requestLayout();
}

void ListView::reload()
{
    updateItems(true);
}

View* ListView::itemForRow(size_t row) const
{
    const auto it = m_items.find(row);
    return it == m_items.end() ? nullptr : it->second;
}

RectF ListView::rowRect(size_t row) const
{
    const double width = std::max(0.0, contentSize().width - m_margins.horizontal());
    return {m_margins.left, m_margins.top + double(row) * (m_rowHeight + m_spacing), width,
            m_rowHeight};
}

void ListView::scrollToRow(size_t row)
{
    if (row < m_rowCount)
        ensureVisible(rowRect(row).adjusted(0, -m_margins.top, 0, m_margins.bottom));
}

void ListView::positionChanged()
{
    updateItems(false);
}

void ListView::updateItems(bool rebind)
{
    if (!createItem)
        return;

    // The rows in sight, with the overscroll of a rubber band taken as the edge.
    const double pitch = m_rowHeight + m_spacing;
    const double top = std::max(0.0, scrollPosition().y - m_margins.top);
    const double bottom = std::max(0.0, scrollPosition().y + geometry().height - m_margins.top);
    size_t first = 0;
    size_t last = 0;
    if (m_rowCount > 0 && pitch > 0) {
        first = std::min(m_rowCount, size_t(std::floor(top / pitch)));
        last = std::min(m_rowCount, size_t(std::ceil(bottom / pitch)));
    }

    for (auto it = m_items.begin(); it != m_items.end();) {
        if (it->first < first || it->first >= last) {
            it->second->setVisible(false);
            m_spare.push_back(it->second);
            it = m_items.erase(it);
        } else {
            ++it;
        }
    }

    for (size_t row = first; row < last; ++row) {
        View* item = itemForRow(row);
        const bool fresh = !item;
        if (fresh) {
            if (!m_spare.empty()) {
                item = m_spare.back();
                m_spare.pop_back();
                item->setVisible(true);
            } else {
                std::unique_ptr<View> created = createItem();
                item = created.get();
                m_rows->addChild(std::move(created));
            }
            m_items.emplace(row, item);
            item->setGeometry(rowRect(row));
        }
        if ((fresh || rebind) && bindItem)
            bindItem(item, row);
    }
}

} // namespace cutefish::ui
