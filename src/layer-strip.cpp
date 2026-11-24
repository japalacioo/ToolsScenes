/*
 * Tools Scenes - botones de capa en la columna central (ver layer-strip.hpp).
 */
#include "layer-strip.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QBoxLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

constexpr const char *kObjectName = "scenePrevioLayerStrip";

} // namespace

LayerStrip::LayerStrip(QWidget *parent) : QWidget(parent)
{
	setObjectName(kObjectName);
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 8, 0, 0);
	root->setSpacing(3);

	auto *line = new QFrame(this);
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);
	root->addWidget(line);

	auto *title = new QLabel(T("Strip.Title"), this);
	title->setAlignment(Qt::AlignCenter);
	title->setStyleSheet("font-weight:bold;");
	root->addWidget(title);

	auto *row = new QHBoxLayout();
	row->setSpacing(3);
	for (int i = 0; i < kLayerCount; i++) {
		auto *b = new QPushButton(QString::number(i + 1), this);
		b->setMinimumSize(36, 30);
		b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		b->setContextMenuPolicy(Qt::CustomContextMenu);
		buttons[(size_t)i] = b;
		row->addWidget(b);
		connect(b, &QPushButton::clicked, this, [i]() {
			LiveEngine &e = LiveEngine::Get();
			e.SetLayerVisible(i, !e.LayerVisible(i));
		});
		connect(b, &QPushButton::customContextMenuRequested, this, [this, i, b](const QPoint &pos) {
			LiveEngine &e = LiveEngine::Get();
			QMenu menu(this);
			const QString name = e.LayerContentName(i);
			QAction *head = menu.addAction(QString("%1 %2: %3").arg(T("Layer")).arg(i + 1).arg(
				name.isEmpty() ? T("Layer.Empty") : name));
			head->setEnabled(false);
			menu.addSeparator();
			QAction *toggle = menu.addAction(e.LayerVisible(i) ? T("Strip.Hide") : T("Strip.Show"));
			toggle->setEnabled(!name.isEmpty());
			connect(toggle, &QAction::triggered, this, [i]() {
				LiveEngine &en = LiveEngine::Get();
				en.SetLayerVisible(i, !en.LayerVisible(i));
			});
			QAction *clear = menu.addAction(T("Layer.Clear"));
			clear->setEnabled(!name.isEmpty());
			connect(clear, &QAction::triggered, this, [i]() { LiveEngine::Get().ClearLayer(i); });
			menu.exec(b->mapToGlobal(pos));
		});
	}
	root->addLayout(row);

	names = new QLabel(this);
	names->setAlignment(Qt::AlignCenter);
	names->setWordWrap(true);
	names->setStyleSheet("color:#b0b4bc;font-size:11px;");
	root->addWidget(names);

	LiveEngine::Get().AddListener(this, [this]() { Refresh(); });
	Refresh();
}

LayerStrip::~LayerStrip()
{
	LiveEngine::Get().RemoveListener(this);
}

void LayerStrip::Refresh()
{
	LiveEngine &e = LiveEngine::Get();
	QStringList onAir;
	for (int i = 0; i < kLayerCount; i++) {
		QPushButton *b = buttons[(size_t)i];
		const QString name = e.LayerContentName(i);
		const bool visible = e.LayerVisible(i);
		b->setToolTip(QString("%1 %2: %3\n%4")
				      .arg(T("Layer"))
				      .arg(i + 1)
				      .arg(name.isEmpty() ? T("Layer.Empty") : name)
				      .arg(T("Strip.Tip")));
		b->setEnabled(!name.isEmpty() || visible);
		if (visible)
			b->setStyleSheet("QPushButton{background-color:#e08a00;color:black;font-weight:bold;}");
		else if (!name.isEmpty())
			b->setStyleSheet("QPushButton{border:1px solid #e08a00;color:#e08a00;font-weight:bold;}");
		else
			b->setStyleSheet(QString());
		if (visible)
			onAir << QString("%1: %2").arg(i + 1).arg(
				names->fontMetrics().elidedText(name, Qt::ElideMiddle, 140));
	}
	names->setText(onAir.isEmpty() ? T("Strip.None") : onAir.join("\n"));
}

void LayerStrip::InjectIfStudioMode()
{
	if (!obs_frontend_preview_program_mode_active())
		return;
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!main)
		return;
	/* La columna central (programOptions) contiene la barra T de OBS. */
	for (QSlider *slider : main->findChildren<QSlider *>()) {
		if (slider->property("class").toString() != "slider-tbar")
			continue;
		QWidget *column = slider->parentWidget();
		/* QWidget: LayerStrip no tiene Q_OBJECT (MSVC rechaza findChild<LayerStrip *>). */
		if (!column || column->findChild<QWidget *>(kObjectName))
			return;
		auto *layout = qobject_cast<QBoxLayout *>(column->layout());
		if (!layout)
			return;
		const int idx = layout->indexOf(slider);
		layout->insertWidget(idx >= 0 ? idx + 1 : layout->count(), new LayerStrip(column));
		return;
	}
}
