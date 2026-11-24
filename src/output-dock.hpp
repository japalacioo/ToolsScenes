/*
 * Tools Scenes - panel "Salida limpia": Programa sin capas (o con las capas
 * elegidas) hacia un monitor conectado y/o por NDI.
 */
#pragma once

#include "live-engine.hpp"

#include <QWidget>

#include <array>

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
	void UpdateState();
	void LoadSettings();
	void SaveSettings() const;

	QComboBox *screenCombo = nullptr;
	std::array<QCheckBox *, kLayerCount> monitorLayers{};
	QPushButton *monitorButton = nullptr;

	QLineEdit *ndiName = nullptr;
	std::array<QCheckBox *, kLayerCount> ndiLayers{};
	QPushButton *ndiButton = nullptr;
	QLabel *ndiStatus = nullptr;

	QString savedScreen;
	int savedScreenIndex = -1; /* desempata pantallas con el mismo nombre */
};
