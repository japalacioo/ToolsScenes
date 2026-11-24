/*
 * Tools Scenes - panel "Salida limpia" (ver output-dock.hpp).
 */
#include "output-dock.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const char *kOnStyle = "QPushButton{background-color:#c81e1e;color:white;font-weight:bold;}";

} // namespace

OutputDock::OutputDock(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(240, 160);
	/* Contenido desplazable: en paneles bajos no se aplasta. */
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	scrollArea->setFrameShape(QFrame::NoFrame);
	auto *content = new QWidget(scrollArea);
	scrollArea->setWidget(content);
	outer->addWidget(scrollArea);
	auto *root = new QVBoxLayout(content);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(6);

	auto *info = new QLabel(T("Clean.Info"), this);
	info->setWordWrap(true);
	root->addWidget(info);

	/* ---- Monitor ---- */
	auto *mon = new QGroupBox(T("Clean.Monitor"), this);
	auto *monLayout = new QVBoxLayout(mon);
	auto *screenRow = new QHBoxLayout();
	screenRow->addWidget(new QLabel(T("Clean.Screen"), mon));
	screenCombo = new QComboBox(mon);
	screenRow->addWidget(screenCombo, 1);
	monLayout->addLayout(screenRow);
	auto *monLayerRow = new QHBoxLayout();
	monLayerRow->addWidget(new QLabel(T("Clean.IncludeLayers"), mon));
	for (int i = 0; i < kLayerCount; i++) {
		monitorLayers[(size_t)i] = new QCheckBox(QString::number(i + 1), mon);
		monLayerRow->addWidget(monitorLayers[(size_t)i]);
	}
	monLayerRow->addStretch(1);
	monLayout->addLayout(monLayerRow);
	monitorButton = new QPushButton(T("Clean.ShowMonitor"), mon);
	monitorButton->setCheckable(true);
	monLayout->addWidget(monitorButton);
	mon->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	root->addWidget(mon);

	/* ---- NDI ---- */
	auto *ndi = new QGroupBox(T("Clean.Ndi"), this);
	auto *ndiLayout = new QVBoxLayout(ndi);
	auto *nameRow = new QHBoxLayout();
	nameRow->addWidget(new QLabel(T("Clean.NdiName"), ndi));
	ndiName = new QLineEdit(ndi);
	ndiName->setText("OBS Limpio");
	nameRow->addWidget(ndiName, 1);
	ndiLayout->addLayout(nameRow);
	auto *ndiLayerRow = new QHBoxLayout();
	ndiLayerRow->addWidget(new QLabel(T("Clean.IncludeLayers"), ndi));
	for (int i = 0; i < kLayerCount; i++) {
		ndiLayers[(size_t)i] = new QCheckBox(QString::number(i + 1), ndi);
		ndiLayerRow->addWidget(ndiLayers[(size_t)i]);
	}
	ndiLayerRow->addStretch(1);
	ndiLayout->addLayout(ndiLayerRow);
	ndiButton = new QPushButton(T("Clean.StartNdi"), ndi);
	ndiButton->setCheckable(true);
	ndiLayout->addWidget(ndiButton);
	ndiStatus = new QLabel(ndi);
	ndiStatus->setWordWrap(true);
	ndiLayout->addWidget(ndiStatus);
	ndi->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	root->addWidget(ndi);
	root->addStretch(1);

	LoadSettings();
	RefreshScreens();

	CleanOutputs &out = CleanOutputs::Get();
	for (int i = 0; i < kLayerCount; i++) {
		out.SetMonitorLayer(i, monitorLayers[(size_t)i]->isChecked());
		out.SetNdiLayer(i, ndiLayers[(size_t)i]->isChecked());
		connect(monitorLayers[(size_t)i], &QCheckBox::toggled, this, [this, i](bool on) {
			CleanOutputs::Get().SetMonitorLayer(i, on);
			SaveSettings();
		});
		connect(ndiLayers[(size_t)i], &QCheckBox::toggled, this, [this, i](bool on) {
			CleanOutputs::Get().SetNdiLayer(i, on);
			SaveSettings();
		});
	}

	connect(monitorButton, &QPushButton::clicked, this, [this](bool on) {
		CleanOutputs &o = CleanOutputs::Get();
		if (on) {
			QScreen *screen = SelectedScreen();
			/* Avisar si es la misma pantalla de OBS. */
			QScreen *own = window() ? window()->screen() : nullptr;
			if (screen && own == screen &&
			    QMessageBox::question(this, T("Clean.Monitor"), T("Clean.SameScreen")) != QMessageBox::Yes) {
				monitorButton->setChecked(false);
				return;
			}
			o.ShowMonitor(screen);
			savedScreen = screenCombo->currentData().toString();
			savedScreenIndex = screenCombo->currentIndex();
			SaveSettings();
		} else {
			o.HideMonitor();
		}
		UpdateState();
	});
	connect(screenCombo, qOverload<int>(&QComboBox::activated), this, [this](int) {
		savedScreen = screenCombo->currentData().toString();
		savedScreenIndex = screenCombo->currentIndex();
		SaveSettings();
		if (CleanOutputs::Get().MonitorActive())
			CleanOutputs::Get().ShowMonitor(SelectedScreen());
	});
	connect(ndiButton, &QPushButton::clicked, this, [this](bool on) {
		CleanOutputs &o = CleanOutputs::Get();
		if (on) {
			QString error;
			const QString name = ndiName->text().trimmed().isEmpty() ? QString("OBS Limpio")
										    : ndiName->text().trimmed();
			if (!o.StartNdi(name, error))
				QMessageBox::warning(this, T("Clean.Ndi"), error);
		} else {
			o.StopNdi();
		}
		SaveSettings();
		UpdateState();
	});
	connect(ndiName, &QLineEdit::editingFinished, this, [this]() { SaveSettings(); });

	connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen *) { RefreshScreens(); });
	connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *) {
		RefreshScreens();
		/* Si se desconectó el monitor de salida, cerrar el proyector. */
		bool found = false;
		for (QScreen *s : QGuiApplication::screens())
			found = found || s->name() == savedScreen;
		if (!found && CleanOutputs::Get().MonitorActive()) {
			CleanOutputs::Get().HideMonitor();
			UpdateState();
		}
	});

	out.onMonitorClosed = [this]() { UpdateState(); };
	UpdateState();
}

OutputDock::~OutputDock()
{
	CleanOutputs::Get().onMonitorClosed = nullptr;
}

void OutputDock::RefreshScreens()
{
	screenCombo->clear();
	int select = -1;
	const QList<QScreen *> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		QScreen *s = screens[i];
		const QRect g = s->geometry();
		const qreal dpr = s->devicePixelRatio();
		screenCombo->addItem(QString("%1: %2 (%3x%4)")
					     .arg(i + 1)
					     .arg(s->name())
					     .arg((int)(g.width() * dpr))
					     .arg((int)(g.height() * dpr)),
				     s->name());
		/* Con nombres repetidos (monitores iguales) gana la posición guardada. */
		if (s->name() == savedScreen && (select < 0 || i == savedScreenIndex))
			select = i;
	}
	if (select < 0 && screens.size() > 1)
		select = 1; /* por defecto, la segunda pantalla */
	if (select >= 0)
		screenCombo->setCurrentIndex(select);
}

QScreen *OutputDock::SelectedScreen() const
{
	/* El combo se llena en el orden de QGuiApplication::screens(): el índice
	 * identifica la pantalla aunque dos monitores tengan el mismo nombre. */
	const QList<QScreen *> screens = QGuiApplication::screens();
	const int i = screenCombo->currentIndex();
	const QString name = screenCombo->currentData().toString();
	if (i >= 0 && i < screens.size() && screens[i]->name() == name)
		return screens[i];
	for (QScreen *s : screens)
		if (s->name() == name)
			return s;
	return nullptr;
}

void OutputDock::UpdateState()
{
	const CleanOutputs &o = CleanOutputs::Get();
	const bool mon = o.MonitorActive();
	monitorButton->setChecked(mon);
	monitorButton->setText(mon ? T("Clean.HideMonitor") : T("Clean.ShowMonitor"));
	monitorButton->setStyleSheet(mon ? kOnStyle : "");

	const bool ndiOn = o.NdiActive();
	ndiButton->setChecked(ndiOn);
	ndiButton->setText(ndiOn ? T("Clean.StopNdi") : T("Clean.StartNdi"));
	ndiButton->setStyleSheet(ndiOn ? kOnStyle : "");
	ndiName->setEnabled(!ndiOn);
	const bool available = CleanOutputs::NdiAvailable();
	ndiButton->setEnabled(available || ndiOn);
	ndiStatus->setText(available ? (ndiOn ? T("Clean.NdiOn").arg(ndiName->text()) : QString())
				     : T("Ndi.Missing"));
	ndiStatus->setVisible(!ndiStatus->text().isEmpty());
}

void OutputDock::LoadSettings()
{
	char *path = obs_module_config_path("outputs.json");
	if (!path)
		return;
	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!data)
		return;
	savedScreen = QString::fromUtf8(obs_data_get_string(data, "monitor_screen"));
	obs_data_set_default_int(data, "monitor_screen_index", -1);
	savedScreenIndex = (int)obs_data_get_int(data, "monitor_screen_index");
	const char *name = obs_data_get_string(data, "ndi_name");
	if (name && *name)
		ndiName->setText(QString::fromUtf8(name));
	for (int i = 0; i < kLayerCount; i++) {
		monitorLayers[(size_t)i]->setChecked(
			obs_data_get_bool(data, ("monitor_layer_" + std::to_string(i)).c_str()));
		ndiLayers[(size_t)i]->setChecked(obs_data_get_bool(data, ("ndi_layer_" + std::to_string(i)).c_str()));
	}
	obs_data_release(data);
}

void OutputDock::SaveSettings() const
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("outputs.json");
	if (!path)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "monitor_screen", savedScreen.toUtf8().constData());
	obs_data_set_int(data, "monitor_screen_index", savedScreenIndex);
	obs_data_set_string(data, "ndi_name", ndiName->text().toUtf8().constData());
	for (int i = 0; i < kLayerCount; i++) {
		obs_data_set_bool(data, ("monitor_layer_" + std::to_string(i)).c_str(),
				  monitorLayers[(size_t)i]->isChecked());
		obs_data_set_bool(data, ("ndi_layer_" + std::to_string(i)).c_str(), ndiLayers[(size_t)i]->isChecked());
	}
	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);
	bfree(path);
}
