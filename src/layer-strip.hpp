/*
 * Tools Scenes - botones de capa 1-4 en la columna central del Modo Estudio
 * (entre Vista previa y Programa, debajo de "Transición"), como en vMix.
 */
#pragma once

#include "live-engine.hpp"

#include <QWidget>

#include <array>

class QLabel;
class QPushButton;

class LayerStrip : public QWidget {
public:
	explicit LayerStrip(QWidget *parent);
	~LayerStrip() override;

	void Refresh();

	/* Inserta la tira en la columna central si el Modo Estudio está activo. */
	static void InjectIfStudioMode();

private:
	std::array<QPushButton *, kLayerCount> buttons{};
	QLabel *names = nullptr;
};
