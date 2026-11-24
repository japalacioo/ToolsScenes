/*
 * Tools Scenes - vista previa de escenas, multimedia, capas y salida limpia
 * al estilo vMix para OBS Studio.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include <string>

#include "layer-strip.hpp"
#include "live-engine.hpp"
#include "media-dock.hpp"
#include "output-dock.hpp"
#include "wall-dock.hpp"

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "1.0.0"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("tools-scenes", "es-ES")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Tools Scenes";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

namespace {

QPointer<QTimer> g_syncTimer;
bool g_exiting = false;

/* Ciclo de vida del motor en vivo y de las salidas limpias. */
void GlobalFrontendEvent(enum obs_frontend_event event, void *)
{
	if (g_exiting)
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		LiveEngine::Get().EnsureInit();
		CleanOutputs::Get().Sync();
		QTimer::singleShot(0, []() { LayerStrip::InjectIfStudioMode(); });
		if (g_syncTimer)
			g_syncTimer->start();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		/* (CHANGING llega antes del guardado: no se limpia allí.) */
		CleanOutputs::Get().CollectionCleanup();
		LiveEngine::Get().CollectionCleanup();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		/* OBS desconecta todos los canales al limpiar la colección. */
		LiveEngine::Get().AttachChannels();
		CleanOutputs::Get().Sync();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
		/* OBS recrea la columna central cada vez que se activa el Modo Estudio. */
		QTimer::singleShot(0, []() { LayerStrip::InjectIfStudioMode(); });
		CleanOutputs::Get().Sync();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		CleanOutputs::Get().Sync();
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		g_exiting = true;
		if (g_syncTimer)
			g_syncTimer->stop();
		CleanOutputs::Get().Shutdown();
		LiveEngine::Get().Shutdown();
		break;
	default:
		break;
	}
}

/* La versión anterior se llamaba ScenePrevio ("scene-previo"): copiar sus
 * ajustes (tamaño, actualización, salidas) a la carpeta nueva una sola vez. */
void MigrateOldConfig()
{
	char *dir = obs_module_config_path("");
	if (!dir)
		return;
	const std::string newDir = dir;
	bfree(dir);
	const std::string key = "tools-scenes";
	const size_t pos = newDir.rfind(key);
	if (pos == std::string::npos)
		return;
	std::string oldDir = newDir;
	oldDir.replace(pos, key.size(), "scene-previo");
	for (const char *file : {"settings.json", "outputs.json"}) {
		const std::string from = oldDir + "/" + file;
		const std::string to = newDir + "/" + file;
		if (!os_file_exists(to.c_str()) && os_file_exists(from.c_str())) {
			os_mkdirs(newDir.c_str());
			if (os_copyfile(from.c_str(), to.c_str()) == 0)
				blog(LOG_INFO, "[tools-scenes] ajustes migrados: %s", file);
		}
	}
}

template<typename T> bool AddDock(QMainWindow *main, const char *id, const char *titleKey)
{
	auto *dock = new T(main);
	if (!obs_frontend_add_dock_by_id(id, obs_module_text(titleKey), dock)) {
		blog(LOG_WARNING, "[tools-scenes] no se pudo registrar el panel %s", id);
		delete dock;
		return false;
	}
#ifdef SCENE_PREVIO_DEBUG
	QTimer::singleShot(1500, dock, [dock]() {
		if (QWidget *w = dock->parentWidget())
			w->show();
	});
#endif
	return true;
}

} // namespace

bool obs_module_load(void)
{
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		blog(LOG_WARNING, "[tools-scenes] ventana principal no disponible; los paneles no se crearán");
		return true;
	}

	MigrateOldConfig();
	obs_frontend_add_event_callback(GlobalFrontendEvent, nullptr);

	/* Revisión periódica barata: cambios de lienzo y de transición principal. */
	g_syncTimer = new QTimer(mainWindow);
	g_syncTimer->setInterval(1000);
	QObject::connect(g_syncTimer, &QTimer::timeout, []() {
		if (g_exiting)
			return;
		LiveEngine::Get().CanvasMaybeChanged();
		LiveEngine::Get().AttachChannels();
		CleanOutputs::Get().Sync();
	});

	AddDock<WallDock>(mainWindow, "scene-previo-dock", "DockTitle");
	AddDock<MediaDock>(mainWindow, "scene-previo-media", "MediaDockTitle");
	AddDock<OutputDock>(mainWindow, "scene-previo-output", "OutputDockTitle");

	blog(LOG_INFO, "[tools-scenes] version %s cargada", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(GlobalFrontendEvent, nullptr);
	blog(LOG_INFO, "[tools-scenes] descargado");
}
