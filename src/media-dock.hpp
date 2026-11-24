/*
 * Tools Scenes - panel "Multimedia y capas", al estilo de las entradas de vMix.
 *
 *  - Biblioteca en tarjetas: carpetas, archivos (imagen, video, audio, PDF,
 *    HTML) y entradas (NDI, audio, cámara, navegador, fuentes existentes).
 *  - Cada tarjeta: miniatura, Vista (Vista previa), Aire (Programa) y botones
 *    de capa 1-4 que se iluminan cuando el elemento está en esa capa.
 *  - Visor/editor en vivo del elemento seleccionado para ajustar posición y
 *    tamaño arrastrando (se aplica en Programa, Vista previa y capas).
 */
#pragma once

#include "live-engine.hpp"

#include <obs-frontend-api.h>

#include <QImage>
#include <QString>
#include <QWidget>

#include <array>
#include <map>
#include <memory>
#include <set>
#include <vector>

class MediaCard;
class QFileSystemWatcher;
class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QVBoxLayout;
class TransformEditor;
class ThumbnailLoader;

class MediaDock : public QWidget {
public:
	explicit MediaDock(QWidget *parent);
	~MediaDock() override;

private:
	static void FrontendEvent(enum obs_frontend_event event, void *data);
	static void FrontendSave(obs_data_t *saveData, bool saving, void *data);

	void Shutdown();
	void RebuildCards();
	void RefreshCardStates();
	void RefreshLayerPanel();
	void SelectKey(const QString &key);
	void UpdateEditor();          /* si el editor está abierto, sigue a la selección */
	void OpenEditor(const QString &key);
	void UpdateControls();

	void AddFolder();
	void AddFiles();
	void ShowAddInputMenu();
	void AddExistingSource();
	void CreateInput(const char *id, const QString &label);
	void RemoveKey(const QString &key);
	void ShowCardMenu(const QString &key, const QPoint &globalPos);

	/* Claves: "f:<ruta>" archivo, "d:<carpeta>" presentación, "i:<uuid>" entrada. */
	obs_source_t *SourceForKey(const QString &key, bool create);   /* referencia nueva */
	obs_source_t *SourceForFile(const QString &path, bool create);
	void FitPdfSource(const QString &path, obs_source_t *src);
	obs_source_t *SourceForFolderSlideshow(const QString &folder, bool create);
	obs_source_t *CreateTempPreviewSource(const QString &key);      /* privada */
	obs_source_t *ControlSource();                                  /* referencia nueva */
	bool BuildFileSource(const QString &path, const char *&id, obs_data_t *settings);
	ItemTransform TransformFor(const QString &key) const;

	void SendToPreview(const QString &key);
	void SendToProgram(const QString &key);
	void ToggleLayer(const QString &key, int layer);
	void LoadToLayer(const QString &key, int layer, bool show);
	void MediaPlayPause();
	void MediaRestart();
	void ToggleLoop();
	void PdfPage(int delta);

	static QString FileType(const QString &path);
	static QStringList SupportedFilters();
	static QString TypeTagForFile(const QString &path);
	static QString TypeTagForSource(obs_source_t *source);

	void Save(obs_data_t *data) const;
	void Load(obs_data_t *data);
#ifdef SCENE_PREVIO_DEBUG
	void SelfTest(const char *dir);
#endif

	/* Widgets */
	TransformEditor *editor = nullptr;
	QLabel *itemTitle = nullptr;
	QPushButton *btnEdit = nullptr;
	QPushButton *btnPlay = nullptr;
	QPushButton *btnRestart = nullptr;
	QPushButton *btnLoop = nullptr;
	QPushButton *btnPrevPage = nullptr;
	QPushButton *btnNextPage = nullptr;
	QSlider *cardSize = nullptr;
	QScrollArea *scroll = nullptr;
	QWidget *cardsHost = nullptr;
	QVBoxLayout *cardsLayout = nullptr;
	struct LayerRow {
		QPushButton *toggle = nullptr;
		QLabel *content = nullptr;
		QPushButton *clear = nullptr;
	};
	std::array<LayerRow, kLayerCount> layerRows;
	QWidget *layersGroup = nullptr; /* oculto en Modo Estudio (las capas van en la columna central) */
	QSpinBox *transitionSpin = nullptr;
	QFileSystemWatcher *watcher = nullptr;
	std::vector<MediaCard *> cards;
	std::unique_ptr<ThumbnailLoader> thumbs;
	std::map<QString, QImage> thumbCache;
	std::set<QString> collapsed;

	/* Selección y vista previa */
	QString selectedKey;
	OBSSourceAutoRelease previewTemp; /* fuente privada del editor mientras no exista la pública */
	QString previewTempKey;

	/* Datos (por colección de escenas) */
	std::vector<QString> folders;
	std::vector<QString> files;
	std::vector<std::string> inputs;            /* UUID de fuentes */
	std::map<QString, std::string> fileSources; /* clave de archivo/carpeta -> UUID creada */
	std::map<QString, ItemTransform> transforms;
	int cardWidth = 180;

	bool callbacksRegistered = false;
	bool shuttingDown = false;
};
