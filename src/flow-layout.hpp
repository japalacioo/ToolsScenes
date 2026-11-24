/*
 * Tools Scenes - layout de flujo (las tarjetas se acomodan según el ancho).
 */
#pragma once

#include <QLayout>
#include <QStyle>
#include <QWidget>

#include <vector>

class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent = nullptr, int spacing = 6) : QLayout(parent), space(spacing)
	{
		setContentsMargins(4, 4, 4, 4);
	}
	~FlowLayout() override
	{
		while (QLayoutItem *item = takeAt(0))
			delete item;
	}

	void addItem(QLayoutItem *item) override { items.push_back(item); }
	int count() const override { return (int)items.size(); }
	QLayoutItem *itemAt(int index) const override
	{
		return (index >= 0 && index < (int)items.size()) ? items[(size_t)index] : nullptr;
	}
	QLayoutItem *takeAt(int index) override
	{
		if (index < 0 || index >= (int)items.size())
			return nullptr;
		QLayoutItem *item = items[(size_t)index];
		items.erase(items.begin() + index);
		return item;
	}
	Qt::Orientations expandingDirections() const override { return {}; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override { return DoLayout(QRect(0, 0, width, 0), true); }
	void setGeometry(const QRect &rect) override
	{
		QLayout::setGeometry(rect);
		DoLayout(rect, false);
	}
	QSize sizeHint() const override { return minimumSize(); }
	QSize minimumSize() const override
	{
		QSize size;
		for (QLayoutItem *item : items)
			size = size.expandedTo(item->minimumSize());
		const QMargins m = contentsMargins();
		return size + QSize(m.left() + m.right(), m.top() + m.bottom());
	}

private:
	int DoLayout(const QRect &rect, bool testOnly) const
	{
		const QMargins m = contentsMargins();
		const QRect r = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
		int x = r.x(), y = r.y(), lineHeight = 0;
		for (QLayoutItem *item : items) {
			if (item->widget() && item->widget()->isHidden())
				continue;
			const QSize hint = item->sizeHint();
			int nextX = x + hint.width() + space;
			if (nextX - space > r.right() + 1 && lineHeight > 0) {
				x = r.x();
				y += lineHeight + space;
				nextX = x + hint.width() + space;
				lineHeight = 0;
			}
			if (!testOnly)
				item->setGeometry(QRect(QPoint(x, y), hint));
			x = nextX;
			lineHeight = std::max(lineHeight, hint.height());
		}
		return y + lineHeight - rect.y() + m.bottom();
	}

	std::vector<QLayoutItem *> items;
	int space;
};
