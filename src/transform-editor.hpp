/*
 * Tools Scenes - ventana "Editar posición y tamaño" (transformación libre).
 *
 * TransformCanvas: lienzo con el resultado EXACTO (se dibuja con una escena
 * privada de OBS aplicando la misma transformación que en Programa/capas) y
 * herramientas como las de OBS: mover, 8 asas de tamaño, asa de rotación,
 * Alt + lado = recortar, flechas = mover. TransformEditor: la ventana con el
 * lienzo y los campos numéricos.
 */
#pragma once

#include "live-engine.hpp"

#include <obs.h>

#include <QDialog>
#include <QWidget>

#include <functional>
#include <mutex>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

class TransformCanvas : public QWidget {
public:
	explicit TransformCanvas(QWidget *parent);
	~TransformCanvas() override;

	void SetSource(obs_source_t *source, const ItemTransform &t, bool activate);
	void SetTransform(const ItemTransform &t);
	const ItemTransform &Transform() const { return transform; }
	obs_source_t *Source() const { return current; }
	ItemTransform Effective() const; /* caja efectiva (también si es "ajustar") */
	void SourceSize(uint32_t &w, uint32_t &h) const;
	void DestroyDisplay();
	void Shutdown();

	std::function<void(const ItemTransform &t, bool final)> onChanged;

protected:
	QPaintEngine *paintEngine() const override { return nullptr; }
	void paintEvent(QPaintEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

private:
	friend class CanvasSurfaceFilter;
	enum class Drag { None, Move, Resize, Rotate, Crop };

	static void DrawCallback(void *data, uint32_t cx, uint32_t cy);
	void CreateDisplay();
	QSize PixelSize() const;
	bool Mapping(double &scale, double &ox, double &oy) const;
	QPointF ToCanvas(const QPointF &p) const;
	QPointF ToLocal(const QPointF &canvasPt, const ItemTransform &box) const;
	bool HitHandle(const QPointF &local, const ItemTransform &box, double tol, int &hx, int &hy, bool &rotate) const;
	void Apply(const ItemTransform &t, bool final);
	void Publish();

	obs_display_t *display = nullptr;
	bool filterInstalled = false;
	bool destroying = false;

	obs_scene_t *scene = nullptr; /* escena privada: dibuja exactamente como en vivo */
	obs_source_t *current = nullptr;
	bool showingHeld = false;
	bool activeHeld = false;

	std::mutex overlayMutex;
	ItemTransform overlay; /* copia para el hilo gráfico */
	bool overlayValid = false;

	ItemTransform transform;
	Drag drag = Drag::None;
	int dragHx = 0, dragHy = 0;
	QPointF dragStartCanvas;
	ItemTransform dragOrigin;
};

class TransformEditor : public QDialog {
public:
	explicit TransformEditor(QWidget *parent);
	~TransformEditor() override;

	void Open(obs_source_t *source, const ItemTransform &t, bool activate, const QString &title);
	void Detach(); /* suelta la fuente (al cerrar o al cambiar de colección) */
	obs_source_t *Source() const;

	std::function<void(const ItemTransform &t, bool final)> onChanged;
	std::function<void()> onClosed;

protected:
	void closeEvent(QCloseEvent *event) override;
	void reject() override;

private:
	void SyncFields();
	void FromFields(bool final);
	void Preset(int which);
	void SetCrop(int l, int t, int r, int b);

	TransformCanvas *canvas = nullptr;
	QLabel *titleLabel = nullptr;
	QDoubleSpinBox *posX = nullptr, *posY = nullptr, *sizeW = nullptr, *sizeH = nullptr, *rotation = nullptr;
	QSpinBox *cropL = nullptr, *cropT = nullptr, *cropR = nullptr, *cropB = nullptr;
	QCheckBox *lockAspect = nullptr;
	bool syncing = false;
};
