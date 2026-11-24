/*
 * Tools Scenes - tarjeta multimedia (ver media-card.hpp).
 */
#include "media-card.hpp"

#include <obs-module.h>

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const char *kSmallBtn = "QPushButton{padding:1px 4px;min-height:18px;font-size:11px;}";

} // namespace

MediaCard::MediaCard(const QString &key_, const QString &titleText, const QString &typeTag_, QWidget *parent)
	: QFrame(parent),
	  key(key_),
	  typeTag(typeTag_)
{
	setObjectName("spCard");
	setFrameShape(QFrame::NoFrame);
	setCursor(Qt::PointingHandCursor);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(3, 3, 3, 3);
	root->setSpacing(2);

	thumb = new QLabel(this);
	thumb->setAlignment(Qt::AlignCenter);
	thumb->setStyleSheet("background-color:#0c0c0e;color:#9aa0aa;font-weight:bold;font-size:15px;");
	root->addWidget(thumb);

	title = new QLabel(this);
	title->setStyleSheet("font-size:11px;");
	title->setTextInteractionFlags(Qt::NoTextInteraction);
	root->addWidget(title);

	auto *row = new QHBoxLayout();
	row->setSpacing(2);
	btnPreview = new QPushButton(T("Card.Preview"), this);
	btnPreview->setToolTip(T("Card.PreviewTip"));
	btnProgram = new QPushButton(T("Card.Program"), this);
	btnProgram->setToolTip(T("Card.ProgramTip"));
	for (QPushButton *b : {btnPreview, btnProgram}) {
		b->setStyleSheet(kSmallBtn);
		b->setCursor(Qt::ArrowCursor);
		row->addWidget(b);
	}
	row->addStretch(1);
	for (int i = 0; i < kLayerCount; i++) {
		auto *b = new QPushButton(QString::number(i + 1), this);
		b->setFixedWidth(22);
		b->setCursor(Qt::ArrowCursor);
		b->setToolTip(T("Card.LayerTip").arg(i + 1));
		layerButtons[(size_t)i] = b;
		row->addWidget(b);
		connect(b, &QPushButton::clicked, this, [this, i]() {
			if (onSelect)
				onSelect();
			if (onLayer)
				onLayer(i);
		});
	}
	root->addLayout(row);

	connect(btnPreview, &QPushButton::clicked, this, [this]() {
		if (onSelect)
			onSelect();
		if (onPreview)
			onPreview();
	});
	connect(btnProgram, &QPushButton::clicked, this, [this]() {
		if (onSelect)
			onSelect();
		if (onProgram)
			onProgram();
	});

	SetTitle(titleText);
	SetCardWidth(cardWidth);
	SetState(State());
}

void MediaCard::SetTitle(const QString &text)
{
	setToolTip(text);
	title->setText(title->fontMetrics().elidedText(text, Qt::ElideMiddle, std::max(20, cardWidth - 8)));
}

void MediaCard::SetThumbnail(const QImage &img)
{
	image = img;
	UpdateThumbLabel();
}

void MediaCard::SetCardWidth(int width)
{
	cardWidth = std::max(120, width);
	setFixedWidth(cardWidth);
	thumb->setFixedSize(cardWidth - 6, (cardWidth - 6) * 9 / 16);
	SetTitle(toolTip());
	UpdateThumbLabel();
}

void MediaCard::UpdateThumbLabel()
{
	const QSize size = thumb->size();
	if (image.isNull()) {
		thumb->setPixmap(QPixmap());
		thumb->setText(typeTag);
		return;
	}
	const qreal dpr = devicePixelRatioF();
	QPixmap pm(size * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(QColor(12, 12, 14));
	QPainter p(&pm);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	const QSize scaled = image.size().scaled(size, Qt::KeepAspectRatio);
	const QRect r(QPoint((size.width() - scaled.width()) / 2, (size.height() - scaled.height()) / 2), scaled);
	p.drawImage(r, image);
	p.end();
	thumb->setText(QString());
	thumb->setPixmap(pm);
}

void MediaCard::SetState(const State &s)
{
	/* Las hojas de estilo son costosas: solo se aplican si algo cambió. */
	if (stateApplied && s.selected == state.selected && s.program == state.program &&
	    s.preview == state.preview && s.studio == state.studio && s.layerLoaded == state.layerLoaded &&
	    s.layerVisible == state.layerVisible)
		return;
	stateApplied = true;
	state = s;
	QString border = "#3a3a40";
	int width = 1;
	if (s.program) {
		border = "#d42020";
		width = 3;
	} else if (s.preview) {
		border = "#1fae3a";
		width = 3;
	} else if (s.selected) {
		border = "#4c7dff";
		width = 2;
	}
	setStyleSheet(QString("QFrame#spCard{background-color:%1;border:%2px solid %3;border-radius:4px;}")
			      .arg(s.selected ? "#2b2f3a" : "#232328")
			      .arg(width)
			      .arg(border));

	btnPreview->setEnabled(s.studio);
	btnPreview->setStyleSheet(QString(kSmallBtn) +
				  (s.preview ? "QPushButton{background-color:#1fae3a;color:white;font-weight:bold;}" : ""));
	btnProgram->setStyleSheet(QString(kSmallBtn) +
				  (s.program ? "QPushButton{background-color:#d42020;color:white;font-weight:bold;}" : ""));

	for (int i = 0; i < kLayerCount; i++) {
		QPushButton *b = layerButtons[(size_t)i];
		QString style = QString(kSmallBtn) + "QPushButton{padding:1px 0px;}";
		if (s.layerVisible[(size_t)i])
			style += "QPushButton{background-color:#e08a00;color:black;font-weight:bold;}";
		else if (s.layerLoaded[(size_t)i])
			style += "QPushButton{border:1px solid #e08a00;color:#e08a00;}";
		b->setStyleSheet(style);
	}
}

void MediaCard::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton && onSelect)
		onSelect();
	QFrame::mousePressEvent(event);
}

void MediaCard::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton && onDoubleClick)
		onDoubleClick();
}

void MediaCard::contextMenuEvent(QContextMenuEvent *event)
{
	if (onSelect)
		onSelect();
	if (onContextMenu)
		onContextMenu(event->globalPos());
}
