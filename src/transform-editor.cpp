/*
 * Tools Scenes - ventana "Editar posición y tamaño" (ver transform-editor.hpp).
 */
#include "transform-editor.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
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

namespace {

constexpr double kMargin = 0.05;    /* margen alrededor del lienzo */
constexpr double kHandlePx = 8.0;   /* tamaño de las asas (px de pantalla) */
constexpr double kRotateGapPx = 28; /* distancia del asa de rotación */
constexpr double kPi = 3.14159265358979323846;

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

void CanvasSize(uint32_t &w, uint32_t &h)
{
	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height) {
		w = ovi.base_width;
		h = ovi.base_height;
	} else {
		w = 1920;
		h = 1080;
	}
}

void DrawSolid(gs_effect_t *solid, gs_eparam_t *color, float x, float y, float w, float h, uint32_t argb)
{
	if (w <= 0 || h <= 0)
		return;
	struct vec4 c;
	vec4_set(&c, ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
		 ((argb >> 24) & 0xFF) / 255.0f);
	gs_effect_set_vec4(color, &c);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, (uint32_t)std::max(1.0f, std::round(w)), (uint32_t)std::max(1.0f, std::round(h)));
	gs_matrix_pop();
}

void DrawFrame(gs_effect_t *solid, gs_eparam_t *color, float x, float y, float w, float h, float t, uint32_t argb)
{
	DrawSolid(solid, color, x, y, w, t, argb);
	DrawSolid(solid, color, x, y + h - t, w, t, argb);
	DrawSolid(solid, color, x, y, t, h, argb);
	DrawSolid(solid, color, x + w - t, y, t, h, argb);
}

QPointF Rotate(const QPointF &v, double deg)
{
	const double r = deg * kPi / 180.0;
	return QPointF(v.x() * std::cos(r) - v.y() * std::sin(r), v.x() * std::sin(r) + v.y() * std::cos(r));
}

} // namespace

class CanvasSurfaceFilter : public QObject {
public:
	explicit CanvasSurfaceFilter(TransformCanvas *c) : QObject(c), canvas(c) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		if (event->type() == QEvent::PlatformSurface &&
		    static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
			    QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
			canvas->DestroyDisplay();
		return QObject::eventFilter(obj, event);
	}

private:
	TransformCanvas *canvas;
};

/* ======================================================================== */
/* TransformCanvas                                                           */

TransformCanvas::TransformCanvas(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	setMinimumSize(480, 270);
	scene = obs_scene_create_private("SP editor");

	/* El tamaño de algunas fuentes (videos) se conoce después de cargar. */
	auto *timer = new QTimer(this);
	timer->setInterval(250);
	connect(timer, &QTimer::timeout, this, [this]() { Publish(); });
	timer->start();
}

TransformCanvas::~TransformCanvas()
{
	Shutdown();
}

void TransformCanvas::Shutdown()
{
	destroying = true;
	DestroyDisplay();
	SetSource(nullptr, ItemTransform(), false);
	if (scene) {
		obs_source_t *ss = obs_scene_get_source(scene);
		if (showingHeld)
			obs_source_dec_showing(ss);
		showingHeld = false;
		obs_scene_release(scene);
		scene = nullptr;
	}
}

QSize TransformCanvas::PixelSize() const
{
	const qreal dpr = devicePixelRatioF();
	return QSize((int)std::lround(width() * dpr), (int)std::lround(height() * dpr));
}

void TransformCanvas::SourceSize(uint32_t &w, uint32_t &h) const
{
	w = current ? obs_source_get_width(current) : 0;
	h = current ? obs_source_get_height(current) : 0;
	/* Un video que terminó (o se detuvo) vuelve a medir 0x0: se conserva el
	 * último tamaño conocido para no perder la proporción del contenido. */
	if (w && h) {
		knownW = w;
		knownH = h;
	} else if (current) {
		w = knownW;
		h = knownH;
	}
}

double TransformCanvas::ContentAspect(const ItemTransform &t) const
{
	uint32_t sw, sh;
	SourceSize(sw, sh);
	const double vw = (double)sw - t.cropL - t.cropR;
	const double vh = (double)sh - t.cropT - t.cropB;
	return (vw > 0.0 && vh > 0.0) ? vw / vh : 0.0;
}

ItemTransform TransformCanvas::Effective() const
{
	uint32_t cw, ch, sw, sh;
	CanvasSize(cw, ch);
	SourceSize(sw, sh);
	return transform.Effective(sw, sh, cw, ch);
}

void TransformCanvas::Publish()
{
	const ItemTransform eff = Effective();
	{
		std::lock_guard<std::mutex> lock(overlayMutex);
		overlay = eff;
		overlayValid = current != nullptr;
	}
	uint32_t sw, sh;
	SourceSize(sw, sh);
	if (sw != lastSourceW || sh != lastSourceH) {
		lastSourceW = sw;
		lastSourceH = sh;
		if (onSourceSizeChanged)
			onSourceSizeChanged();
	}
}

void TransformCanvas::SetSource(obs_source_t *source, const ItemTransform &t, bool activate)
{
	transform = t;
	drag = Drag::None;
	if (source != current && scene) {
		obs_source_t *ss = obs_scene_get_source(scene);
		if (activeHeld) {
			obs_source_dec_active(ss);
			activeHeld = false;
		}
		LiveEngine::ClearScene(scene);
		if (current)
			obs_source_release(current);
		current = source ? obs_source_get_ref(source) : nullptr;
		knownW = knownH = 0;
		if (current) {
			obs_sceneitem_t *item = obs_scene_add(scene, current);
			if (item)
				LiveEngine::ApplyTransform(item, transform);
			if (activate) {
				/* Fuente privada temporal (p. ej. un video): se activa para que
				 * reproduzca; no llega a la mezcla de audio de OBS. */
				obs_source_inc_active(ss);
				activeHeld = true;
			}
		}
	} else {
		SetTransform(t);
	}
	Publish();
}

void TransformCanvas::SetTransform(const ItemTransform &t)
{
	transform = t;
	if (scene && current) {
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				LiveEngine::ApplyTransform(item, *static_cast<ItemTransform *>(param));
				return true;
			},
			&transform);
	}
	Publish();
}

void TransformCanvas::Apply(const ItemTransform &t, bool final)
{
	SetTransform(t);
	if (onChanged)
		onChanged(t, final);
}

/* ---- Geometría ---- */

bool TransformCanvas::Mapping(double &scale, double &ox, double &oy) const
{
	uint32_t cw, ch;
	CanvasSize(cw, ch);
	const double w = width(), h = height();
	if (w <= 0 || h <= 0)
		return false;
	scale = std::min(w * (1.0 - 2 * kMargin) / cw, h * (1.0 - 2 * kMargin) / ch);
	ox = (w - cw * scale) / 2.0;
	oy = (h - ch * scale) / 2.0;
	return scale > 0;
}

QPointF TransformCanvas::ToCanvas(const QPointF &p) const
{
	double s, ox, oy;
	if (!Mapping(s, ox, oy))
		return QPointF();
	return QPointF((p.x() - ox) / s, (p.y() - oy) / s);
}

QPointF TransformCanvas::ToLocal(const QPointF &c, const ItemTransform &b) const
{
	const QPointF center(b.x + b.w / 2.0, b.y + b.h / 2.0);
	return Rotate(c - center, -b.rot);
}

bool TransformCanvas::HitHandle(const QPointF &l, const ItemTransform &b, double tol, int &hx, int &hy,
				bool &rotate) const
{
	double s, ox, oy;
	Mapping(s, ox, oy);
	rotate = false;
	/* Asa de rotación (sobre el lado superior) */
	const QPointF rh(0.0, -b.h / 2.0 - kRotateGapPx / s);
	if (std::abs(l.x() - rh.x()) <= tol && std::abs(l.y() - rh.y()) <= tol) {
		rotate = true;
		return true;
	}
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			if (!x && !y)
				continue;
			if (std::abs(l.x() - x * b.w / 2.0) <= tol && std::abs(l.y() - y * b.h / 2.0) <= tol) {
				hx = x;
				hy = y;
				return true;
			}
		}
	}
	return false;
}

/* ---- Ratón y teclado ---- */

void TransformCanvas::mousePressEvent(QMouseEvent *event)
{
	setFocus();
	if (!current || event->button() != Qt::LeftButton)
		return;
	double s, ox, oy;
	if (!Mapping(s, ox, oy))
		return;
	const ItemTransform box = Effective();
	const QPointF c = ToCanvas(event->position());
	const QPointF l = ToLocal(c, box);
	const double tol = kHandlePx * 1.4 / s;
	int hx = 0, hy = 0;
	bool rotate = false;

	drag = Drag::None;
	if (HitHandle(l, box, tol, hx, hy, rotate)) {
		if (rotate)
			drag = Drag::Rotate;
		else
			drag = (event->modifiers() & Qt::AltModifier) ? Drag::Crop : Drag::Resize;
	} else if (std::abs(l.x()) <= box.w / 2.0 && std::abs(l.y()) <= box.h / 2.0) {
		drag = Drag::Move;
	}
	dragHx = hx;
	dragHy = hy;
	dragStartCanvas = c;
	dragOrigin = box;
	dragOrigin.custom = true;
}

void TransformCanvas::mouseMoveEvent(QMouseEvent *event)
{
	double s, ox, oy;
	if (!current || !Mapping(s, ox, oy)) {
		unsetCursor();
		return;
	}
	const QPointF c = ToCanvas(event->position());

	if (drag == Drag::None) {
		const ItemTransform box = Effective();
		const QPointF l = ToLocal(c, box);
		int hx = 0, hy = 0;
		bool rotate = false;
		if (HitHandle(l, box, kHandlePx * 1.4 / s, hx, hy, rotate)) {
			if (rotate)
				setCursor(Qt::CrossCursor);
			else if (hx && hy)
				setCursor((hx == hy) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
			else
				setCursor(hx ? Qt::SizeHorCursor : Qt::SizeVerCursor);
		} else if (std::abs(l.x()) <= box.w / 2.0 && std::abs(l.y()) <= box.h / 2.0) {
			setCursor(Qt::SizeAllCursor);
		} else {
			unsetCursor();
		}
		return;
	}

	const bool shift = event->modifiers() & Qt::ShiftModifier;
	const ItemTransform &o = dragOrigin;
	ItemTransform t = o;
	t.custom = true;
	const QPointF center0(o.x + o.w / 2.0, o.y + o.h / 2.0);

	if (drag == Drag::Move) {
		t.x = (float)(o.x + c.x() - dragStartCanvas.x());
		t.y = (float)(o.y + c.y() - dragStartCanvas.y());
		if (std::abs(t.rot) < 0.01f) {
			/* Imán a bordes y centro del lienzo */
			uint32_t cw, ch;
			CanvasSize(cw, ch);
			const double snap = 8.0 / s;
			auto snapTo = [&](float &v, float size, double target) {
				if (std::abs(v - target) < snap)
					v = (float)target;
				else if (std::abs(v + size - target) < snap)
					v = (float)(target - size);
				else if (std::abs(v + size / 2 - target) < snap)
					v = (float)(target - size / 2);
			};
			for (double target : {0.0, (double)cw, cw / 2.0})
				snapTo(t.x, t.w, target);
			for (double target : {0.0, (double)ch, ch / 2.0})
				snapTo(t.y, t.h, target);
		}
	} else if (drag == Drag::Rotate) {
		const QPointF a0 = dragStartCanvas - center0;
		const QPointF a1 = c - center0;
		double deg = o.rot + (std::atan2(a1.y(), a1.x()) - std::atan2(a0.y(), a0.x())) * 180.0 / kPi;
		if (shift)
			deg = std::round(deg / 15.0) * 15.0;
		while (deg > 180.0)
			deg -= 360.0;
		while (deg <= -180.0)
			deg += 360.0;
		t.rot = (float)deg;
	} else {
		const QPointF m = Rotate(c - center0, -o.rot);
		double left = -o.w / 2.0, right = o.w / 2.0, top = -o.h / 2.0, bottom = o.h / 2.0;

		if (drag == Drag::Resize) {
			if (dragHx == 1)
				right = m.x();
			if (dragHx == -1)
				left = m.x();
			if (dragHy == 1)
				bottom = m.y();
			if (dragHy == -1)
				top = m.y();
			double w = std::max(4.0, right - left), h = std::max(4.0, bottom - top);
			/* Esquinas: proporción DEL CONTENIDO salvo con Mayús (como OBS); así
			 * una caja deformada recupera su forma al arrastrar una esquina. */
			if (dragHx && dragHy && !shift && o.w > 0 && o.h > 0) {
				const double content = ContentAspect(o);
				const double a = content > 0.0 ? content : o.w / o.h;
				if (w / h > a)
					h = w / a;
				else
					w = h * a;
			}
			if (dragHx == 1)
				right = left + w;
			else if (dragHx == -1)
				left = right - w;
			if (dragHy == 1)
				bottom = top + h;
			else if (dragHy == -1)
				top = bottom - h;
		} else { /* Crop: Alt + asa */
			uint32_t sw, sh;
			SourceSize(sw, sh);
			if (sw && sh) {
				const double sx = o.w / std::max(1.0, (double)sw - o.cropL - o.cropR);
				const double sy = o.h / std::max(1.0, (double)sh - o.cropT - o.cropB);
				auto cropAxis = [&](int handle, double mouse, double half, double scale, bool flip, int &c1,
						    int &c2, int total, double &lo, double &hi) {
					/* handle = +1: lado derecho/inferior visible; -1: izquierdo/superior. */
					if (!handle)
						return;
					int &side = ((handle > 0) != flip) ? c2 : c1;
					const int other = (&side == &c1) ? c2 : c1;
					const double delta = handle > 0 ? (half - mouse) : (mouse + half); /* >0 hacia adentro */
					int value = (int)std::lround(side + delta / scale);
					value = std::clamp(value, 0, std::max(0, total - other - 2));
					const double moved = (value - side) * scale;
					side = value;
					if (handle > 0)
						hi = half - moved;
					else
						lo = -half + moved;
				};
				cropAxis(dragHx, m.x(), o.w / 2.0, sx, o.flipH, t.cropL, t.cropR, (int)sw, left, right);
				cropAxis(dragHy, m.y(), o.h / 2.0, sy, o.flipV, t.cropT, t.cropB, (int)sh, top, bottom);
			}
		}
		const QPointF lc((left + right) / 2.0, (top + bottom) / 2.0);
		const QPointF wc = center0 + Rotate(lc, o.rot);
		t.w = (float)(right - left);
		t.h = (float)(bottom - top);
		t.x = (float)(wc.x() - t.w / 2.0);
		t.y = (float)(wc.y() - t.h / 2.0);
	}
	Apply(t, false);
}

void TransformCanvas::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || drag == Drag::None)
		return;
	drag = Drag::None;
	if (transform.custom)
		Apply(transform, true);
}

void TransformCanvas::keyPressEvent(QKeyEvent *event)
{
	if (!current) {
		QWidget::keyPressEvent(event);
		return;
	}
	const float step = (event->modifiers() & Qt::ShiftModifier) ? 10.0f : 1.0f;
	ItemTransform t = Effective();
	t.custom = true;
	switch (event->key()) {
	case Qt::Key_Left:
		t.x -= step;
		break;
	case Qt::Key_Right:
		t.x += step;
		break;
	case Qt::Key_Up:
		t.y -= step;
		break;
	case Qt::Key_Down:
		t.y += step;
		break;
	default:
		QWidget::keyPressEvent(event);
		return;
	}
	Apply(t, true);
}

/* ---- Pantalla ---- */

void TransformCanvas::CreateDisplay()
{
	if (display || destroying || !isVisible())
		return;
	(void)winId();
	QWindow *window = windowHandle();
	if (!window)
		return;
	if (!filterInstalled) {
		window->installEventFilter(new CanvasSurfaceFilter(this));
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
	display = obs_display_create(&info, 0xFF2A2A2E);
	if (display)
		obs_display_add_draw_callback(display, DrawCallback, this);
}

void TransformCanvas::DestroyDisplay()
{
	if (!display)
		return;
	obs_display_remove_draw_callback(display, DrawCallback, this);
	obs_display_destroy(display);
	display = nullptr;
}

void TransformCanvas::DrawCallback(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<TransformCanvas *>(data);
	if (!cx || !cy || !self->scene)
		return;
	uint32_t cw, ch;
	CanvasSize(cw, ch);
	const double scale = std::min(cx * (1.0 - 2 * kMargin) / cw, cy * (1.0 - 2 * kMargin) / ch);
	const double ox = (cx - cw * scale) / 2.0;
	const double oy = (cy - ch * scale) / 2.0;
	const float px = (float)(1.0 / scale);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_ortho((float)(-ox / scale), (float)((cx - ox) / scale), (float)(-oy / scale), (float)((cy - oy) / scale),
		 -100.0f, 100.0f);

	/* Lienzo y resultado exacto (escena privada con la misma transformación). */
	gs_blend_state_push();
	gs_enable_blending(false);
	DrawSolid(solid, color, 0, 0, (float)cw, (float)ch, 0xFF000000);
	gs_blend_state_pop();
	obs_source_video_render(obs_scene_get_source(self->scene));

	/* Borde del lienzo */
	gs_blend_state_push();
	gs_enable_blending(false);
	DrawFrame(solid, color, -px, -px, (float)cw + 2 * px, (float)ch + 2 * px, px, 0xFF707078);

	ItemTransform b;
	bool valid;
	{
		std::lock_guard<std::mutex> lock(self->overlayMutex);
		b = self->overlay;
		valid = self->overlayValid;
	}
	if (valid && b.w > 0 && b.h > 0) {
		const uint32_t red = 0xFFE0312B;
		const float hs = (float)kHandlePx * px;
		gs_matrix_push();
		gs_matrix_translate3f(b.x + b.w / 2.0f, b.y + b.h / 2.0f, 0.0f);
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, (float)(b.rot * kPi / 180.0));
		DrawFrame(solid, color, -b.w / 2, -b.h / 2, b.w, b.h, 1.5f * px, red);
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++)
				if (x || y)
					DrawSolid(solid, color, x * b.w / 2 - hs / 2, y * b.h / 2 - hs / 2, hs, hs, red);
		/* Asa de rotación */
		const float gap = (float)kRotateGapPx * px;
		DrawSolid(solid, color, -px / 2, -b.h / 2 - gap, px, gap, red);
		DrawSolid(solid, color, -hs / 2, -b.h / 2 - gap - hs / 2, hs, hs, 0xFFFFFFFF);
		DrawFrame(solid, color, -hs / 2, -b.h / 2 - gap - hs / 2, hs, hs, px, red);
		gs_matrix_pop();
	}
	gs_blend_state_pop();

	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
}

void TransformCanvas::paintEvent(QPaintEvent *)
{
	CreateDisplay();
}

void TransformCanvas::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	CreateDisplay();
	if (display)
		obs_display_set_enabled(display, true);
	if (scene && !showingHeld) {
		obs_source_inc_showing(obs_scene_get_source(scene));
		showingHeld = true;
	}
}

void TransformCanvas::hideEvent(QHideEvent *event)
{
	if (display)
		obs_display_set_enabled(display, false);
	if (scene && showingHeld) {
		obs_source_dec_showing(obs_scene_get_source(scene));
		showingHeld = false;
	}
	QWidget::hideEvent(event);
}

void TransformCanvas::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	CreateDisplay();
	if (display && isVisible()) {
		const QSize size = PixelSize();
		obs_display_resize(display, (uint32_t)size.width(), (uint32_t)size.height());
	}
}

/* ======================================================================== */
/* TransformEditor                                                           */

TransformEditor::TransformEditor(QWidget *parent) : QDialog(parent, Qt::Window)
{
	setWindowTitle(T("Editor.Title"));
	setModal(false);
	resize(1100, 620);

	auto *root = new QHBoxLayout(this);
	auto *left = new QVBoxLayout();
	titleLabel = new QLabel(this);
	titleLabel->setStyleSheet("font-weight:bold;font-size:14px;");
	left->addWidget(titleLabel);
	canvas = new TransformCanvas(this);
	left->addWidget(canvas, 1);
	auto *hint = new QLabel(T("Editor.Hint"), this);
	hint->setWordWrap(true);
	hint->setStyleSheet("color:#9aa0aa;");
	left->addWidget(hint);
	root->addLayout(left, 1);

	auto *side = new QVBoxLayout();
	auto mkSpin = [this](double min, double max, int decimals, const QString &suffix) {
		auto *s = new QDoubleSpinBox(this);
		s->setRange(min, max);
		s->setDecimals(decimals);
		s->setSuffix(suffix);
		s->setKeyboardTracking(false);
		s->setMinimumWidth(110);
		return s;
	};
	auto mkInt = [this]() {
		auto *s = new QSpinBox(this);
		s->setRange(0, 16384);
		s->setSuffix(" px");
		s->setKeyboardTracking(false);
		return s;
	};

	auto *geo = new QGroupBox(T("Editor.Geometry"), this);
	auto *form = new QFormLayout(geo);
	posX = mkSpin(-20000, 20000, 1, " px");
	posY = mkSpin(-20000, 20000, 1, " px");
	sizeW = mkSpin(1, 20000, 1, " px");
	sizeH = mkSpin(1, 20000, 1, " px");
	rotation = mkSpin(-360, 360, 1, QString::fromUtf8(" °"));
	lockAspect = new QCheckBox(T("Editor.LockAspect"), this);
	lockAspect->setChecked(true);
	form->addRow(T("Editor.PosX"), posX);
	form->addRow(T("Editor.PosY"), posY);
	form->addRow(T("Editor.Width"), sizeW);
	form->addRow(T("Editor.Height"), sizeH);
	form->addRow(QString(), lockAspect);
	form->addRow(T("Editor.Rotation"), rotation);
	side->addWidget(geo);

	auto *crop = new QGroupBox(T("Editor.Crop"), this);
	auto *cropForm = new QFormLayout(crop);
	cropL = mkInt();
	cropT = mkInt();
	cropR = mkInt();
	cropB = mkInt();
	cropForm->addRow(T("Editor.CropLeft"), cropL);
	cropForm->addRow(T("Editor.CropTop"), cropT);
	cropForm->addRow(T("Editor.CropRight"), cropR);
	cropForm->addRow(T("Editor.CropBottom"), cropB);
	side->addWidget(crop);

	auto *quick = new QGroupBox(T("Editor.Quick"), this);
	auto *grid = new QGridLayout(quick);
	auto mkBtn = [this](const char *key) {
		auto *b = new QPushButton(T(key), this);
		/* Sin "botón por defecto": Enter en un campo numérico no debe disparar
		 * la última acción rápida pulsada (p. ej. Restablecer o Girar). */
		b->setAutoDefault(false);
		b->setDefault(false);
		return b;
	};
	QPushButton *fit = mkBtn("Media.Fit"), *fill = mkBtn("Media.Fill"), *orig = mkBtn("Media.Original");
	QPushButton *center = mkBtn("Editor.Center"), *rotL = mkBtn("Editor.RotL"), *rotR = mkBtn("Editor.RotR");
	QPushButton *flipH = mkBtn("Editor.FlipH"), *flipV = mkBtn("Editor.FlipV"), *reset = mkBtn("Editor.Reset");
	fit->setToolTip(T("Media.FitTip"));
	fill->setToolTip(T("Media.FillTip"));
	orig->setToolTip(T("Media.OriginalTip"));
	grid->addWidget(fit, 0, 0);
	grid->addWidget(fill, 0, 1);
	grid->addWidget(orig, 0, 2);
	grid->addWidget(center, 1, 0);
	grid->addWidget(rotL, 1, 1);
	grid->addWidget(rotR, 1, 2);
	grid->addWidget(flipH, 2, 0);
	grid->addWidget(flipV, 2, 1);
	grid->addWidget(reset, 2, 2);
	side->addWidget(quick);
	side->addStretch(1);
	auto *close = new QPushButton(T("Editor.Close"), this);
	side->addWidget(close);
	root->addLayout(side);

	/* Conexiones */
	canvas->onSourceSizeChanged = [this]() { SyncFields(); };
	canvas->onChanged = [this](const ItemTransform &t, bool final) {
		SyncFields();
		if (onChanged)
			onChanged(t, final);
	};
	for (QDoubleSpinBox *s : {posX, posY, rotation})
		connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { FromFields(true); });
	connect(sizeW, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
		if (syncing)
			return;
		const double a = LockedAspect();
		if (lockAspect->isChecked() && a > 0.0) {
			syncing = true;
			sizeH->setValue(v / a);
			syncing = false;
		}
		FromFields(true);
	});
	connect(sizeH, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
		if (syncing)
			return;
		const double a = LockedAspect();
		if (lockAspect->isChecked() && a > 0.0) {
			syncing = true;
			sizeW->setValue(v * a);
			syncing = false;
		}
		FromFields(true);
	});
	for (QSpinBox *s : {cropL, cropT, cropR, cropB})
		connect(s, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
			if (!syncing)
				SetCrop(cropL->value(), cropT->value(), cropR->value(), cropB->value());
		});
	connect(fit, &QPushButton::clicked, this, [this]() { Preset(0); });
	connect(fill, &QPushButton::clicked, this, [this]() { Preset(1); });
	connect(orig, &QPushButton::clicked, this, [this]() { Preset(2); });
	connect(center, &QPushButton::clicked, this, [this]() { Preset(3); });
	connect(rotL, &QPushButton::clicked, this, [this]() { Preset(4); });
	connect(rotR, &QPushButton::clicked, this, [this]() { Preset(5); });
	connect(flipH, &QPushButton::clicked, this, [this]() { Preset(6); });
	connect(flipV, &QPushButton::clicked, this, [this]() { Preset(7); });
	connect(reset, &QPushButton::clicked, this, [this]() { Preset(8); });
	close->setAutoDefault(false);
	connect(close, &QPushButton::clicked, this, [this]() { reject(); });
}

TransformEditor::~TransformEditor()
{
	canvas->Shutdown();
}

obs_source_t *TransformEditor::Source() const
{
	return canvas->Source();
}

void TransformEditor::Open(obs_source_t *source, const ItemTransform &t, bool activate, const QString &title)
{
	titleLabel->setText(title);
	setWindowTitle(T("Editor.Title") + " - " + title);
	canvas->SetSource(source, t, activate);
	SyncFields();
	if (!isVisible())
		show();
	raise();
	activateWindow();
	canvas->setFocus();
	/* El tamaño de un video se conoce después de cargar: refrescar campos. */
	QTimer::singleShot(400, this, [this]() { SyncFields(); });
}

void TransformEditor::Detach()
{
	canvas->SetSource(nullptr, ItemTransform(), false);
}

void TransformEditor::closeEvent(QCloseEvent *event)
{
	/* QDialog::closeEvent llama a reject(), que oculta la ventana. */
	QDialog::closeEvent(event);
}

void TransformEditor::reject()
{
	Detach();
	if (onClosed)
		onClosed();
	QDialog::reject(); /* oculta la ventana (se reutiliza la próxima vez) */
}

double TransformEditor::LockedAspect() const
{
	/* La proporción del contenido (no la de la caja, que pudo quedar deformada). */
	const ItemTransform e = canvas->Effective();
	const double content = canvas->ContentAspect(e);
	if (content > 0.0)
		return content;
	return e.h > 0 ? e.w / e.h : 0.0;
}

void TransformEditor::SyncFields()
{
	const ItemTransform e = canvas->Effective();
	syncing = true;
	for (QDoubleSpinBox *s : {posX, posY, sizeW, sizeH, rotation})
		s->blockSignals(true);
	for (QSpinBox *s : {cropL, cropT, cropR, cropB})
		s->blockSignals(true);
	posX->setValue(e.x);
	posY->setValue(e.y);
	sizeW->setValue(e.w);
	sizeH->setValue(e.h);
	rotation->setValue(e.rot);
	cropL->setValue(e.cropL);
	cropT->setValue(e.cropT);
	cropR->setValue(e.cropR);
	cropB->setValue(e.cropB);
	for (QDoubleSpinBox *s : {posX, posY, sizeW, sizeH, rotation})
		s->blockSignals(false);
	for (QSpinBox *s : {cropL, cropT, cropR, cropB})
		s->blockSignals(false);
	syncing = false;
}

void TransformEditor::FromFields(bool final)
{
	if (syncing || !canvas->Source())
		return;
	ItemTransform t = canvas->Effective();
	t.custom = true;
	t.x = (float)posX->value();
	t.y = (float)posY->value();
	t.w = (float)sizeW->value();
	t.h = (float)sizeH->value();
	t.rot = (float)rotation->value();
	canvas->SetTransform(t);
	if (onChanged)
		onChanged(t, final);
}

void TransformEditor::SetCrop(int l, int tp, int r, int b)
{
	if (!canvas->Source())
		return;
	/* Mantiene la escala del contenido: la caja se encoge o crece con el recorte. */
	ItemTransform o = canvas->Effective();
	uint32_t sw, sh;
	canvas->SourceSize(sw, sh);
	if (!sw || !sh)
		return;
	l = std::clamp(l, 0, (int)sw - 2);
	r = std::clamp(r, 0, (int)sw - l - 2);
	tp = std::clamp(tp, 0, (int)sh - 2);
	b = std::clamp(b, 0, (int)sh - tp - 2);
	const double sx = o.w / std::max(1.0, (double)sw - o.cropL - o.cropR);
	const double sy = o.h / std::max(1.0, (double)sh - o.cropT - o.cropB);
	/* Lados visibles (con volteo los recortes se intercambian en pantalla) */
	const int dl = o.flipH ? (r - o.cropR) : (l - o.cropL);
	const int dr = o.flipH ? (l - o.cropL) : (r - o.cropR);
	const int dt = o.flipV ? (b - o.cropB) : (tp - o.cropT);
	const int db = o.flipV ? (tp - o.cropT) : (b - o.cropB);
	const double left = -o.w / 2.0 + dl * sx, right = o.w / 2.0 - dr * sx;
	const double top = -o.h / 2.0 + dt * sy, bottom = o.h / 2.0 - db * sy;
	const QPointF c0(o.x + o.w / 2.0, o.y + o.h / 2.0);
	const QPointF wc = c0 + Rotate(QPointF((left + right) / 2.0, (top + bottom) / 2.0), o.rot);

	ItemTransform t = o;
	t.custom = true;
	t.cropL = l;
	t.cropR = r;
	t.cropT = tp;
	t.cropB = b;
	t.w = (float)std::max(4.0, right - left);
	t.h = (float)std::max(4.0, bottom - top);
	t.x = (float)(wc.x() - t.w / 2.0);
	t.y = (float)(wc.y() - t.h / 2.0);
	canvas->SetTransform(t);
	SyncFields();
	if (onChanged)
		onChanged(t, true);
}

void TransformEditor::Preset(int which)
{
	if (!canvas->Source())
		return;
	uint32_t cw, ch, sw, sh;
	CanvasSize(cw, ch);
	canvas->SourceSize(sw, sh);
	ItemTransform t = canvas->Effective();
	switch (which) {
	case 0: /* Ajustar (conserva rotación/recorte/volteo) */
		t.custom = false;
		t = t.Effective(sw, sh, cw, ch);
		t.custom = true;
		break;
	case 1: /* Llenar */
		t.custom = true;
		t.x = 0;
		t.y = 0;
		t.w = (float)cw;
		t.h = (float)ch;
		break;
	case 2: /* Original */
		if (sw && sh) {
			t.custom = true;
			t.w = (float)std::max(1, (int)sw - t.cropL - t.cropR);
			t.h = (float)std::max(1, (int)sh - t.cropT - t.cropB);
			t.x = (cw - t.w) / 2.0f;
			t.y = (ch - t.h) / 2.0f;
		}
		break;
	case 3: /* Centrar */
		t.custom = true;
		t.x = (cw - t.w) / 2.0f;
		t.y = (ch - t.h) / 2.0f;
		break;
	case 4:
	case 5: { /* Rotar 90° */
		t.custom = true;
		float r = t.rot + (which == 4 ? -90.0f : 90.0f);
		while (r > 180.0f)
			r -= 360.0f;
		while (r <= -180.0f)
			r += 360.0f;
		t.rot = r;
		break;
	}
	case 6:
		t.custom = true;
		t.flipH = !t.flipH;
		break;
	case 7:
		t.custom = true;
		t.flipV = !t.flipV;
		break;
	case 8: /* Restablecer todo: ajustar sin rotación, recorte ni volteo */
		t = ItemTransform();
		break;
	}
	canvas->SetTransform(t);
	SyncFields();
	if (onChanged)
		onChanged(t, true);
}
