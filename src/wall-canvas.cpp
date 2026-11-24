/*
 * Tools Scenes - vista previa de escenas tipo vMix para OBS Studio
 * Implementación del lienzo de miniaturas (ver wall-canvas.hpp).
 */
#include "wall-canvas.hpp"

#include <util/platform.h>

#include <QEvent>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QWheelEvent>
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

/* ------------------------------------------------------------------------ */
/* Filtro de eventos de la superficie nativa: si Qt va a destruir la ventana  */
/* nativa (p. ej. al desacoplar/acoplar el dock) la pantalla de OBS debe      */
/* destruirse ANTES, o el hilo gráfico dibujaría sobre un HWND inválido.      */

class WallSurfaceFilter : public QObject {
public:
	explicit WallSurfaceFilter(WallCanvas *canvas_) : QObject(canvas_), canvas(canvas_) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		if (event->type() == QEvent::PlatformSurface) {
			auto *surfaceEvent = static_cast<QPlatformSurfaceEvent *>(event);
			if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
				canvas->DestroyDisplay();
		}
		return QObject::eventFilter(obj, event);
	}

private:
	WallCanvas *canvas;
};

/* ------------------------------------------------------------------------ */

WallCanvas::WallCanvas(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMouseTracking(true);
	setFocusPolicy(Qt::NoFocus);
	setMinimumSize(120, 80);
}

WallCanvas::~WallCanvas()
{
	destroying = true;
	DestroyDisplay();
	{
		std::lock_guard<std::mutex> lock(snapshotMutex);
		current.reset();
	}
	retired.clear();
}

QSize WallCanvas::PixelSize() const
{
	const qreal dpr = devicePixelRatioF();
	return QSize((int)std::lround(width() * dpr), (int)std::lround(height() * dpr));
}

void WallCanvas::InstallSurfaceFilter()
{
	if (surfaceFilterInstalled)
		return;
	QWindow *window = windowHandle();
	if (!window)
		return;
	window->installEventFilter(new WallSurfaceFilter(this));
	QObject::connect(window, &QWindow::screenChanged, this, [this](QScreen *) {
		if (display) {
			const QSize size = PixelSize();
			obs_display_resize(display, (uint32_t)size.width(), (uint32_t)size.height());
			obs_display_update_color_space(display);
		}
		if (onResized)
			onResized();
	});
	surfaceFilterInstalled = true;
}

void WallCanvas::CreateDisplay()
{
	if (display || destroying)
		return;
	if (!isVisible())
		return;

	(void)winId(); /* fuerza la creación de la ventana nativa */
	InstallSurfaceFilter();

	QWindow *window = windowHandle();
	if (!window || !window->isExposed())
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
		return; /* Wayland no soportado en esta versión */
	info.window.id = window->winId();
	info.window.display = obs_get_nix_platform_display();
#endif

	display = obs_display_create(&info, 0xFF1A1A1A);
	if (!display) {
		blog(LOG_WARNING, "[tools-scenes] no se pudo crear obs_display");
		return;
	}
	obs_display_add_draw_callback(display, DrawCallback, this);
}

void WallCanvas::DestroyDisplay()
{
	if (!display)
		return;

	obs_display_remove_draw_callback(display, DrawCallback, this);
	/* obs_display_destroy espera a que el hilo gráfico termine con la pantalla. */
	obs_display_destroy(display);
	display = nullptr;

	/* Ya no hay callback activo: liberar recursos de GPU. */
	obs_enter_graphics();
	FreeGpuCache();
	obs_leave_graphics();
}

void WallCanvas::SetSnapshot(std::shared_ptr<const WallSnapshot> snapshot)
{
	std::shared_ptr<const WallSnapshot> old;
	{
		std::lock_guard<std::mutex> lock(snapshotMutex);
		old = std::move(current);
		current = std::move(snapshot);
	}
	if (old)
		retired.push_back(std::move(old));
	PurgeRetired();
}

void WallCanvas::PurgeRetired()
{
	/* Una instantánea retirada ya no es accesible para el render (no está en
	 * `current`), así que si su contador es 1 solo la tenemos nosotros y se
	 * puede liberar en este hilo (UI) con seguridad. */
	retired.erase(std::remove_if(retired.begin(), retired.end(),
				     [](const std::shared_ptr<const WallSnapshot> &s) { return s.use_count() <= 1; }),
		      retired.end());
}

void WallCanvas::ReleaseAllSync()
{
	{
		std::lock_guard<std::mutex> lock(snapshotMutex);
		if (current)
			retired.push_back(std::move(current));
		current.reset();
	}
	/* Esperar a que termine cualquier fotograma en curso (render_displays se
	 * ejecuta dentro del contexto gráfico). Operación poco frecuente. */
	if (display) {
		obs_enter_graphics();
		obs_leave_graphics();
	}
	retired.clear();
	programPtr.store(nullptr);
	previewPtr.store(nullptr);
}

/* ------------------------------------------------------------------------ */
/* Eventos Qt                                                                */

void WallCanvas::paintEvent(QPaintEvent *)
{
	CreateDisplay();
}

void WallCanvas::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	CreateDisplay();
	if (display)
		obs_display_set_enabled(display, true);
}

void WallCanvas::hideEvent(QHideEvent *event)
{
	/* Oculto = costo cero: OBS omite las pantallas deshabilitadas. */
	if (display)
		obs_display_set_enabled(display, false);
	QWidget::hideEvent(event);
}

void WallCanvas::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	CreateDisplay();
	if (display && isVisible()) {
		const QSize size = PixelSize();
		obs_display_resize(display, (uint32_t)size.width(), (uint32_t)size.height());
	}
	if (onResized)
		onResized();
}

void WallCanvas::moveEvent(QMoveEvent *event)
{
	QWidget::moveEvent(event);
	if (display)
		obs_display_update_color_space(display);
}

bool WallCanvas::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG *msg = static_cast<const MSG *>(message);
	if (msg && msg->message == WM_DISPLAYCHANGE && display)
		obs_display_update_color_space(display);
#else
	(void)message;
#endif
	return false;
}

void WallCanvas::mousePressEvent(QMouseEvent *event)
{
	if (onMousePress)
		onMousePress(event);
}

void WallCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (onMouseDoubleClick)
		onMouseDoubleClick(event);
}

void WallCanvas::mouseMoveEvent(QMouseEvent *event)
{
	if (onMouseMove)
		onMouseMove(event);
}

void WallCanvas::leaveEvent(QEvent *event)
{
	if (onMouseLeave)
		onMouseLeave();
	QWidget::leaveEvent(event);
}

void WallCanvas::wheelEvent(QWheelEvent *event)
{
	if (onWheel)
		onWheel(event);
}

/* ------------------------------------------------------------------------ */
/* Hilo gráfico                                                              */

void WallCanvas::DrawCallback(void *data, uint32_t cx, uint32_t cy)
{
	static_cast<WallCanvas *>(data)->Draw(cx, cy);
}

void WallCanvas::FreeGpuCache()
{
	for (auto &entry : gpuCache) {
		gs_texrender_destroy(entry.second.texrender);
		gs_texture_destroy(entry.second.labelTex);
	}
	gpuCache.clear();
}

void WallCanvas::RenderTileTexture(const WallTile &tile, TileGpu &gpu)
{
	if (!tile.source || tile.w <= 0 || tile.imageH <= 0)
		return;

	const uint32_t sw = obs_source_get_base_width(tile.source);
	const uint32_t sh = obs_source_get_base_height(tile.source);
	if (!sw || !sh)
		return;

	if (!gpu.texrender) {
		gpu.texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!gpu.texrender)
			return;
	}

	gs_texrender_reset(gpu.texrender);
	if (!gs_texrender_begin_with_color_space(gpu.texrender, (uint32_t)tile.w, (uint32_t)tile.imageH, GS_CS_SRGB))
		return;

	struct vec4 clear;
	vec4_set(&clear, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_reset_blend_state();
	obs_source_video_render(tile.source);
	gs_blend_state_pop();

	gs_texrender_end(gpu.texrender);
	gpu.hasImage = true;
}

static void draw_solid(gs_effect_t *solid, gs_eparam_t *colorParam, float x, float y, float w, float h,
		       const WallColor &c)
{
	if (w <= 0.0f || h <= 0.0f)
		return;
	struct vec4 color;
	vec4_set(&color, c.r, c.g, c.b, c.a);
	gs_effect_set_vec4(colorParam, &color);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, (uint32_t)w, (uint32_t)h);
	gs_matrix_pop();
}

static void draw_frame(gs_effect_t *solid, gs_eparam_t *colorParam, float x, float y, float w, float h, float t,
		       const WallColor &c)
{
	draw_solid(solid, colorParam, x, y, w, t, c);
	draw_solid(solid, colorParam, x, y + h - t, w, t, c);
	draw_solid(solid, colorParam, x, y + t, t, h - 2 * t, c);
	draw_solid(solid, colorParam, x + w - t, y + t, t, h - 2 * t, c);
}

void WallCanvas::Draw(uint32_t cx, uint32_t cy)
{
	std::shared_ptr<const WallSnapshot> snap;
	{
		std::lock_guard<std::mutex> lock(snapshotMutex);
		snap = current;
	}

	frameCounter++;
	const uint64_t now = os_gettime_ns();
	if (lastDrawNs && now > lastDrawNs) {
		const double dt = (double)(now - lastDrawNs);
		if (dt < 1e9)
			frameIntervalNs = frameIntervalNs * 0.9 + dt * 0.1;
	}
	lastDrawNs = now;

	/* Recolección de recursos de miniaturas que ya no se ven (otra pestaña,
	 * escena eliminada). Barato y poco frecuente. */
	if ((frameCounter % 120) == 0) {
		for (auto it = gpuCache.begin(); it != gpuCache.end();) {
			if (it->second.lastSeenFrame + 600 < frameCounter) {
				gs_texrender_destroy(it->second.texrender);
				gs_texture_destroy(it->second.labelTex);
				it = gpuCache.erase(it);
			} else {
				++it;
			}
		}
	}

	if (!snap || snap->tiles.empty())
		return;

	const int scroll = scrollY.load();
	const int fps = refreshFps.load();
	const uint64_t interval = fps > 0 ? (uint64_t)(1000000000ULL / (uint64_t)fps) : 0;

	/* ---- 1) Actualizar texturas con presupuesto por fotograma ---- */
	const size_t count = snap->tiles.size();
	gpuScratch.assign(count, nullptr);
	std::vector<TileGpu *> &gpuForTile = gpuScratch;
	dueScratch.clear();
	size_t visibleCount = 0;

	for (size_t i = 0; i < count; i++) {
		const WallTile &tile = snap->tiles[i];
		const int top = tile.y - scroll;
		const int bottom = top + tile.headerH + tile.imageH;
		if (bottom < 0 || top > (int)cy)
			continue; /* fuera de pantalla: costo cero */

		visibleCount++;
		TileGpu &gpu = gpuCache[tile.key];
		gpu.lastSeenFrame = frameCounter;
		gpuForTile[i] = &gpu;

		const bool due = !gpu.hasImage || interval == 0 || (now - gpu.lastRenderNs) >= interval;
		if (due)
			dueScratch.emplace_back(gpu.hasImage ? gpu.lastRenderNs : 0, i);
	}

	size_t budget = dueScratch.size();
	if (interval > 0 && visibleCount > 0) {
		/* Repartir uniformemente: N escenas a F fps sobre fotogramas de OBS. */
		const double perFrame = (double)visibleCount * frameIntervalNs / (double)interval;
		budget = std::min(budget, std::max<size_t>(1, (size_t)std::ceil(perFrame)));
	}
	if (budget < dueScratch.size()) {
		std::partial_sort(dueScratch.begin(), dueScratch.begin() + (ptrdiff_t)budget, dueScratch.end(),
				  [](const auto &a, const auto &b) { return a.first < b.first; });
	}
	for (size_t k = 0; k < budget && k < dueScratch.size(); k++) {
		const size_t i = dueScratch[k].second;
		TileGpu &gpu = *gpuForTile[i];
		RenderTileTexture(snap->tiles[i], gpu);
		gpu.lastRenderNs = now;
	}

	Compose(*snap, scroll, cx, cy);

#ifdef SCENE_PREVIO_DEBUG
	DebugAfterDraw(*snap, scroll, cx, cy, os_gettime_ns() - now);
#endif
}

/* ---- 2) Componer la cuadrícula (usa gpuScratch calculado en Draw) ---- */
void WallCanvas::Compose(const WallSnapshot &snapRef, int scroll, uint32_t cx, uint32_t cy)
{
	const WallSnapshot *snap = &snapRef;
	const size_t count = snap->tiles.size();
	std::vector<TileGpu *> &gpuForTile = gpuScratch;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *solidColor = gs_effect_get_param_by_name(solid, "color");
	gs_effect_t *defaultEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *imageParam = gs_effect_get_param_by_name(defaultEffect, "image");

	const bool previousSrgb = gs_framebuffer_srgb_enabled();

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_blend_state_push();

	obs_source_t *program = programPtr.load();
	obs_source_t *preview = previewPtr.load();
	const int hover = hoverIndex.load();

	const WallColor black{0.0f, 0.0f, 0.0f, 1.0f};
	const WallColor idleFrame{0.30f, 0.30f, 0.30f, 1.0f};
	const WallColor hoverFrame{0.85f, 0.85f, 0.85f, 1.0f};
	const WallColor programColor{0.86f, 0.11f, 0.11f, 1.0f};
	const WallColor previewColor{0.10f, 0.72f, 0.20f, 1.0f};

	for (size_t i = 0; i < count; i++) {
		TileGpu *gpu = gpuForTile[i];
		if (!gpu)
			continue;
		const WallTile &tile = snap->tiles[i];
		const float x = (float)tile.x;
		const float y = (float)(tile.y - scroll);
		const float w = (float)tile.w;
		const float hh = (float)tile.headerH;
		const float ih = (float)tile.imageH;

		const bool isProgram = tile.source == program;
		const bool isPreview = !isProgram && tile.source == preview;

		/* Cabecera y fondo (sin sRGB, colores tal cual) */
		gs_enable_framebuffer_srgb(false);
		gs_enable_blending(false);
		WallColor header = tile.headerColor;
		if (isProgram)
			header = programColor;
		else if (isPreview)
			header = previewColor;
		draw_solid(solid, solidColor, x, y, w, hh, header);

		/* Imagen de la escena */
		gs_texture_t *tex = gpu->hasImage ? gs_texrender_get_texture(gpu->texrender) : nullptr;
		if (tex) {
			gs_enable_framebuffer_srgb(true);
			gs_effect_set_texture_srgb(imageParam, tex);
			gs_matrix_push();
			gs_matrix_translate3f(x, y + hh, 0.0f);
			while (gs_effect_loop(defaultEffect, "Draw"))
				gs_draw_sprite(tex, 0, (uint32_t)tile.w, (uint32_t)tile.imageH);
			gs_matrix_pop();
			gs_enable_framebuffer_srgb(false);
		} else {
			draw_solid(solid, solidColor, x, y + hh, w, ih, black);
		}

		/* Rótulo con el nombre (textura generada por Qt) */
		const WallLabelImage *label = tile.label.get();
		if (label && label->width && label->height) {
			if (gpu->labelId != label->id) {
				gs_texture_destroy(gpu->labelTex);
				const uint8_t *planes[] = {label->pixels.data()};
				gpu->labelTex = gs_texture_create(label->width, label->height, GS_BGRA, 1, planes, 0);
				gpu->labelId = label->id;
			}
			if (gpu->labelTex) {
				gs_enable_blending(true);
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
				gs_effect_set_texture(imageParam, gpu->labelTex);
				gs_matrix_push();
				gs_matrix_translate3f(x, y, 0.0f);
				while (gs_effect_loop(defaultEffect, "Draw"))
					gs_draw_sprite(gpu->labelTex, 0, label->width, label->height);
				gs_matrix_pop();
				gs_enable_blending(false);
			}
		}

		/* Marco: rojo = Programa, verde = Vista previa */
		const float tileH = hh + ih;
		if (isProgram)
			draw_frame(solid, solidColor, x, y, w, tileH, 3.0f, programColor);
		else if (isPreview)
			draw_frame(solid, solidColor, x, y, w, tileH, 3.0f, previewColor);
		else
			draw_frame(solid, solidColor, x, y, w, tileH, 1.0f,
				   (int)i == hover ? hoverFrame : idleFrame);
	}

	gs_blend_state_pop();
	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
	gs_enable_framebuffer_srgb(previousSrgb);
}

#ifdef SCENE_PREVIO_DEBUG
/* Solo en compilaciones de prueba: estadísticas de tiempo de render y una
 * captura del panel compuesto para verificar visualmente sin la pantalla. */
void WallCanvas::DebugAfterDraw(const WallSnapshot &snap, int scroll, uint32_t cx, uint32_t cy, uint64_t drawNs)
{
	debugSumNs += drawNs;
	debugMaxNs = std::max(debugMaxNs, drawNs);
	if (++debugFrames >= 300) {
		blog(LOG_INFO, "[tools-scenes][debug] draw avg %.3f ms max %.3f ms (%zu tiles, fps=%d, %ux%u)",
		     (double)debugSumNs / (double)debugFrames / 1e6, (double)debugMaxNs / 1e6, snap.tiles.size(),
		     refreshFps.load(), cx, cy);
		debugSumNs = debugMaxNs = 0;
		debugFrames = 0;
	}

	const char *path = getenv("SCENE_PREVIO_CAPTURE");
	if (!path || debugCaptured || frameCounter < 240)
		return;
	debugCaptured = true;

	gs_texrender_t *tr = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(cx, cy, GS_BGRA);
	if (tr && stage && gs_texrender_begin(tr, cx, cy)) {
		struct vec4 bg;
		vec4_set(&bg, 0.102f, 0.102f, 0.102f, 1.0f);
		gs_clear(GS_CLEAR_COLOR, &bg, 0.0f, 0);
		Compose(snap, scroll, cx, cy);
		gs_texrender_end(tr);
		gs_stage_texture(stage, gs_texrender_get_texture(tr));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			FILE *f = fopen(path, "wb");
			if (f) {
				fprintf(f, "%u %u\n", cx, cy);
				for (uint32_t y = 0; y < cy; y++)
					fwrite(data + (size_t)y * linesize, 1, (size_t)cx * 4, f);
				fclose(f);
				blog(LOG_INFO, "[tools-scenes][debug] captura escrita en %s", path);
			}
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tr);
}
#endif
