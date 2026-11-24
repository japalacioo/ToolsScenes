/*
 * Tools Scenes - ventana de proyector para la salida limpia.
 */
#include "projector-window.hpp"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QWindow>

#include <cmath>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

class ProjectorSurfaceFilter : public QObject {
public:
	explicit ProjectorSurfaceFilter(ProjectorWindow *w) : QObject(w), window(w) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		if (event->type() == QEvent::PlatformSurface &&
		    static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
			    QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
			window->DestroyDisplay();
		return QObject::eventFilter(obj, event);
	}

private:
	ProjectorWindow *window;
};

ProjectorWindow::ProjectorWindow(obs_view_t *view_) : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), view(view_)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_DeleteOnClose, false);
	setWindowTitle(QString::fromUtf8("Tools Scenes - Salida limpia"));
	setCursor(Qt::BlankCursor);
	setFocusPolicy(Qt::StrongFocus);
}

ProjectorWindow::~ProjectorWindow()
{
	destroying = true;
	DestroyDisplay();
}

QSize ProjectorWindow::PixelSize() const
{
	const qreal dpr = devicePixelRatioF();
	return QSize((int)std::lround(width() * dpr), (int)std::lround(height() * dpr));
}

void ProjectorWindow::SetCanvasSize(uint32_t cx, uint32_t cy)
{
	if (cx && cy) {
		canvasW.store(cx);
		canvasH.store(cy);
	}
}

void ProjectorWindow::ShowOn(QScreen *screen)
{
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	(void)winId();
	if (QWindow *w = windowHandle())
		w->setScreen(screen);
	setGeometry(screen->geometry());
#ifdef __APPLE__
	/* En macOS showFullScreen crea un "espacio" aparte; una ventana sin marco
	 * del tamaño de la pantalla es más predecible. */
	show();
#else
	showFullScreen();
#endif
	raise();
	CreateDisplay();
}

void ProjectorWindow::CreateDisplay()
{
	if (display || destroying || !isVisible())
		return;
	(void)winId();
	QWindow *window = windowHandle();
	if (!window)
		return;
	if (!filterInstalled) {
		window->installEventFilter(new ProjectorSurfaceFilter(this));
		filterInstalled = true;
	}
	if (!window->isExposed())
		return;

	const QSize size = PixelSize();
	if (size.width() <= 0 || size.height() <= 0)
		return;

	gs_init_data info = {};
	info.cx = (uint32_t)size.width();
	info.cy = (uint32_t)size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
	info.window.hwnd = (HWND)window->winId();
#elif defined(__APPLE__)
	info.window.view = (id)window->winId();
#else
	if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL)
		return;
	info.window.id = window->winId();
	info.window.display = obs_get_nix_platform_display();
#endif
	display = obs_display_create(&info, 0xFF000000);
	if (display)
		obs_display_add_draw_callback(display, DrawCallback, this);
}

void ProjectorWindow::DestroyDisplay()
{
	if (!display)
		return;
	obs_display_remove_draw_callback(display, DrawCallback, this);
	obs_display_destroy(display);
	display = nullptr;
}

void ProjectorWindow::DrawCallback(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<ProjectorWindow *>(data);
	const uint32_t bw = self->canvasW.load();
	const uint32_t bh = self->canvasH.load();
	if (!bw || !bh || !cx || !cy)
		return;

	const double scale = std::min((double)cx / bw, (double)cy / bh);
	const int w = (int)std::lround(bw * scale);
	const int h = (int)std::lround(bh * scale);
	const int x = ((int)cx - w) / 2;
	const int y = ((int)cy - h) / 2;

	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(x, y, w, h);
	gs_ortho(0.0f, (float)bw, 0.0f, (float)bh, -100.0f, 100.0f);
	obs_view_render(self->view);
	gs_projection_pop();
	gs_viewport_pop();
}

void ProjectorWindow::paintEvent(QPaintEvent *)
{
	CreateDisplay();
}

void ProjectorWindow::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	CreateDisplay();
}

void ProjectorWindow::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	CreateDisplay();
	if (display) {
		const QSize size = PixelSize();
		obs_display_resize(display, (uint32_t)size.width(), (uint32_t)size.height());
	}
}

void ProjectorWindow::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Escape) {
		close();
		return;
	}
	QWidget::keyPressEvent(event);
}

void ProjectorWindow::mouseDoubleClickEvent(QMouseEvent *)
{
	/* Doble clic alterna el cursor para poder ubicar la ventana. */
	setCursor(cursor().shape() == Qt::BlankCursor ? Qt::ArrowCursor : Qt::BlankCursor);
}

void ProjectorWindow::closeEvent(QCloseEvent *event)
{
	event->ignore();
	hide();
	if (onClosedByUser)
		onClosedByUser();
}
