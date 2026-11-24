/*
 * Tools Scenes - ventana de proyector a pantalla completa para una obs_view
 * (salida limpia hacia un monitor conectado al equipo).
 */
#pragma once

#include <obs.h>

#include <QWidget>

#include <atomic>
#include <functional>

class QScreen;

class ProjectorWindow : public QWidget {
public:
	explicit ProjectorWindow(obs_view_t *view);
	~ProjectorWindow() override;

	void ShowOn(QScreen *screen);
	void SetCanvasSize(uint32_t cx, uint32_t cy);
	void DestroyDisplay();

	std::function<void()> onClosedByUser;

protected:
	QPaintEngine *paintEngine() const override { return nullptr; }
	void paintEvent(QPaintEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

private:
	friend class ProjectorSurfaceFilter;
	static void DrawCallback(void *data, uint32_t cx, uint32_t cy);
	void CreateDisplay();
	QSize PixelSize() const;

	obs_view_t *view;
	obs_display_t *display = nullptr;
	bool filterInstalled = false;
	bool destroying = false;
	std::atomic<uint32_t> canvasW{1920};
	std::atomic<uint32_t> canvasH{1080};
};
