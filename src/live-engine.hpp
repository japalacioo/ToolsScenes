/*
 * Tools Scenes - motor "en vivo" estilo vMix (todo en el hilo de interfaz).
 *
 *  - Envío de cualquier fuente (archivo, NDI, entrada…) a Vista previa o a
 *    Programa sin que el usuario cree escenas: se usan dos escenas gestionadas
 *    "SP Multimedia A/B" que se alternan para poder hacer la transición.
 *  - Capas superpuestas (overlays) sobre la salida de Programa usando los
 *    canales de salida de OBS 10..13 (como un keyer downstream), cada una con
 *    su propia transición de fundido y dos escenas privadas para encadenar.
 *  - Salidas limpias: vistas (obs_view) con el canal 0 de OBS (Programa) y solo
 *    las capas elegidas, hacia un monitor a pantalla completa y/o por NDI.
 */
#pragma once

#include <obs.hpp>

#include <QString>

#include <array>
#include <functional>
#include <string>
#include <vector>

class ProjectorWindow;

constexpr int kLayerCount = 4;
constexpr uint32_t kLayerChannelBase = 10;

/* Transformación de un elemento en coordenadas del lienzo de OBS (como la
 * "Transformación" de OBS). La caja (x, y, w, h) es la caja sin rotar; la
 * rotación es alrededor de su centro. El recorte está en píxeles de la fuente.
 * custom = false: se ajusta al lienzo conservando la proporción. */
struct ItemTransform {
	bool custom = false;
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
	float rot = 0.0f; /* grados, sentido horario */
	int cropL = 0, cropT = 0, cropR = 0, cropB = 0;
	bool flipH = false, flipV = false;

	bool HasExtras() const { return rot != 0.0f || cropL || cropT || cropR || cropB || flipH || flipV; }
	void Save(obs_data_t *data) const;
	static ItemTransform Load(obs_data_t *data);
	/* Caja efectiva para una fuente sw x sh en un lienzo cw x ch. */
	ItemTransform Effective(uint32_t sw, uint32_t sh, uint32_t cw, uint32_t ch) const;
};

class LiveEngine {
public:
	static LiveEngine &Get();

	void EnsureInit();
	void AttachChannels(); /* (re)conecta las capas a los canales de salida */
	void Shutdown();
	void CollectionCleanup();
	void CanvasMaybeChanged();

	/* ---------------- Envío al aire ---------------- */
	bool SendToPreview(obs_source_t *source, const ItemTransform &t);
	bool SendToProgram(obs_source_t *source, const ItemTransform &t);
	/* Fuente colocada en una escena gestionada (identidad; nullptr si no aplica). */
	obs_source_t *ManagedContent(obs_source_t *sceneSource) const;
	/* Aplica en vivo una nueva posición/tamaño donde esté colocada la fuente. */
	void UpdateTransform(obs_source_t *source, const ItemTransform &t);
	bool IsManagedScene(obs_source_t *source) const;
	/* Quita lo que haya en Programa / Vista previa (pasa a una escena vacía). */
	void ClearProgram();
	void ClearPreview();

	/* Biblioteca: OBS destruye las fuentes que nadie usa. Las entradas y
	 * archivos del plugin se guardan como elementos ocultos de la escena
	 * "SP Biblioteca", así sobreviven y se guardan en la colección. */
	void HoldInLibrary(obs_source_t *source);
	void ReleaseFromLibrary(obs_source_t *source);
	static bool IsLibraryScene(obs_source_t *source);

	/* ---------------- Capas ---------------- */
	void LoadLayer(int index, obs_source_t *source, const ItemTransform &t);
	void SetLayerVisible(int index, bool visible);
	bool LayerVisible(int index) const;
	void ClearLayer(int index);
	obs_source_t *LayerContent(int index) const;       /* prestado */
	obs_source_t *LayerOutputSource(int index) const;  /* transición (prestada) */
	QString LayerContentName(int index) const;
	void SetTransitionMs(int ms);
	int TransitionMs() const { return transitionMs; }

	/* Avisos a la interfaz cuando cambian las capas o las escenas gestionadas. */
	void AddListener(void *owner, std::function<void()> fn);
	void RemoveListener(void *owner);

	/* Persistencia por colección de escenas */
	void Save(obs_data_t *data) const;
	void Load(obs_data_t *data);

	/* Utilidad: coloca `source` como único elemento de `scene` con el ajuste dado. */
	static void PlaceInScene(obs_scene_t *scene, obs_source_t *source, const ItemTransform &t);
	static void ApplyTransform(obs_sceneitem_t *item, const ItemTransform &t);
	static void ClearScene(obs_scene_t *scene);

private:
	LiveEngine() = default;

	struct Layer {
		OBSSourceAutoRelease transition;
		std::array<OBSSceneAutoRelease, 2> scenes;
		int activeScene = 0;
		OBSSourceAutoRelease content;
		bool visible = false;
		ItemTransform transform;
	};

	obs_scene_t *ManagedScene(int index, bool create);
	/* forPreview: escena para Vista previa; si no, una libre (ni Programa ni
	 * Vista previa) para enviar al aire sin perder la Vista previa. */
	obs_scene_t *PickTargetScene(bool forPreview);
	void Notify();
	void ApplyCanvasSize();

	bool initialized = false;
	std::array<Layer, kLayerCount> layers;
	std::array<std::string, 3> managedUuids;
	std::string libraryUuid;
	obs_scene_t *LibraryScene(bool create);
	int transitionMs = 500;
	uint32_t canvasW = 0, canvasH = 0;
	std::vector<std::pair<void *, std::function<void()>>> listeners;
};

/* ------------------------------------------------------------------------ */
/* Salidas limpias (monitor + NDI)                                           */

class CleanOutputs {
public:
	static CleanOutputs &Get();

	struct Target {
		obs_view_t *view = nullptr;
		std::array<bool, kLayerCount> layers{};
		obs_source_t *mainPtr = nullptr; /* identidad del canal 0 asignado */
	};

	void Sync();          /* actualizar canal 0 y capas en las vistas */
	void Shutdown();
	void CollectionCleanup();

	/* Monitor */
	bool ShowMonitor(const QString &screenName);
	void HideMonitor();
	bool MonitorActive() const;
	void SetMonitorLayer(int index, bool on);
	bool MonitorLayer(int index) const { return monitor.layers[(size_t)index]; }
	std::function<void()> onMonitorClosed;

	/* NDI (requiere el plugin DistroAV) */
	static bool NdiAvailable();
	bool StartNdi(const QString &name, QString &error);
	void StopNdi();
	bool NdiActive() const;
	void SetNdiLayer(int index, bool on);
	bool NdiLayer(int index) const { return ndi.layers[(size_t)index]; }

private:
	CleanOutputs() = default;
	void EnsureView(Target &t);
	void SyncTarget(Target &t);
	void ReleaseView(Target &t);

	Target monitor;
	Target ndi;
	ProjectorWindow *projector = nullptr;
	obs_output_t *ndiOutput = nullptr;
	bool ndiVideoAdded = false;
};
