/*
 * Tools Scenes - panel "Salida limpia": Programa sin capas (o con las capas
 * elegidas) hacia un monitor conectado y/o por NDI.
 */
#pragma once

#include "live-engine.hpp"

#include <obs-frontend-api.h>

#include <QPointer>
#include <QWidget>

#include <array>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScreen;

class OutputDock : public QWidget {
public:
	explicit OutputDock(QWidget *parent);
	~OutputDock() override;

private:
	void RefreshScreens();
	QScreen *SelectedScreen() const;
	void IdentifyScreens();
	void CloseIdentify();
	void UpdateState();
	void LoadSettings();
	void SaveSettings() const;
	void RestoreOutputs();
	static void FrontendEvent(enum obs_frontend_event event, void *data);

	QComboBox *screenCombo = nullptr;
	std::vector<QPointer<QWidget>> identifyWindows;
	std::array<QCheckBox *, kLayerCount> monitorLayers{};
	QPushButton *monitorButton = nullptr;

	QLineEdit *ndiName = nullptr;
	std::array<QCheckBox *, kLayerCount> ndiLayers{};
	QPushButton *ndiButton = nullptr;
	QLabel *ndiStatus = nullptr;

	QString savedScreen;
	int savedScreenIndex = -1; /* desempata pantallas con el mismo nombre */
	/* Salidas que el usuario dejó encendidas: se reanudan al abrir OBS. */
	bool monitorWanted = false;
	bool ndiWanted = false;
};
