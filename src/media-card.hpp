/*
 * Tools Scenes - tarjeta de un elemento multimedia, al estilo de las entradas
 * de vMix: miniatura, nombre, Vista / Aire y botones de capa 1-4.
 */
#pragma once

#include "live-engine.hpp"

#include <QFrame>
#include <QImage>
#include <QString>

#include <array>
#include <functional>

class QLabel;
class QPushButton;

class MediaCard : public QFrame {
public:
	MediaCard(const QString &key, const QString &title, const QString &typeTag, QWidget *parent);

	const QString &Key() const { return key; }
	void SetTitle(const QString &title);
	void SetThumbnail(const QImage &image);
	void SetCardWidth(int width);

	struct State {
		bool selected = false;
		bool program = false;
		bool preview = false;
		bool studio = false;
		std::array<bool, kLayerCount> layerLoaded{};
		std::array<bool, kLayerCount> layerVisible{};
	};
	void SetState(const State &state);
	const State &GetState() const { return state; }

	std::function<void()> onSelect;
	std::function<void()> onDoubleClick;
	std::function<void()> onPreview;
	std::function<void()> onProgram;
	std::function<void(int layer)> onLayer;
	std::function<void(const QPoint &globalPos)> onContextMenu;

protected:
	void mousePressEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;

private:
	void UpdateThumbLabel();

	QString key;
	QString typeTag;
	QLabel *thumb = nullptr;
	QLabel *title = nullptr;
	QPushButton *btnPreview = nullptr;
	QPushButton *btnProgram = nullptr;
	std::array<QPushButton *, kLayerCount> layerButtons{};
	QImage image;
	int cardWidth = 180;
	State state;
	bool stateApplied = false;
};
