/*
 * Tools Scenes - vista previa de escenas tipo vMix para OBS Studio
 *
 * WallDock: panel acoplable. Mantiene la lista de escenas, grupos (pestañas),
 * diseño de la cuadrícula y la interacción con el usuario. Todo en el hilo UI.
 */
#pragma once

#include "wall-canvas.hpp"

#include <obs-frontend-api.h>

#include <QColor>
#include <QRect>
#include <QString>
#include <QWidget>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class QComboBox;
class QHBoxLayout;
class QPushButton;
class QScrollBar;
class QSlider;
class QTimer;
class QToolButton;

class WallDock : public QWidget {
public:
	explicit WallDock(QWidget *parent);
	~WallDock() override;

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	struct SceneEntry {
		std::shared_ptr<WallSourceRef> ref;
		std::string uuid;
		QString name;
	};
	struct Group {
		QString id;
		QString name;
		QColor color;
	};
	struct LayoutTile {
		size_t sceneIndex = 0;
		QRect rect; /* coordenadas lógicas del contenido (sin scroll) */
	};
	struct LabelCacheEntry {
		QString name;
		int width = 0;
		int height = 0;
		qreal dpr = 1.0;
		std::shared_ptr<const WallLabelImage> image;
	};

	static void FrontendEvent(enum obs_frontend_event event, void *data);
	static void FrontendSave(obs_data_t *saveData, bool saving, void *data);
	static void SourceRenamed(void *data, calldata_t *cd);

	void Shutdown();
	void ReloadScenes();
	void ReleaseScenes();
	void RebuildTabs();
	void ScheduleRelayout();
	void Relayout();
	void UpdateActive();
	void UpdateShowing();
	void UpdateScrollBar();
	int HitTest(const QPoint &pos) const;
	void ShowTileMenu(int layoutIndex, const QPoint &globalPos);
	void ShowSettingsMenu(const QPoint &globalPos);
	void AutoSize();
	void SetCurrentTab(const QString &groupId);

	bool CreateGroupDialog(QString &outId);
	void RenameGroup(const QString &id);
	void RecolorGroup(const QString &id);
	void DeleteGroup(const QString &id);
	const Group *FindGroup(const QString &id) const;
	void CollectionDataChanged();

	void LoadGlobalSettings();
	void SaveGlobalSettings() const;
	void LoadCollectionData(obs_data_t *data);
	bool ParseGroupsData(obs_data_t *data); /* true si trajo grupos */
	void ResolvePendingAssignments();
	/* Copia de los grupos en el perfil de OBS (respaldo y por perfil). */
	static QString ProfileFilePath();
	void WriteProfileData() const;
	bool ReadProfileData();
	obs_data_t *SaveCollectionData() const;

	std::vector<size_t> ScenesInCurrentTab() const;
	WallColor HeaderColorFor(const std::string &uuid) const;
	std::shared_ptr<const WallLabelImage> LabelFor(const SceneEntry &scene, int width, int height, qreal dpr);
	int ComputeContentHeight(int tileWidth, int viewWidth, size_t count) const;
	int HeaderHeight() const;

	/* Widgets */
	QWidget *tabBar = nullptr;
	QHBoxLayout *tabLayout = nullptr;
	std::vector<QPushButton *> tabButtons;
	QSlider *sizeSlider = nullptr;
	QPushButton *autoButton = nullptr;
	QComboBox *fpsCombo = nullptr;
	QToolButton *settingsButton = nullptr;
	WallCanvas *canvas = nullptr;
	QScrollBar *scrollBar = nullptr;
	QTimer *maintenanceTimer = nullptr;
	QTimer *relayoutTimer = nullptr;

	/* Estado */
	bool loading = true; /* hasta FINISHED_LOADING / cambio de colección */
	bool shuttingDown = false;
	bool callbacksRegistered = false;
	bool dockHooked = false;
	std::vector<SceneEntry> scenes;
	std::vector<Group> groups;
	std::map<std::string, QString> assignments; /* uuid escena -> id grupo */
	std::map<std::string, QString> assignmentNames; /* uuid escena -> nombre (para recuperar por nombre) */
	std::map<QString, QString> pendingByName;      /* nombre escena -> id grupo, aún sin resolver */
	QString currentTab;                           /* vacío = "Todas" */
	std::vector<LayoutTile> layout;
	std::unordered_map<obs_source_t *, std::shared_ptr<WallSourceRef>> showing;
	std::unordered_map<std::string, LabelCacheEntry> labelCache;
	uint32_t lastBaseWidth = 0;
	uint32_t lastBaseHeight = 0;
	int contentHeight = 0;

	/* Preferencias globales */
	int tileWidth = 220;
	int refreshFps = 10;
	bool keepLive = true;
	bool doubleClickToProgram = true;
};
