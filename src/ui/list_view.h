// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ui/scroll_view.h"

namespace cutefish::ui {

// A scrolling list of rows of one height. Only the rows in sight have views:
// they are made by createItem, filled by bindItem, and reused for other rows
// as the list scrolls.
class ListView : public ScrollView
{
public:
    ListView();
    ~ListView() override;

    std::function<std::unique_ptr<View>()> createItem;
    std::function<void(View* item, size_t row)> bindItem;

    void setRowCount(size_t count);
    size_t rowCount() const { return m_rowCount; }
    // Keeps the row at the top where it is.
    void setRowHeight(double height);
    double rowHeight() const { return m_rowHeight; }
    void setSpacing(double spacing);
    // Around the rows, inside the scrolled content.
    void setMargins(const Margins& margins);

    // Binds the rows in sight again, after their data changed.
    void reload();
    // The view showing the row, while it is in sight.
    View* itemForRow(size_t row) const;
    RectF rowRect(size_t row) const;
    void scrollToRow(size_t row);

protected:
    void positionChanged() override;

private:
    class Rows;

    void updateItems(bool rebind);

    Rows* m_rows;
    size_t m_rowCount = 0;
    double m_rowHeight = 32;
    double m_spacing = 0;
    Margins m_margins;
    std::unordered_map<size_t, View*> m_items;
    std::vector<View*> m_spare;
};

} // namespace cutefish::ui
