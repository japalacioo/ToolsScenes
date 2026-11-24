/*
 * Tools Scenes - motor en vivo (ver live-engine.hpp).
 */
#include "live-engine.hpp"
#include "projector-window.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <functional>

namespace {

constexpr int kManagedCount = 3;
constexpr const char *kManagedNames[kManagedCount] = {"SP Multimedia A", "SP Multimedia B", "SP Multimedia C"};
constexpr const char *kLibraryName = "SP Biblioteca";

void GetCanvas(uint32_t &w, uint32_t &h)
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

} // namespace

/* ======================================================================== */
/* LiveEngine                                                                */

LiveEngine &LiveEngine::Get()
{
	static LiveEngine engine;
	return engine;
}

void LiveEngine::EnsureInit()
{
	if (initialized)
		return;
	initialized = true;

	GetCanvas(canvasW, canvasH);
	for (int i = 0; i < kLayerCount; i++) {
		Layer &l = layers[(size_t)i];
		const std::string name = "SP Capa " + std::to_string(i + 1);
		l.transition = obs_source_create_private("fade_transition", (name + " (transición)").c_str(), nullptr);
		for (int s = 0; s < 2; s++)
			l.scenes[(size_t)s] = obs_scene_create_private((name + (s ? " B" : " A")).c_str());
		if (l.transition) {
			obs_transition_set_size(l.transition, canvasW, canvasH);
			obs_transition_set_alignment(l.transition, OBS_ALIGN_CENTER);
			obs_transition_set_scale_type(l.transition, OBS_TRANSITION_SCALE_ASPECT);
			obs_transition_set(l.transition, nullptr);
		}
	}
	AttachChannels();
	blog(LOG_INFO, "[tools-scenes] capas en canales %u-%u", kLayerChannelBase,
	     kLayerChannelBase + kLayerCount - 1);
}

void LiveEngine::AttachChannels()
{
	if (!initialized)
		return;
	for (int i = 0; i < kLayerCount; i++) {
		obs_source_t *tr = layers[(size_t)i].transition;
		if (!tr)
			continue;
		OBSSourceAutoRelease current = obs_get_output_source(kLayerChannelBase + (uint32_t)i);
		if (current.Get() != tr)
			obs_set_output_source(kLayerChannelBase + (uint32_t)i, tr);
	}
}

void LiveEngine::Shutdown()
{
	if (!initialized)
		return;
	for (int i = 0; i < kLayerCount; i++) {
		obs_set_output_source(kLayerChannelBase + (uint32_t)i, nullptr);
		Layer &l = layers[(size_t)i];
		if (l.transition)
			obs_transition_clear(l.transition);
		for (auto &s : l.scenes)
			ClearScene(s);
		l.content = nullptr;
		l.transition = nullptr;
		for (auto &s : l.scenes)
			s = nullptr;
		l.visible = false;
	}
	initialized = false;
	listeners.clear();
}

void LiveEngine::CollectionCleanup()
{
	/* Soltar todo lo que pertenece a la colección que se cierra. */
	for (int i = 0; i < kLayerCount; i++) {
		Layer &l = layers[(size_t)i];
		if (l.transition) {
			obs_transition_clear(l.transition);
			obs_transition_set(l.transition, nullptr);
		}
		for (auto &s : l.scenes)
			ClearScene(s);
		l.content = nullptr;
		l.visible = false;
	}
	managedUuids = {};
	libraryUuid.clear();
	Notify();
}

void LiveEngine::CanvasMaybeChanged()
{
	if (!initialized)
		return;
	uint32_t w, h;
	GetCanvas(w, h);
	if (w == canvasW && h == canvasH)
		return;
	canvasW = w;
	canvasH = h;
	ApplyCanvasSize();
}

void LiveEngine::ApplyCanvasSize()
{
	for (int i = 0; i < kLayerCount; i++) {
		Layer &l = layers[(size_t)i];
		if (l.transition)
			obs_transition_set_size(l.transition, canvasW, canvasH);
		obs_scene_t *scene = l.scenes[(size_t)l.activeScene];
		if (l.content && scene)
			PlaceInScene(scene, l.content, l.transform);
	}
}

void LiveEngine::AddListener(void *owner, std::function<void()> fn)
{
	listeners.emplace_back(owner, std::move(fn));
}

void LiveEngine::RemoveListener(void *owner)
{
	listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
				       [owner](const auto &p) { return p.first == owner; }),
			listeners.end());
}

void LiveEngine::Notify()
{
	auto copy = listeners;
	for (auto &l : copy)
		l.second();
}

/* ------------------------------------------------------------------------ */
/* Utilidades de escena                                                      */

void LiveEngine::ClearScene(obs_scene_t *scene)
{
	if (!scene)
		return;
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_sceneitem_addref(item);
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			return true;
		},
		&items);
	for (obs_sceneitem_t *item : items) {
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
	}
}

void ItemTransform::Save(obs_data_t *data) const
{
	obs_data_set_bool(data, "custom", custom);
	obs_data_set_double(data, "x", x);
	obs_data_set_double(data, "y", y);
	obs_data_set_double(data, "w", w);
	obs_data_set_double(data, "h", h);
	obs_data_set_double(data, "rot", rot);
	obs_data_set_int(data, "crop_l", cropL);
	obs_data_set_int(data, "crop_t", cropT);
	obs_data_set_int(data, "crop_r", cropR);
	obs_data_set_int(data, "crop_b", cropB);
	obs_data_set_bool(data, "flip_h", flipH);
	obs_data_set_bool(data, "flip_v", flipV);
}

ItemTransform ItemTransform::Load(obs_data_t *data)
{
	ItemTransform t;
	if (!data)
		return t;
	obs_data_set_default_bool(data, "custom", true);
	t.custom = obs_data_get_bool(data, "custom");
	t.x = (float)obs_data_get_double(data, "x");
	t.y = (float)obs_data_get_double(data, "y");
	t.w = (float)obs_data_get_double(data, "w");
	t.h = (float)obs_data_get_double(data, "h");
	t.rot = (float)obs_data_get_double(data, "rot");
	t.cropL = (int)obs_data_get_int(data, "crop_l");
	t.cropT = (int)obs_data_get_int(data, "crop_t");
	t.cropR = (int)obs_data_get_int(data, "crop_r");
	t.cropB = (int)obs_data_get_int(data, "crop_b");
	t.flipH = obs_data_get_bool(data, "flip_h");
	t.flipV = obs_data_get_bool(data, "flip_v");
	return t;
}

ItemTransform ItemTransform::Effective(uint32_t sw, uint32_t sh, uint32_t cw, uint32_t ch) const
{
	if (custom && w > 0.0f && h > 0.0f)
		return *this;
	ItemTransform t = *this;
	const double vw = std::max(1.0, (double)sw - cropL - cropR);
	const double vh = std::max(1.0, (double)sh - cropT - cropB);
	if (!sw || !sh) {
		t.x = 0;
		t.y = 0;
		t.w = (float)cw;
		t.h = (float)ch;
		return t;
	}
	const double s = std::min(cw / vw, ch / vh);
	t.w = (float)(vw * s);
	t.h = (float)(vh * s);
	t.x = (float)((cw - t.w) / 2.0);
	t.y = (float)((ch - t.h) / 2.0);
	return t;
}

void LiveEngine::ApplyTransform(obs_sceneitem_t *item, const ItemTransform &t)
{
	uint32_t cw, ch;
	GetCanvas(cw, ch);
	struct vec2 pos, bounds, scale;
	/* El signo de la escala voltea (OBS lo respeta con caja de límites). */
	vec2_set(&scale, t.flipH ? -1.0f : 1.0f, t.flipV ? -1.0f : 1.0f);
	struct obs_sceneitem_crop crop = {t.cropL, t.cropT, t.cropR, t.cropB};

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_scale(item, &scale);
	obs_sceneitem_set_crop(item, &crop);
	/* Alineación al centro: la rotación es alrededor del centro de la caja. */
	obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	if (t.custom && t.w > 0.0f && t.h > 0.0f) {
		vec2_set(&pos, t.x + t.w / 2.0f, t.y + t.h / 2.0f);
		vec2_set(&bounds, t.w, t.h);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
	} else {
		vec2_set(&pos, cw / 2.0f, ch / 2.0f);
		vec2_set(&bounds, (float)cw, (float)ch);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	}
	obs_sceneitem_set_rot(item, t.rot);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_defer_update_end(item);
}

void LiveEngine::PlaceInScene(obs_scene_t *scene, obs_source_t *source, const ItemTransform &t)
{
	if (!scene)
		return;
	ClearScene(scene);
	if (!source)
		return;
	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (!item)
		return;
	ApplyTransform(item, t);
	obs_sceneitem_set_visible(item, true);
}

static void ForEachItemOf(obs_scene_t *scene, obs_source_t *source, const std::function<void(obs_sceneitem_t *)> &fn)
{
	if (!scene)
		return;
	struct Ctx {
		obs_source_t *source;
		std::vector<obs_sceneitem_t *> items;
	} ctx{source, {}};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Ctx *>(param);
			if (obs_sceneitem_get_source(item) == c->source) {
				obs_sceneitem_addref(item);
				c->items.push_back(item);
			}
			return true;
		},
		&ctx);
	for (obs_sceneitem_t *item : ctx.items) {
		fn(item);
		obs_sceneitem_release(item);
	}
}

obs_source_t *LiveEngine::ManagedContent(obs_source_t *sceneSource) const
{
	if (!IsManagedScene(sceneSource))
		return nullptr;
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_source_t *found = nullptr;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			*static_cast<obs_source_t **>(param) = obs_sceneitem_get_source(item);
			return false;
		},
		&found);
	return found;
}

void LiveEngine::UpdateTransform(obs_source_t *source, const ItemTransform &t)
{
	if (!source)
		return;
	obs_scene_t *managed[kManagedCount] = {ManagedScene(0, false), ManagedScene(1, false), ManagedScene(2, false)};
	for (obs_scene_t *scene : managed)
		ForEachItemOf(scene, source, [&](obs_sceneitem_t *item) { ApplyTransform(item, t); });

	/* Modo Estudio con "Duplicar escena": Programa muestra una COPIA privada
	 * de la escena gestionada; también se actualiza para verlo en vivo. */
	OBSSourceAutoRelease transition = obs_get_output_source(0);
	if (transition && obs_source_get_type(transition) == OBS_SOURCE_TYPE_TRANSITION) {
		OBSSourceAutoRelease active = obs_transition_get_active_source(transition);
		obs_scene_t *activeScene = active ? obs_scene_from_source(active) : nullptr;
		if (activeScene && activeScene != managed[0] && activeScene != managed[1] && activeScene != managed[2] &&
		    IsManagedScene(active))
			ForEachItemOf(activeScene, source, [&](obs_sceneitem_t *item) { ApplyTransform(item, t); });
	}
	for (Layer &l : layers) {
		if (l.content.Get() != source)
			continue;
		l.transform = t;
		for (auto &s : l.scenes)
			ForEachItemOf(s, source, [&](obs_sceneitem_t *item) { ApplyTransform(item, t); });
	}
}

/* ------------------------------------------------------------------------ */
/* Envío al aire con escenas gestionadas A/B                                 */

obs_scene_t *LiveEngine::ManagedScene(int index, bool create)
{
	obs_source_t *src = nullptr;
	if (!managedUuids[(size_t)index].empty())
		src = obs_get_source_by_uuid(managedUuids[(size_t)index].c_str());
	if (!src)
		src = obs_get_source_by_name(kManagedNames[index]);
	if (src && !obs_scene_from_source(src)) {
		obs_source_release(src);
		src = nullptr;
	}
	if (!src && create) {
		/* Escena pública: OBS la agrega a su lista y la guarda en la colección. */
		obs_scene_t *scene = obs_scene_create(kManagedNames[index]);
		if (!scene)
			return nullptr;
		src = obs_source_get_ref(obs_scene_get_source(scene));
		obs_scene_release(scene);
	}
	if (!src)
		return nullptr;
	const char *uuid = obs_source_get_uuid(src);
	managedUuids[(size_t)index] = uuid ? uuid : "";
	obs_scene_t *scene = obs_scene_from_source(src);
	/* La escena sigue viva en la lista de OBS; devolvemos un puntero prestado. */
	obs_source_release(src);
	return scene;
}

obs_scene_t *LiveEngine::LibraryScene(bool create)
{
	obs_source_t *src = nullptr;
	if (!libraryUuid.empty())
		src = obs_get_source_by_uuid(libraryUuid.c_str());
	if (!src)
		src = obs_get_source_by_name(kLibraryName);
	if (src && !obs_scene_from_source(src)) {
		obs_source_release(src);
		src = nullptr;
	}
	if (!src && create) {
		obs_scene_t *scene = obs_scene_create(kLibraryName);
		if (!scene)
			return nullptr;
		src = obs_source_get_ref(obs_scene_get_source(scene));
		obs_scene_release(scene);
	}
	if (!src)
		return nullptr;
	const char *uuid = obs_source_get_uuid(src);
	libraryUuid = uuid ? uuid : "";
	obs_scene_t *scene = obs_scene_from_source(src);
	obs_source_release(src); /* la mantiene viva la lista de escenas de OBS */
	return scene;
}

bool LiveEngine::IsLibraryScene(obs_source_t *source)
{
	const char *name = source ? obs_source_get_name(source) : nullptr;
	return name && strcmp(name, kLibraryName) == 0 && obs_scene_from_source(source);
}

void LiveEngine::HoldInLibrary(obs_source_t *source)
{
	if (!source || obs_scene_from_source(source))
		return;
	obs_scene_t *library = LibraryScene(true);
	if (!library || obs_scene_find_source(library, obs_source_get_name(source)))
		return;
	obs_sceneitem_t *item = obs_scene_add(library, source);
	if (item) {
		/* Oculto: la biblioteca nunca se muestra; solo conserva la fuente. */
		obs_sceneitem_set_visible(item, false);
	}
}

void LiveEngine::ReleaseFromLibrary(obs_source_t *source)
{
	obs_scene_t *library = source ? LibraryScene(false) : nullptr;
	if (!library)
		return;
	ForEachItemOf(library, source, [](obs_sceneitem_t *item) { obs_sceneitem_remove(item); });
}

static bool SceneHasItems(obs_scene_t *scene)
{
	bool any = false;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *, void *param) {
			*static_cast<bool *>(param) = true;
			return false;
		},
		&any);
	return any;
}

void LiveEngine::ClearProgram()
{
	obs_scene_t *target = PickTargetScene(false);
	if (!target)
		return;
	bool targetHoldsPreview = false;
	if (obs_frontend_preview_program_mode_active()) {
		OBSSourceAutoRelease preview = obs_frontend_get_current_preview_scene();
		targetHoldsPreview = preview.Get() == obs_scene_get_source(target) && SceneHasItems(target);
	}
	if (!targetHoldsPreview) {
		/* Transición normal hacia una escena gestionada vacía. */
		ClearScene(target);
		obs_frontend_set_current_scene(obs_scene_get_source(target));
	} else {
		/* La otra escena tiene la Vista previa: retirar de Programa directamente
		 * (también de la copia que usa "Duplicar escena"). */
		OBSSourceAutoRelease program = obs_frontend_get_current_scene();
		if (IsManagedScene(program))
			ClearScene(obs_scene_from_source(program));
		OBSSourceAutoRelease transition = obs_get_output_source(0);
		if (transition && obs_source_get_type(transition) == OBS_SOURCE_TYPE_TRANSITION) {
			OBSSourceAutoRelease active = obs_transition_get_active_source(transition);
			if (active && IsManagedScene(active) && active.Get() != program.Get())
				ClearScene(obs_scene_from_source(active));
		}
	}
	Notify();
}

void LiveEngine::ClearPreview()
{
	if (!obs_frontend_preview_program_mode_active())
		return;
	obs_scene_t *target = PickTargetScene(true);
	if (!target)
		return;
	ClearScene(target);
	obs_frontend_set_current_preview_scene(obs_scene_get_source(target));
	Notify();
}

bool LiveEngine::IsManagedScene(obs_source_t *source) const
{
	if (!source)
		return false;
	const char *uuid = obs_source_get_uuid(source);
	for (const auto &u : managedUuids)
		if (uuid && !u.empty() && u == uuid)
			return true;
	const char *name = obs_source_get_name(source);
	if (!name)
		return false;
	for (const char *n : kManagedNames)
		if (strcmp(name, n) == 0)
			return true;
	return false;
}

obs_scene_t *LiveEngine::PickTargetScene(bool forPreview)
{
	OBSSourceAutoRelease program = obs_frontend_get_current_scene();
	OBSSourceAutoRelease preview =
		obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene() : nullptr;
	/* La C solo se crea si hace falta (Programa y Vista previa ocupan A y B). */
	obs_scene_t *m[kManagedCount] = {ManagedScene(0, true), ManagedScene(1, true), ManagedScene(2, false)};
	auto src = [&](int i) { return m[i] ? obs_scene_get_source(m[i]) : nullptr; };

	if (forPreview) {
		/* Reutilizar la escena que ya está en Vista previa si es nuestra. */
		for (int i = 0; i < kManagedCount; i++)
			if (m[i] && src(i) == preview.Get() && src(i) != program.Get())
				return m[i];
		for (int i = 0; i < kManagedCount; i++)
			if (m[i] && src(i) != program.Get())
				return m[i];
		return nullptr;
	}
	/* Al aire: nunca la de Programa ni la de Vista previa. */
	for (int i = 0; i < kManagedCount; i++)
		if (m[i] && src(i) != program.Get() && src(i) != preview.Get())
			return m[i];
	m[2] = ManagedScene(2, true);
	if (m[2] && src(2) != program.Get() && src(2) != preview.Get())
		return m[2];
	for (int i = 0; i < kManagedCount; i++)
		if (m[i] && src(i) != program.Get())
			return m[i];
	return nullptr;
}

bool LiveEngine::SendToPreview(obs_source_t *source, const ItemTransform &t)
{
	if (!source)
		return false;
	if (!obs_frontend_preview_program_mode_active())
		return SendToProgram(source, t);
	obs_scene_t *target = PickTargetScene(true);
	if (!target)
		return false;
	PlaceInScene(target, source, t);
	obs_frontend_set_current_preview_scene(obs_scene_get_source(target));
	Notify();
	return true;
}

bool LiveEngine::SendToProgram(obs_source_t *source, const ItemTransform &t)
{
	if (!source)
		return false;
	obs_scene_t *target = PickTargetScene(false);
	if (!target)
		return false;
	PlaceInScene(target, source, t);
	/* En Modo Estudio hace la transición actual; sin él, cambia la escena. */
	obs_frontend_set_current_scene(obs_scene_get_source(target));
	Notify();
	return true;
}

/* ------------------------------------------------------------------------ */
/* Capas                                                                     */

void LiveEngine::LoadLayer(int index, obs_source_t *source, const ItemTransform &t)
{
	if (index < 0 || index >= kLayerCount)
		return;
	EnsureInit();
	Layer &l = layers[(size_t)index];
	if (!l.transition)
		return;
	l.transform = t;

	if (!l.visible) {
		/* Capa apagada: preparar en la escena activa sin mostrar. */
		l.content = source ? obs_source_get_ref(source) : nullptr;
		PlaceInScene(l.scenes[(size_t)l.activeScene], l.content, l.transform);
	} else {
		/* Capa al aire: encadenar con fundido hacia la otra escena. */
		const int next = 1 - l.activeScene;
		l.content = source ? obs_source_get_ref(source) : nullptr;
		PlaceInScene(l.scenes[(size_t)next], l.content, l.transform);
		obs_transition_start(l.transition, OBS_TRANSITION_MODE_AUTO, (uint32_t)transitionMs,
				     l.content ? obs_scene_get_source(l.scenes[(size_t)next]) : nullptr);
		l.activeScene = next;
		if (!l.content)
			l.visible = false;
	}
	Notify();
}

void LiveEngine::SetLayerVisible(int index, bool visible)
{
	if (index < 0 || index >= kLayerCount)
		return;
	EnsureInit();
	Layer &l = layers[(size_t)index];
	if (!l.transition || l.visible == visible)
		return;
	if (visible && !l.content)
		return;
	l.visible = visible;
	obs_source_t *dest = visible ? obs_scene_get_source(l.scenes[(size_t)l.activeScene]) : nullptr;
	obs_transition_start(l.transition, OBS_TRANSITION_MODE_AUTO, (uint32_t)transitionMs, dest);
	Notify();
}

bool LiveEngine::LayerVisible(int index) const
{
	return index >= 0 && index < kLayerCount && layers[(size_t)index].visible;
}

void LiveEngine::ClearLayer(int index)
{
	if (index < 0 || index >= kLayerCount)
		return;
	Layer &l = layers[(size_t)index];
	if (l.visible && l.transition) {
		obs_transition_start(l.transition, OBS_TRANSITION_MODE_AUTO, (uint32_t)transitionMs, nullptr);
		l.visible = false;
		/* La escena se vacía en la siguiente carga; el fundido sigue usando
		 * la escena activa mientras dura. */
		l.activeScene = 1 - l.activeScene;
		ClearScene(l.scenes[(size_t)l.activeScene]);
	} else {
		for (auto &s : l.scenes)
			ClearScene(s);
	}
	l.content = nullptr;
	Notify();
}

obs_source_t *LiveEngine::LayerContent(int index) const
{
	return (index >= 0 && index < kLayerCount) ? layers[(size_t)index].content.Get() : nullptr;
}

obs_source_t *LiveEngine::LayerOutputSource(int index) const
{
	return (index >= 0 && index < kLayerCount) ? layers[(size_t)index].transition.Get() : nullptr;
}

QString LiveEngine::LayerContentName(int index) const
{
	obs_source_t *c = LayerContent(index);
	return c ? QString::fromUtf8(obs_source_get_name(c)) : QString();
}

void LiveEngine::SetTransitionMs(int ms)
{
	transitionMs = std::clamp(ms, 0, 10000);
}

/* ------------------------------------------------------------------------ */
/* Persistencia                                                              */

void LiveEngine::Save(obs_data_t *data) const
{
	obs_data_set_int(data, "layer_transition_ms", transitionMs);
	obs_data_set_string(data, "managed_a", managedUuids[0].c_str());
	obs_data_set_string(data, "managed_b", managedUuids[1].c_str());
	obs_data_set_string(data, "managed_c", managedUuids[2].c_str());
	obs_data_set_string(data, "library", libraryUuid.c_str());
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < kLayerCount; i++) {
		const Layer &l = layers[(size_t)i];
		obs_data_t *item = obs_data_create();
		const char *uuid = l.content ? obs_source_get_uuid(l.content) : nullptr;
		obs_data_set_string(item, "content", uuid ? uuid : "");
		OBSDataAutoRelease tr = obs_data_create();
		l.transform.Save(tr);
		obs_data_set_obj(item, "transform", tr);
		obs_data_set_bool(item, "visible", l.visible);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "layers", arr);
	obs_data_array_release(arr);
}

void LiveEngine::Load(obs_data_t *data)
{
	EnsureInit();
	CollectionCleanup();
	AttachChannels();
	if (!data)
		return;
	obs_data_set_default_int(data, "layer_transition_ms", 500);
	transitionMs = (int)obs_data_get_int(data, "layer_transition_ms");
	managedUuids[0] = obs_data_get_string(data, "managed_a");
	managedUuids[1] = obs_data_get_string(data, "managed_b");
	managedUuids[2] = obs_data_get_string(data, "managed_c");
	libraryUuid = obs_data_get_string(data, "library");

	obs_data_array_t *arr = obs_data_get_array(data, "layers");
	const size_t n = std::min<size_t>(obs_data_array_count(arr), kLayerCount);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		OBSDataAutoRelease tr = obs_data_get_obj(item, "transform");
		const ItemTransform t = tr ? ItemTransform::Load(tr) : ItemTransform();
		const char *uuid = obs_data_get_string(item, "content");
		if (uuid && *uuid) {
			OBSSourceAutoRelease src = obs_get_source_by_uuid(uuid);
			if (src) {
				LoadLayer((int)i, src, t);
				if (obs_data_get_bool(item, "visible"))
					SetLayerVisible((int)i, true);
			}
		}
		obs_data_release(item);
	}
	obs_data_array_release(arr);
	Notify();
}

/* ======================================================================== */
/* CleanOutputs                                                              */

CleanOutputs &CleanOutputs::Get()
{
	static CleanOutputs outputs;
	return outputs;
}

void CleanOutputs::EnsureView(Target &t)
{
	if (!t.view) {
		t.view = obs_view_create();
		t.mainPtr = nullptr;
	}
	SyncTarget(t);
}

void CleanOutputs::SyncTarget(Target &t)
{
	if (!t.view)
		return;
	/* Canal 0 = transición principal de OBS (la salida de Programa sin capas). */
	OBSSourceAutoRelease main = obs_get_output_source(0);
	if (main.Get() != t.mainPtr) {
		obs_view_set_source(t.view, 0, main);
		t.mainPtr = main.Get();
	}
	LiveEngine &engine = LiveEngine::Get();
	for (int i = 0; i < kLayerCount; i++) {
		obs_source_t *want = t.layers[(size_t)i] ? engine.LayerOutputSource(i) : nullptr;
		OBSSourceAutoRelease have = obs_view_get_source(t.view, kLayerChannelBase + (uint32_t)i);
		if (have.Get() != want)
			obs_view_set_source(t.view, kLayerChannelBase + (uint32_t)i, want);
	}
}

void CleanOutputs::ReleaseView(Target &t)
{
	if (!t.view)
		return;
	for (uint32_t ch = 0; ch < MAX_CHANNELS; ch++)
		obs_view_set_source(t.view, ch, nullptr);
	obs_view_destroy(t.view);
	t.view = nullptr;
	t.mainPtr = nullptr;
}

void CleanOutputs::Sync()
{
	if (monitor.view)
		SyncTarget(monitor);
	if (ndi.view)
		SyncTarget(ndi);
	if (projector) {
		uint32_t w, h;
		GetCanvas(w, h);
		projector->SetCanvasSize(w, h);
	}
}

void CleanOutputs::CollectionCleanup()
{
	/* Soltar la transición de la colección anterior; Sync() asigna la nueva. */
	for (Target *t : {&monitor, &ndi}) {
		if (!t->view)
			continue;
		obs_view_set_source(t->view, 0, nullptr);
		t->mainPtr = nullptr;
	}
}

void CleanOutputs::Shutdown()
{
	StopNdi();
	HideMonitor();
	ReleaseView(monitor);
	ReleaseView(ndi);
	onMonitorClosed = nullptr;
}

/* ---- Monitor ---- */

bool CleanOutputs::ShowMonitor(QScreen *screen)
{
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	if (!screen)
		return false;

	EnsureView(monitor);
	/* Una ventana nativa ya creada no se lleva de forma fiable a otra pantalla
	 * (en Windows queda a pantalla completa donde se creó): para cambiar de
	 * monitor se crea una ventana nueva directamente en el elegido. */
	CloseProjectorWindow();
	projector = new ProjectorWindow(monitor.view);
	projector->onClosedByUser = [this]() {
		HideMonitor();
		if (onMonitorClosed)
			onMonitorClosed();
	};
	uint32_t w, h;
	GetCanvas(w, h);
	projector->SetCanvasSize(w, h);
	projector->ShowOn(screen);
	return true;
}

void CleanOutputs::CloseProjectorWindow()
{
	if (!projector)
		return;
	projector->onClosedByUser = nullptr;
	projector->DestroyDisplay();
	projector->hide();
	projector->deleteLater();
	projector = nullptr;
}

void CleanOutputs::HideMonitor()
{
	if (!projector)
		return;
	CloseProjectorWindow();
	/* Sin monitor la vista no se renderiza; se suelta para no mantener
	 * referencias innecesarias. */
	ReleaseView(monitor);
}

bool CleanOutputs::MonitorActive() const
{
	return projector != nullptr;
}

void CleanOutputs::SetMonitorLayer(int index, bool on)
{
	monitor.layers[(size_t)index] = on;
	SyncTarget(monitor);
}

/* ---- NDI ---- */

bool CleanOutputs::NdiAvailable()
{
	return obs_output_get_display_name("ndi_output") != nullptr;
}

bool CleanOutputs::StartNdi(const QString &name, QString &error)
{
	if (ndiOutput)
		return true;
	if (!NdiAvailable()) {
		error = QString::fromUtf8(obs_module_text("Ndi.Missing"));
		return false;
	}
	EnsureView(ndi);

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		error = "obs_get_video_info";
		return false;
	}
	video_t *video = obs_view_add2(ndi.view, &ovi);
	if (!video) {
		error = QString::fromUtf8(obs_module_text("Ndi.ViewFailed"));
		return false;
	}
	ndiVideoAdded = true;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "ndi_name", name.toUtf8().constData());
	obs_data_set_string(settings, "ndi_groups", "");
	obs_data_set_bool(settings, "uses_video", true);
	obs_data_set_bool(settings, "uses_audio", true);
	ndiOutput = obs_output_create("ndi_output", "Tools Scenes Salida Limpia", settings, nullptr);
	obs_data_release(settings);
	if (!ndiOutput) {
		error = QString::fromUtf8(obs_module_text("Ndi.CreateFailed"));
		StopNdi();
		return false;
	}
	obs_output_set_media(ndiOutput, video, obs_get_audio());
	if (!obs_output_start(ndiOutput)) {
		const char *e = obs_output_get_last_error(ndiOutput);
		error = e ? QString::fromUtf8(e) : QString::fromUtf8(obs_module_text("Ndi.StartFailed"));
		StopNdi();
		return false;
	}
	blog(LOG_INFO, "[tools-scenes] salida NDI limpia iniciada: %s", name.toUtf8().constData());
	return true;
}

void CleanOutputs::StopNdi()
{
	if (ndiOutput) {
		obs_output_force_stop(ndiOutput);
		obs_output_release(ndiOutput);
		ndiOutput = nullptr;
		blog(LOG_INFO, "[tools-scenes] salida NDI limpia detenida");
	}
	if (ndiVideoAdded && ndi.view) {
		obs_view_remove(ndi.view);
		ndiVideoAdded = false;
	}
	ReleaseView(ndi);
}

bool CleanOutputs::NdiActive() const
{
	return ndiOutput != nullptr;
}

void CleanOutputs::SetNdiLayer(int index, bool on)
{
	ndi.layers[(size_t)index] = on;
	SyncTarget(ndi);
}
