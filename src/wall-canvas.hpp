/*
 * Tools Scenes - vista previa de escenas tipo vMix para OBS Studio
 *
 * WallCanvas: widget nativo con UN solo obs_display que dibuja todas las
 * miniaturas en el hilo gráfico de OBS.
 *
 * Reglas de diseño (para no causar tirones ni bloqueos):
 *  - Nunca se hace lectura de GPU a CPU (sin stagesurface / map).
 *  - El hilo de interfaz nunca entra al contexto gráfico durante el uso normal
 *    (solo al destruir la pantalla o al cambiar de colección de escenas).
 *  - El hilo gráfico solo lee una "instantánea" inmutable protegida por un
 *    mutex que se mantiene unos nanosegundos (copia de shared_ptr).
 *  - Las referencias a fuentes que usa el hilo gráfico las posee la
 *    instantánea; el hilo gráfico nunca libera la última referencia.
 *  - Las miniaturas se actualizan con un presupuesto por fotograma repartido
 *    entre todas las escenas visibles, de modo que el costo es constante.
 */
#pragma once

#include <obs.h>

#include <QWidget>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/* Imagen de rótulo (BGRA premultiplicado) generada con QPainter en el hilo UI. */
struct WallLabelImage {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> pixels;
	uint64_t id = 0;
};

struct WallColor {
	float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

struct WallTile {
	obs_source_t *source = nullptr; /* referencia propiedad de WallSnapshot::refs */
	std::string key;                /* UUID estable de la escena */
	/* Rectángulos en píxeles físicos, relativos al contenido (sin scroll). */
	int x = 0, y = 0, w = 0;
	int headerH = 0;
	int imageH = 0;
	WallColor headerColor;
	std::shared_ptr<const WallLabelImage> label;
};

class WallSourceRef {
public:
	explicit WallSourceRef(obs_source_t *src) : source(obs_source_get_ref(src)) {}
	~WallSourceRef() { obs_source_release(source); }
	WallSourceRef(const WallSourceRef &) = delete;
	WallSourceRef &operator=(const WallSourceRef &) = delete;
	obs_source_t *get() const { return source; }

private:
	obs_source_t *source;
};

struct WallSnapshot {
	std::vector<WallTile> tiles;
	std::vector<std::unique_ptr<WallSourceRef>> refs;
	int contentHeight = 0; /* píxeles físicos */
};

class WallCanvas : public QWidget {
public:
	explicit WallCanvas(QWidget *parent);
	~WallCanvas() override;

	/* Hilo UI. Publica una nueva instantánea para el render. */
	void SetSnapshot(std::shared_ptr<const WallSnapshot> snapshot);
	/* Hilo UI. Libera todas las fuentes y espera a que el render termine el
	 * fotograma en curso. Usar en limpieza de colección y al salir. */
	void ReleaseAllSync();
	/* Hilo UI. Libera instantáneas antiguas que ya no usa el render. */
	void PurgeRetired();

	void SetScrollY(int physicalPixels) { scrollY.store(physicalPixels); }
	void SetActiveScenes(obs_source_t *program, obs_source_t *preview)
	{
		programPtr.store(program);
		previewPtr.store(preview);
	}
	/* 0 = tiempo real (cada fotograma de OBS). */
	void SetRefreshFps(int fps) { refreshFps.store(fps < 0 ? 0 : fps); }
	void SetHoverIndex(int index) { hoverIndex.store(index); }

	void DestroyDisplay();
	/* Destruye la pantalla y evita que se vuelva a crear (salida de OBS). */
	void Shutdown()
	{
		destroying = true;
		DestroyDisplay();
	}

	std::function<void(QMouseEvent *)> onMousePress;
	std::function<void(QMouseEvent *)> onMouseDoubleClick;
	std::function<void(QMouseEvent *)> onMouseMove;
	std::function<void()> onMouseLeave;
	std::function<void(QWheelEvent *)> onWheel;
	std::function<void()> onResized;

protected:
	QPaintEngine *paintEngine() const override { return nullptr; }
	void paintEvent(QPaintEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void moveEvent(QMoveEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void leaveEvent(QEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
	friend class WallSurfaceFilter;

	struct TileGpu {
		gs_texrender_t *texrender = nullptr;
		gs_texture_t *labelTex = nullptr;
		uint64_t labelId = 0;
		uint64_t lastRenderNs = 0;
		uint64_t lastSeenFrame = 0;
		bool hasImage = false;
	};

	static void DrawCallback(void *data, uint32_t cx, uint32_t cy);
	void Draw(uint32_t cx, uint32_t cy);
	void RenderTileTexture(const WallTile &tile, TileGpu &gpu);
	void Compose(const WallSnapshot &snap, int scroll, uint32_t cx, uint32_t cy);
#ifdef SCENE_PREVIO_DEBUG
	void DebugAfterDraw(const WallSnapshot &snap, int scroll, uint32_t cx, uint32_t cy, uint64_t drawNs);
	uint64_t debugSumNs = 0, debugMaxNs = 0;
	uint32_t debugFrames = 0;
	bool debugCaptured = false;
#endif
	void FreeGpuCache(); /* debe llamarse dentro del contexto gráfico */
	void CreateDisplay();
	void InstallSurfaceFilter();
	QSize PixelSize() const;

	obs_display_t *display = nullptr;
	bool surfaceFilterInstalled = false;
	bool destroying = false;

	/* Compartido UI <-> render */
	std::mutex snapshotMutex;
	std::shared_ptr<const WallSnapshot> current;
	std::vector<std::shared_ptr<const WallSnapshot>> retired; /* solo UI */

	std::atomic<int> scrollY{0};
	std::atomic<int> refreshFps{10};
	std::atomic<int> hoverIndex{-1};
	std::atomic<obs_source_t *> programPtr{nullptr};
	std::atomic<obs_source_t *> previewPtr{nullptr};

	/* Solo hilo gráfico */
	std::unordered_map<std::string, TileGpu> gpuCache;
	uint64_t frameCounter = 0;
	uint64_t lastDrawNs = 0;
	double frameIntervalNs = 1e9 / 30.0;
	std::vector<std::pair<uint64_t, size_t>> dueScratch;
	std::vector<TileGpu *> gpuScratch;
};
