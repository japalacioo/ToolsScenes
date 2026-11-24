/*
 * Tools Scenes - vista previa de escenas tipo vMix para OBS Studio
 * Panel acoplable (ver wall-dock.hpp).
 */
#include "wall-dock.hpp"
#include "flow-layout.hpp"
#include "live-engine.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QAction>
#include <QColorDialog>
#include <QComboBox>
#include <QDockWidget>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_set>

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "1.0.0"
#endif

namespace {

constexpr int kMargin = 6;
constexpr int kGap = 6;
constexpr int kMinTile = 80;
constexpr int kMaxTile = 800;
constexpr const char *kSaveKey = "scene-previo";
const int kFpsValues[] = {0, 30, 15, 10, 5, 2, 1};

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

std::atomic<uint64_t> g_labelIds{1};

WallColor ToWallColor(const QColor &c)
{
	return WallColor{(float)c.redF(), (float)c.greenF(), (float)c.blueF(), 1.0f};
}

QString ButtonStyle(const QColor &bg, bool checked)
{
	const QColor border = checked ? QColor(255, 255, 255) : bg.darker(130);
	return QString("QPushButton{background-color:%1;color:#ffffff;border:2px solid %2;"
		       "border-radius:3px;padding:3px 10px;font-weight:%3;}"
		       "QPushButton:hover{background-color:%4;}")
		.arg(bg.name(), border.name(), checked ? "bold" : "normal", bg.lighter(115).name());
}

std::string SourceUuid(obs_source_t *source)
{
	const char *uuid = obs_source_get_uuid(source);
	return uuid ? std::string(uuid) : std::string();
}

} // namespace

/* ======================================================================== */

WallDock::WallDock(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(200, 150);
	LoadGlobalSettings();

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(4);

	/* Fila 1: pestañas + ajustes */
	auto *row1 = new QHBoxLayout();
	row1->setSpacing(4);
	tabBar = new QWidget(this);
	tabLayout = new QHBoxLayout(tabBar);
	tabLayout->setContentsMargins(0, 0, 0, 0);
	tabLayout->setSpacing(3);
	row1->addWidget(tabBar, 1);

	settingsButton = new QToolButton(this);
	settingsButton->setText(QString::fromUtf8("⚙"));
	settingsButton->setToolTip(T("Settings"));
	settingsButton->setAutoRaise(true);
	row1->addWidget(settingsButton);
	root->addLayout(row1);

	/* Fila 2: tamaño, autoajuste, actualización (fluye en paneles angostos) */
	auto *row2Host = new QWidget(this);
	auto *row2 = new FlowLayout(row2Host, 6);
	row2->setContentsMargins(0, 0, 0, 0);
	auto *sizeBox = new QWidget(row2Host);
	auto *sizeRow = new QHBoxLayout(sizeBox);
	sizeRow->setContentsMargins(0, 0, 0, 0);
	sizeRow->addWidget(new QLabel(T("Size"), sizeBox));
	sizeSlider = new QSlider(Qt::Horizontal, sizeBox);
	sizeSlider->setRange(kMinTile, kMaxTile);
	sizeSlider->setValue(tileWidth);
	sizeSlider->setFixedWidth(120);
	sizeRow->addWidget(sizeSlider);
	row2->addWidget(sizeBox);
	autoButton = new QPushButton(T("AutoSize"), row2Host);
	autoButton->setToolTip(T("AutoSize.Tooltip"));
	row2->addWidget(autoButton);
	auto *fpsBox = new QWidget(row2Host);
	auto *fpsRow = new QHBoxLayout(fpsBox);
	fpsRow->setContentsMargins(0, 0, 0, 0);
	fpsRow->addWidget(new QLabel(T("Refresh"), fpsBox));
	fpsCombo = new QComboBox(fpsBox);
	for (int fps : kFpsValues)
		fpsCombo->addItem(fps == 0 ? T("Refresh.Realtime") : QString("%1 fps").arg(fps), fps);
	fpsCombo->setToolTip(T("Refresh.Tooltip"));
	{
		int idx = fpsCombo->findData(refreshFps);
		fpsCombo->setCurrentIndex(idx >= 0 ? idx : 3);
		refreshFps = fpsCombo->currentData().toInt();
	}
	fpsRow->addWidget(fpsCombo);
	row2->addWidget(fpsBox);
	root->addWidget(row2Host);

	/* Lienzo + barra de desplazamiento */
	auto *row3 = new QHBoxLayout();
	row3->setSpacing(0);
	canvas = new WallCanvas(this);
	canvas->SetRefreshFps(refreshFps);
	row3->addWidget(canvas, 1);
	scrollBar = new QScrollBar(Qt::Vertical, this);
	scrollBar->setRange(0, 0);
	row3->addWidget(scrollBar);
	root->addLayout(row3, 1);

	/* Temporizadores */
	relayoutTimer = new QTimer(this);
	relayoutTimer->setSingleShot(true);
	relayoutTimer->setInterval(0);
	connect(relayoutTimer, &QTimer::timeout, this, [this]() { Relayout(); });

	maintenanceTimer = new QTimer(this);
	maintenanceTimer->setInterval(1000);
	connect(maintenanceTimer, &QTimer::timeout, this, [this]() {
		canvas->PurgeRetired();
		if (loading || shuttingDown)
			return;
		obs_video_info ovi = {};
		if (obs_get_video_info(&ovi) &&
		    (ovi.base_width != lastBaseWidth || ovi.base_height != lastBaseHeight))
			ScheduleRelayout();
	});
	maintenanceTimer->start();

	/* Conexiones de la interfaz */
	connect(sizeSlider, &QSlider::valueChanged, this, [this](int value) {
		tileWidth = value;
		ScheduleRelayout();
	});
	connect(sizeSlider, &QSlider::sliderReleased, this, [this]() { SaveGlobalSettings(); });
	connect(autoButton, &QPushButton::clicked, this, [this]() { AutoSize(); });
	connect(fpsCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
		refreshFps = fpsCombo->currentData().toInt();
		canvas->SetRefreshFps(refreshFps);
		SaveGlobalSettings();
	});
	connect(settingsButton, &QToolButton::clicked, this, [this]() {
		ShowSettingsMenu(settingsButton->mapToGlobal(QPoint(0, settingsButton->height())));
	});
	connect(scrollBar, &QScrollBar::valueChanged, this, [this](int value) {
		canvas->SetScrollY((int)std::lround(value * canvas->devicePixelRatioF()));
	});

	canvas->onResized = [this]() { ScheduleRelayout(); };
	canvas->onWheel = [this](QWheelEvent *event) {
		const int dy = event->angleDelta().y();
		if (event->modifiers() & Qt::ControlModifier) {
			sizeSlider->setValue(sizeSlider->value() + (dy > 0 ? 20 : -20));
			SaveGlobalSettings();
		} else if (!event->pixelDelta().isNull()) {
			scrollBar->setValue(scrollBar->value() - event->pixelDelta().y());
		} else {
			scrollBar->setValue(scrollBar->value() - dy * 60 / 120);
		}
		event->accept();
	};
	canvas->onMouseMove = [this](QMouseEvent *event) {
		const int idx = HitTest(event->position().toPoint());
		canvas->SetHoverIndex(idx);
		if (idx >= 0 && (size_t)idx < layout.size())
			canvas->setToolTip(scenes[layout[(size_t)idx].sceneIndex].name);
		else
			canvas->setToolTip(QString());
	};
	canvas->onMouseLeave = [this]() { canvas->SetHoverIndex(-1); };
	canvas->onMousePress = [this](QMouseEvent *event) {
		const int idx = HitTest(event->position().toPoint());
		if (event->button() == Qt::RightButton) {
			if (idx >= 0)
				ShowTileMenu(idx, event->globalPosition().toPoint());
			else
				ShowSettingsMenu(event->globalPosition().toPoint());
			return;
		}
		if (event->button() != Qt::LeftButton || idx < 0)
			return;
		obs_source_t *src = scenes[layout[(size_t)idx].sceneIndex].ref->get();
		if (obs_frontend_preview_program_mode_active())
			obs_frontend_set_current_preview_scene(src);
		else
			obs_frontend_set_current_scene(src);
	};
	canvas->onMouseDoubleClick = [this](QMouseEvent *event) {
		if (event->button() != Qt::LeftButton || !doubleClickToProgram)
			return;
		const int idx = HitTest(event->position().toPoint());
		if (idx < 0)
			return;
		if (obs_frontend_preview_program_mode_active()) {
			obs_source_t *src = scenes[layout[(size_t)idx].sceneIndex].ref->get();
			obs_frontend_set_current_scene(src); /* transición a Programa */
		}
	};

	/* Callbacks de OBS */
	obs_frontend_add_event_callback(FrontendEvent, this);
	obs_frontend_add_save_callback(FrontendSave, this);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", SourceRenamed, this);
	callbacksRegistered = true;

	RebuildTabs();
}

WallDock::~WallDock()
{
	Shutdown();
}

void WallDock::Shutdown()
{
	if (callbacksRegistered) {
		/* signal_handler_disconnect espera a que termine cualquier llamada en curso. */
		signal_handler_disconnect(obs_get_signal_handler(), "source_rename", SourceRenamed, this);
		obs_frontend_remove_event_callback(FrontendEvent, this);
		obs_frontend_remove_save_callback(FrontendSave, this);
		callbacksRegistered = false;
	}
	shuttingDown = true;
	if (maintenanceTimer)
		maintenanceTimer->stop();
	if (relayoutTimer)
		relayoutTimer->stop();
	ReleaseScenes();
	if (canvas)
		canvas->Shutdown();
}

/* ------------------------------------------------------------------------ */
/* Callbacks de OBS                                                           */

void WallDock::FrontendEvent(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<WallDock *>(data);
	if (self->shuttingDown)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		self->loading = false;
		self->ReloadScenes();
#ifdef SCENE_PREVIO_DEBUG
		blog(LOG_INFO, "[walltest] grupos cargados=%zu asignaciones=%zu", self->groups.size(),
		     self->assignments.size());
		if (getenv("SCENE_PREVIO_WALLTEST") && self->groups.empty()) {
			Group g;
			g.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
			g.name = "Grupo prueba";
			g.color = QColor(30, 110, 40);
			self->groups.push_back(g);
			if (!self->scenes.empty())
				self->assignments[self->scenes.front().uuid] = g.id;
			self->CollectionDataChanged();
			blog(LOG_INFO, "[walltest] grupo creado (escenas=%zu)", self->scenes.size());
		}
		if (getenv("SCENE_PREVIO_PROFILETEST")) {
			/* Cambiar a un perfil temporal y volver (como hace el usuario). */
			QTimer::singleShot(1500, self, [self]() {
				char *orig = obs_frontend_get_current_profile();
				const std::string original = orig ? orig : "";
				bfree(orig);
				blog(LOG_INFO, "[walltest] perfil original '%s' grupos=%zu", original.c_str(), self->groups.size());
				obs_frontend_create_profile("Tools Scenes Perfil Temporal");
				obs_frontend_set_current_profile("Tools Scenes Perfil Temporal");
				blog(LOG_INFO, "[walltest] en perfil temporal: grupos=%zu", self->groups.size());
				QTimer::singleShot(1500, self, [self, original]() {
					obs_frontend_set_current_profile(original.c_str());
					blog(LOG_INFO, "[walltest] de vuelta al perfil: grupos=%zu", self->groups.size());
					obs_frontend_delete_profile("Tools Scenes Perfil Temporal");
				});
			});
		}
#endif
		break;

	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		/* OBS ya guardó la colección (CHANGING llega ANTES del guardado, por
		 * eso no se limpia allí). Soltar todas las referencias. */
		self->loading = true;
		self->ReleaseScenes();
		self->groups.clear();
		self->assignments.clear();
		self->currentTab.clear();
		self->labelCache.clear();
		self->RebuildTabs();
		break;

	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		self->loading = false;
		self->RebuildTabs();
		self->ReloadScenes();
		break;

	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		/* Grupos del perfil nuevo; si no tiene, se copian los actuales. */
		if (self->ReadProfileData()) {
			blog(LOG_INFO, "[tools-scenes] perfil cambiado: %zu grupos del perfil", self->groups.size());
			self->ResolvePendingAssignments();
			self->RebuildTabs();
			self->Relayout();
			self->UpdateShowing();
		} else {
			self->WriteProfileData();
		}
		break;

	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		if (!self->loading)
			self->ReloadScenes();
		break;

	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		if (!self->loading)
			self->UpdateActive();
		break;

	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		self->Shutdown();
		break;

	default:
		break;
	}
}

void WallDock::FrontendSave(obs_data_t *saveData, bool saving, void *data)
{
	auto *self = static_cast<WallDock *>(data);
#ifdef SCENE_PREVIO_DEBUG
	blog(LOG_INFO, "[walltest] save callback saving=%d grupos=%zu", saving, self->groups.size());
#endif
	if (saving) {
		obs_data_t *obj = self->SaveCollectionData();
		obs_data_set_obj(saveData, kSaveKey, obj);
		obs_data_release(obj);
		/* El respaldo del perfil solo se vacía por una acción del usuario
		 * (CollectionDataChanged), nunca por un guardado automático. */
		if (!self->groups.empty())
			self->WriteProfileData();
		blog(LOG_INFO, "[tools-scenes] grupos guardados en la colección: %zu (asignaciones %zu)",
		     self->groups.size(), self->assignments.size());
	} else {
		obs_data_t *obj = obs_data_get_obj(saveData, kSaveKey);
		self->LoadCollectionData(obj);
		obs_data_release(obj);
	}
}

void WallDock::SourceRenamed(void *data, calldata_t *cd)
{
	/* Puede llegar desde cualquier hilo: solo se encola trabajo al hilo UI. */
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_scene_from_source(source))
		return;
	auto *self = static_cast<WallDock *>(data);
	QMetaObject::invokeMethod(
		self,
		[self]() {
			if (!self->loading && !self->shuttingDown)
				self->ReloadScenes();
		},
		Qt::QueuedConnection);
}

/* ------------------------------------------------------------------------ */
/* Escenas                                                                    */

void WallDock::ReleaseScenes()
{
	/* 1) El render deja de usar las fuentes (espera a fin de fotograma). */
	if (canvas)
		canvas->ReleaseAllSync();
	/* 2) Equilibrar inc_showing. */
	for (auto &entry : showing)
		obs_source_dec_showing(entry.first);
	showing.clear();
	/* 3) Liberar referencias. */
	layout.clear();
	scenes.clear();
}

void WallDock::ReloadScenes()
{
	if (shuttingDown || loading)
		return;

	struct obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);

	std::vector<SceneEntry> fresh;
	fresh.reserve(list.sources.num);
	for (size_t i = 0; i < list.sources.num; i++) {
		obs_source_t *src = list.sources.array[i];
		if (LiveEngine::IsLibraryScene(src))
			continue; /* escena interna del plugin, nunca se muestra */
		SceneEntry entry;
		entry.ref = std::make_shared<WallSourceRef>(src);
		entry.uuid = SourceUuid(src);
		entry.name = QString::fromUtf8(obs_source_get_name(src));
		fresh.push_back(std::move(entry));
	}
#ifdef SCENE_PREVIO_DEBUG
	/* Escena privada de prueba (no se guarda en la colección). */
	if (getenv("SCENE_PREVIO_TESTSCENE")) {
		static obs_scene_t *testScene = nullptr;
		if (!testScene) {
			testScene = obs_scene_create_private("Prueba Tools Scenes");
			const uint32_t colors[] = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFF808080};
			for (int i = 0; i < 4; i++) {
				obs_data_t *s = obs_data_create();
				obs_data_set_int(s, "color", colors[i]);
				obs_data_set_int(s, "width", 960);
				obs_data_set_int(s, "height", 540);
				obs_source_t *c = obs_source_create_private("color_source_v3", "bar", s);
				obs_sceneitem_t *item = obs_scene_add(testScene, c);
				struct vec2 pos;
				vec2_set(&pos, (float)((i % 2) * 960), (float)((i / 2) * 540));
				obs_sceneitem_set_pos(item, &pos);
				obs_source_release(c);
				obs_data_release(s);
			}
		}
		SceneEntry e;
		e.ref = std::make_shared<WallSourceRef>(obs_scene_get_source(testScene));
		e.uuid = "debug-test-scene";
		e.name = "Prueba Tools Scenes";
		fresh.insert(fresh.begin(), e);
	}
	/* Prueba de carga: repetir las escenas N veces (no modifica OBS). */
	if (const char *rep = getenv("SCENE_PREVIO_REPEAT")) {
		const int n = atoi(rep);
		const size_t base = fresh.size();
		for (int r = 1; r < n; r++) {
			for (size_t i = 0; i < base; i++) {
				SceneEntry e = fresh[i];
				e.uuid += "#" + std::to_string(r);
				e.name += QString(" (%1)").arg(r);
				fresh.push_back(e);
			}
		}
	}
#endif
	obs_frontend_source_list_free(&list);

	/* Las nuevas referencias se toman antes de soltar las viejas: ninguna
	 * escena que siga existiendo pierde su estado "showing". */
	scenes.swap(fresh);
	ResolvePendingAssignments();
	Relayout();
	UpdateShowing();
	UpdateActive();
	/* `fresh` (lista anterior) se libera aquí; el lienzo ya publicó una
	 * instantánea nueva y la anterior la conserva el lienzo hasta purgarla. */
}

void WallDock::UpdateActive()
{
	if (shuttingDown)
		return;
	obs_source_t *program = obs_frontend_get_current_scene();
	obs_source_t *programPtr = program;
	obs_source_release(program);

	obs_source_t *previewPtr = nullptr;
	if (obs_frontend_preview_program_mode_active()) {
		obs_source_t *preview = obs_frontend_get_current_preview_scene();
		previewPtr = preview;
		obs_source_release(preview);
	}
	/* Solo se usan como identidad para comparar, nunca se desreferencian. */
	canvas->SetActiveScenes(programPtr, previewPtr);
}

void WallDock::UpdateShowing()
{
	std::unordered_set<obs_source_t *> desired;
	if (keepLive && isVisible() && !loading && !shuttingDown) {
		for (size_t idx : ScenesInCurrentTab())
			desired.insert(scenes[idx].ref->get());
	}

	for (auto it = showing.begin(); it != showing.end();) {
		if (!desired.count(it->first)) {
			obs_source_dec_showing(it->first);
			it = showing.erase(it);
		} else {
			++it;
		}
	}
	for (size_t idx : ScenesInCurrentTab()) {
		obs_source_t *src = scenes[idx].ref->get();
		if (desired.count(src) && !showing.count(src)) {
			obs_source_inc_showing(src);
			showing.emplace(src, scenes[idx].ref);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Diseño                                                                     */

std::vector<size_t> WallDock::ScenesInCurrentTab() const
{
	std::vector<size_t> out;
	out.reserve(scenes.size());
	for (size_t i = 0; i < scenes.size(); i++) {
		if (currentTab.isEmpty()) {
			out.push_back(i);
			continue;
		}
		auto it = assignments.find(scenes[i].uuid);
		if (it != assignments.end() && it->second == currentTab)
			out.push_back(i);
	}
	return out;
}

int WallDock::HeaderHeight() const
{
	return QFontMetrics(font()).height() + 6;
}

int WallDock::ComputeContentHeight(int tw, int viewWidth, size_t count) const
{
	if (!count)
		return 0;
	const uint32_t bw = lastBaseWidth ? lastBaseWidth : 1920;
	const uint32_t bh = lastBaseHeight ? lastBaseHeight : 1080;
	const int imageH = std::max(1, (int)std::lround((double)tw * bh / bw));
	const int tileH = HeaderHeight() + imageH;
	const int cols = std::max(1, (viewWidth - 2 * kMargin + kGap) / (tw + kGap));
	const int rows = (int)((count + (size_t)cols - 1) / (size_t)cols);
	return 2 * kMargin + rows * tileH + (rows - 1) * kGap;
}

void WallDock::ScheduleRelayout()
{
	if (!shuttingDown && relayoutTimer)
		relayoutTimer->start();
}

WallColor WallDock::HeaderColorFor(const std::string &uuid) const
{
	auto it = assignments.find(uuid);
	if (it != assignments.end()) {
		if (const Group *g = FindGroup(it->second))
			return ToWallColor(g->color);
	}
	return WallColor{0.24f, 0.24f, 0.26f, 1.0f};
}

std::shared_ptr<const WallLabelImage> WallDock::LabelFor(const SceneEntry &scene, int width, int height, qreal dpr)
{
	LabelCacheEntry &entry = labelCache[scene.uuid];
	if (entry.image && entry.name == scene.name && entry.width == width && entry.height == height &&
	    entry.dpr == dpr)
		return entry.image;

	const int pw = std::max(1, (int)std::lround(width * dpr));
	const int ph = std::max(1, (int)std::lround(height * dpr));

	QImage img(pw, ph, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	img.setDevicePixelRatio(dpr);
	{
		QPainter p(&img);
		p.setRenderHint(QPainter::TextAntialiasing, true);
		QFont f = font();
		p.setFont(f);
		const QFontMetrics fm(f);
		const QRect r(4, 0, width - 8, height);
		const QString text = fm.elidedText(scene.name, Qt::ElideRight, std::max(0, r.width()));
		p.setPen(QColor(0, 0, 0, 160));
		p.drawText(r.translated(1, 1), Qt::AlignCenter, text);
		p.setPen(Qt::white);
		p.drawText(r, Qt::AlignCenter, text);
	}

	auto label = std::make_shared<WallLabelImage>();
	label->width = (uint32_t)pw;
	label->height = (uint32_t)ph;
	label->id = g_labelIds.fetch_add(1);
	label->pixels.resize((size_t)pw * (size_t)ph * 4);
	for (int y = 0; y < ph; y++)
		memcpy(label->pixels.data() + (size_t)y * (size_t)pw * 4, img.constScanLine(y), (size_t)pw * 4);

	entry.name = scene.name;
	entry.width = width;
	entry.height = height;
	entry.dpr = dpr;
	entry.image = label;
	return label;
}

void WallDock::Relayout()
{
	if (shuttingDown)
		return;

	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height) {
		lastBaseWidth = ovi.base_width;
		lastBaseHeight = ovi.base_height;
	}
	const uint32_t bw = lastBaseWidth ? lastBaseWidth : 1920;
	const uint32_t bh = lastBaseHeight ? lastBaseHeight : 1080;

	const qreal dpr = canvas->devicePixelRatioF();
	const int viewW = std::max(1, canvas->width());
	const int tw = std::clamp(tileWidth, kMinTile, std::max(kMinTile, viewW - 2 * kMargin));
	const int headerH = HeaderHeight();
	const int imageH = std::max(1, (int)std::lround((double)tw * bh / bw));
	const int tileH = headerH + imageH;
	const int cols = std::max(1, (viewW - 2 * kMargin + kGap) / (tw + kGap));

	const std::vector<size_t> visible = loading ? std::vector<size_t>() : ScenesInCurrentTab();

	auto snap = std::make_shared<WallSnapshot>();
	layout.clear();
	layout.reserve(visible.size());
	snap->tiles.reserve(visible.size());
	snap->refs.reserve(visible.size());

	/* Ancho/alto en píxeles físicos calculados una vez para que todas las
	 * miniaturas tengan exactamente el mismo tamaño. */
	const int pw = (int)std::lround(tw * dpr);
	const int phh = (int)std::lround(headerH * dpr);
	const int pih = (int)std::lround(imageH * dpr);

	for (size_t k = 0; k < visible.size(); k++) {
		const SceneEntry &scene = scenes[visible[k]];
		const int col = (int)(k % (size_t)cols);
		const int row = (int)(k / (size_t)cols);
		const int x = kMargin + col * (tw + kGap);
		const int y = kMargin + row * (tileH + kGap);

		LayoutTile lt;
		lt.sceneIndex = visible[k];
		lt.rect = QRect(x, y, tw, tileH);
		layout.push_back(lt);

		WallTile tile;
		tile.source = scene.ref->get();
		tile.key = scene.uuid;
		tile.x = (int)std::lround(x * dpr);
		tile.y = (int)std::lround(y * dpr);
		tile.w = pw;
		tile.headerH = phh;
		tile.imageH = pih;
		tile.headerColor = HeaderColorFor(scene.uuid);
		tile.label = LabelFor(scene, tw, headerH, dpr);
		snap->tiles.push_back(std::move(tile));
		snap->refs.push_back(std::make_unique<WallSourceRef>(scene.ref->get()));
	}

	const int rows = visible.empty() ? 0 : (int)((visible.size() + (size_t)cols - 1) / (size_t)cols);
	contentHeight = rows ? 2 * kMargin + rows * tileH + (rows - 1) * kGap : 0;
	snap->contentHeight = (int)std::lround(contentHeight * dpr);

	canvas->SetSnapshot(std::move(snap));
	UpdateScrollBar();

	/* Limpiar rótulos de escenas que ya no existen. */
	if (labelCache.size() > scenes.size() * 2 + 16) {
		std::unordered_set<std::string> alive;
		for (const auto &s : scenes)
			alive.insert(s.uuid);
		for (auto it = labelCache.begin(); it != labelCache.end();)
			it = alive.count(it->first) ? std::next(it) : labelCache.erase(it);
	}
}

void WallDock::UpdateScrollBar()
{
	const int viewH = std::max(1, canvas->height());
	const int maxScroll = std::max(0, contentHeight - viewH);
	scrollBar->setRange(0, maxScroll);
	scrollBar->setPageStep(viewH);
	scrollBar->setSingleStep(40);
	scrollBar->setVisible(maxScroll > 0);
	canvas->SetScrollY((int)std::lround(scrollBar->value() * canvas->devicePixelRatioF()));
}

int WallDock::HitTest(const QPoint &pos) const
{
	const QPoint p(pos.x(), pos.y() + scrollBar->value());
	for (size_t i = 0; i < layout.size(); i++) {
		if (layout[i].rect.contains(p))
			return (int)i;
	}
	return -1;
}

void WallDock::AutoSize()
{
	const size_t count = ScenesInCurrentTab().size();
	if (!count)
		return;
	const int viewW = std::max(1, canvas->width());
	const int viewH = std::max(1, canvas->height());
	const int maxW = std::max(kMinTile, std::min(kMaxTile, viewW - 2 * kMargin));
	int best = kMinTile;
	for (int tw = maxW; tw >= kMinTile; tw -= 2) {
		if (ComputeContentHeight(tw, viewW, count) <= viewH) {
			best = tw;
			break;
		}
	}
	sizeSlider->setValue(best);
	SaveGlobalSettings();
}

/* ------------------------------------------------------------------------ */
/* Pestañas / grupos                                                          */

const WallDock::Group *WallDock::FindGroup(const QString &id) const
{
	for (const auto &g : groups) {
		if (g.id == id)
			return &g;
	}
	return nullptr;
}

void WallDock::RebuildTabs()
{
	for (QPushButton *b : tabButtons)
		b->deleteLater();
	tabButtons.clear();
	while (QLayoutItem *item = tabLayout->takeAt(0))
		delete item;

	if (!currentTab.isEmpty() && !FindGroup(currentTab))
		currentTab.clear();

	auto addButton = [this](const QString &id, const QString &name, const QColor &color) {
		auto *b = new QPushButton(name, tabBar);
		b->setCursor(Qt::PointingHandCursor);
		b->setStyleSheet(ButtonStyle(color, id == currentTab));
		b->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(b, &QPushButton::clicked, this, [this, id]() { SetCurrentTab(id); });
		connect(b, &QPushButton::customContextMenuRequested, this, [this, id, b](const QPoint &pos) {
			SetCurrentTab(id);
			ShowSettingsMenu(b->mapToGlobal(pos));
		});
		tabLayout->addWidget(b);
		tabButtons.push_back(b);
	};

	addButton(QString(), T("Tab.All"), QColor(70, 70, 75));
	for (const auto &g : groups)
		addButton(g.id, g.name, g.color);
	tabLayout->addStretch(1);
}

void WallDock::SetCurrentTab(const QString &groupId)
{
	if (currentTab == groupId)
		return;
	currentTab = groupId;
	RebuildTabs();
	scrollBar->setValue(0);
	Relayout();
	UpdateShowing();
}

bool WallDock::CreateGroupDialog(QString &outId)
{
	bool ok = false;
	const QString name =
		QInputDialog::getText(this, T("Group.New"), T("Group.Name"), QLineEdit::Normal, QString(), &ok).trimmed();
	if (!ok || name.isEmpty())
		return false;

	static const QColor palette[] = {QColor(200, 30, 30), QColor(30, 110, 40), QColor(30, 60, 200),
					 QColor(180, 110, 0), QColor(120, 40, 160), QColor(0, 130, 140)};
	const QColor initial = palette[groups.size() % (sizeof(palette) / sizeof(palette[0]))];
	QColor color = QColorDialog::getColor(initial, this, T("Group.Color"));
	if (!color.isValid())
		color = initial;

	Group g;
	g.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	g.name = name;
	g.color = color;
	groups.push_back(g);
	outId = g.id;
	return true;
}

void WallDock::RenameGroup(const QString &id)
{
	for (auto &g : groups) {
		if (g.id != id)
			continue;
		bool ok = false;
		const QString name = QInputDialog::getText(this, T("Group.Rename"), T("Group.Name"), QLineEdit::Normal,
							   g.name, &ok)
					     .trimmed();
		if (ok && !name.isEmpty()) {
			g.name = name;
			CollectionDataChanged();
		}
		return;
	}
}

void WallDock::RecolorGroup(const QString &id)
{
	for (auto &g : groups) {
		if (g.id != id)
			continue;
		const QColor color = QColorDialog::getColor(g.color, this, T("Group.Color"));
		if (color.isValid()) {
			g.color = color;
			CollectionDataChanged();
		}
		return;
	}
}

void WallDock::DeleteGroup(const QString &id)
{
	const Group *g = FindGroup(id);
	if (!g)
		return;
	const auto answer = QMessageBox::question(this, T("Group.Delete"), T("Group.DeleteConfirm").arg(g->name));
	if (answer != QMessageBox::Yes)
		return;
	groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const Group &x) { return x.id == id; }),
		     groups.end());
	for (auto it = assignments.begin(); it != assignments.end();)
		it = it->second == id ? assignments.erase(it) : std::next(it);
	for (auto it = pendingByName.begin(); it != pendingByName.end();)
		it = it->second == id ? pendingByName.erase(it) : std::next(it);
	if (currentTab == id)
		currentTab.clear();
	CollectionDataChanged();
}

void WallDock::CollectionDataChanged()
{
	RebuildTabs();
	Relayout();
	UpdateShowing();
	/* Guardar en la colección de escenas y también en el perfil. */
	QTimer::singleShot(0, this, []() { obs_frontend_save(); });
	WriteProfileData();
}

/* ------------------------------------------------------------------------ */
/* Menús                                                                      */

void WallDock::ShowTileMenu(int layoutIndex, const QPoint &globalPos)
{
	if (layoutIndex < 0 || (size_t)layoutIndex >= layout.size())
		return;
	const SceneEntry &scene = scenes[layout[(size_t)layoutIndex].sceneIndex];
	/* Copias propias: la lista puede cambiar mientras el menú está abierto. */
	const std::shared_ptr<WallSourceRef> ref = scene.ref;
	const std::string uuid = scene.uuid;
	const QString name = scene.name;
	QPointer<WallDock> self(this);

	QMenu menu(this);
	QAction *title = menu.addAction(name);
	title->setEnabled(false);
	menu.addSeparator();

	const bool studio = obs_frontend_preview_program_mode_active();
	QAction *toPreview = menu.addAction(T("Menu.ToPreview"));
	toPreview->setEnabled(studio);
	connect(toPreview, &QAction::triggered, this, [ref]() { obs_frontend_set_current_preview_scene(ref->get()); });
	QAction *toProgram = menu.addAction(T("Menu.ToProgram"));
	connect(toProgram, &QAction::triggered, this, [ref]() { obs_frontend_set_current_scene(ref->get()); });
	menu.addSeparator();

	QMenu *assign = menu.addMenu(T("Menu.AssignGroup"));
	auto current = assignments.find(uuid);
	for (const auto &g : groups) {
		QAction *a = assign->addAction(g.name);
		a->setCheckable(true);
		a->setChecked(current != assignments.end() && current->second == g.id);
		const QString gid = g.id;
		connect(a, &QAction::triggered, this, [this, uuid, gid, name]() {
			assignments[uuid] = gid;
			assignmentNames[uuid] = name;
			CollectionDataChanged();
		});
	}
	if (!groups.empty())
		assign->addSeparator();
	QAction *newGroup = assign->addAction(T("Group.New") + QString::fromUtf8("…"));
	connect(newGroup, &QAction::triggered, this, [this, uuid, name]() {
		QString id;
		if (CreateGroupDialog(id)) {
			assignments[uuid] = id;
			assignmentNames[uuid] = name;
			CollectionDataChanged();
		}
	});
	if (current != assignments.end()) {
		QAction *unassign = menu.addAction(T("Menu.RemoveFromGroup"));
		connect(unassign, &QAction::triggered, this, [this, uuid, name]() {
			assignments.erase(uuid);
			assignmentNames.erase(uuid);
			pendingByName.erase(name);
			CollectionDataChanged();
		});
	}
	menu.addSeparator();

	QAction *filters = menu.addAction(T("Menu.Filters"));
	connect(filters, &QAction::triggered, this, [ref]() { obs_frontend_open_source_filters(ref->get()); });
	QAction *projector = menu.addAction(T("Menu.Projector"));
	connect(projector, &QAction::triggered, this, [name]() {
		obs_frontend_open_projector("Scene", -1, nullptr, name.toUtf8().constData());
	});

	menu.exec(globalPos);
	(void)self;
}

void WallDock::ShowSettingsMenu(const QPoint &globalPos)
{
	QMenu menu(this);

	QAction *newGroup = menu.addAction(T("Group.New") + QString::fromUtf8("…"));
	connect(newGroup, &QAction::triggered, this, [this]() {
		QString id;
		if (CreateGroupDialog(id)) {
			currentTab = id;
			CollectionDataChanged();
		}
	});

	if (const Group *g = FindGroup(currentTab)) {
		const QString id = g->id;
		menu.addSeparator();
		QAction *rename = menu.addAction(T("Group.Rename") + QString(" «%1»…").arg(g->name));
		connect(rename, &QAction::triggered, this, [this, id]() { RenameGroup(id); });
		QAction *recolor = menu.addAction(T("Group.Color") + QString::fromUtf8("…"));
		connect(recolor, &QAction::triggered, this, [this, id]() { RecolorGroup(id); });
		QAction *del = menu.addAction(T("Group.Delete"));
		connect(del, &QAction::triggered, this, [this, id]() { DeleteGroup(id); });
	}

	menu.addSeparator();
	QAction *live = menu.addAction(T("Option.KeepLive"));
	live->setCheckable(true);
	live->setChecked(keepLive);
	live->setToolTip(T("Option.KeepLive.Tooltip"));
	connect(live, &QAction::toggled, this, [this](bool on) {
		keepLive = on;
		UpdateShowing();
		SaveGlobalSettings();
	});
	QAction *dbl = menu.addAction(T("Option.DoubleClick"));
	dbl->setCheckable(true);
	dbl->setChecked(doubleClickToProgram);
	connect(dbl, &QAction::toggled, this, [this](bool on) {
		doubleClickToProgram = on;
		SaveGlobalSettings();
	});

	menu.addSeparator();
	QAction *about = menu.addAction(T("About"));
	connect(about, &QAction::triggered, this, [this]() {
		QMessageBox::about(this, QString("Tools Scenes"),
				   T("About.Text").arg(QString::fromUtf8(PLUGIN_VERSION)));
	});

	menu.exec(globalPos);
}

/* ------------------------------------------------------------------------ */
/* Visibilidad                                                                */

void WallDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	/* La superficie nativa del lienzo debe ocultarse explícitamente cuando el
	 * panel queda detrás de otra pestaña; si no, en algunos sistemas sigue
	 * dibujada encima del panel visible. */
	if (!dockHooked) {
		if (auto *dw = qobject_cast<QDockWidget *>(parentWidget())) {
			connect(dw, &QDockWidget::visibilityChanged, this, [this](bool visible) {
				if (canvas && !shuttingDown)
					canvas->setVisible(visible);
			});
			dockHooked = true;
		}
	}
	if (canvas && !canvas->isVisible())
		canvas->show();
	if (!loading && !shuttingDown) {
		Relayout();
		UpdateShowing();
	}
}

void WallDock::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	if (canvas)
		canvas->hide();
	/* Panel oculto: las escenas dejan de mantenerse activas por nosotros. */
	if (!shuttingDown)
		UpdateShowing();
}

/* ------------------------------------------------------------------------ */
/* Persistencia                                                               */

void WallDock::LoadGlobalSettings()
{
	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;
	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!data)
		return;
	obs_data_set_default_int(data, "tile_width", 220);
	obs_data_set_default_int(data, "refresh_fps", 10);
	obs_data_set_default_bool(data, "keep_live", true);
	obs_data_set_default_bool(data, "double_click_program", true);
	tileWidth = std::clamp((int)obs_data_get_int(data, "tile_width"), kMinTile, kMaxTile);
	refreshFps = (int)obs_data_get_int(data, "refresh_fps");
	keepLive = obs_data_get_bool(data, "keep_live");
	doubleClickToProgram = obs_data_get_bool(data, "double_click_program");
	obs_data_release(data);
}

void WallDock::SaveGlobalSettings() const
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "tile_width", tileWidth);
	obs_data_set_int(data, "refresh_fps", refreshFps);
	obs_data_set_bool(data, "keep_live", keepLive);
	obs_data_set_bool(data, "double_click_program", doubleClickToProgram);
	if (!obs_data_save_json_safe(data, path, "tmp", "bak"))
		blog(LOG_WARNING, "[tools-scenes] no se pudo guardar %s", path);
	obs_data_release(data);
	bfree(path);
}

bool WallDock::ParseGroupsData(obs_data_t *data)
{
	groups.clear();
	assignments.clear();
	assignmentNames.clear();
	pendingByName.clear();
	currentTab.clear();
	if (!data)
		return false;

	obs_data_array_t *arr = obs_data_get_array(data, "groups");
	const size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		Group g;
		g.id = QString::fromUtf8(obs_data_get_string(item, "id"));
		g.name = QString::fromUtf8(obs_data_get_string(item, "name"));
		g.color = QColor(QString::fromUtf8(obs_data_get_string(item, "color")));
		if (!g.color.isValid())
			g.color = QColor(70, 70, 75);
		if (!g.id.isEmpty() && !FindGroup(g.id))
			groups.push_back(g);
		obs_data_release(item);
	}
	obs_data_array_release(arr);

	arr = obs_data_get_array(data, "assignments");
	const size_t m = obs_data_array_count(arr);
	for (size_t i = 0; i < m; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const std::string uuid = obs_data_get_string(item, "scene");
		const QString name = QString::fromUtf8(obs_data_get_string(item, "scene_name"));
		const QString gid = QString::fromUtf8(obs_data_get_string(item, "group"));
		if (FindGroup(gid)) {
			if (!uuid.empty()) {
				assignments[uuid] = gid;
				if (!name.isEmpty())
					assignmentNames[uuid] = name;
			}
			/* Si la escena tiene otro UUID (otra colección), se reconoce por nombre. */
			if (!name.isEmpty())
				pendingByName[name] = gid;
		}
		obs_data_release(item);
	}
	obs_data_array_release(arr);

	const QString tab = QString::fromUtf8(obs_data_get_string(data, "current_tab"));
	if (FindGroup(tab))
		currentTab = tab;
	return !groups.empty();
}

void WallDock::ResolvePendingAssignments()
{
	for (const SceneEntry &scene : scenes) {
		auto it = assignments.find(scene.uuid);
		if (it != assignments.end()) {
			assignmentNames[scene.uuid] = scene.name;
			continue;
		}
		auto byName = pendingByName.find(scene.name);
		if (byName != pendingByName.end() && FindGroup(byName->second)) {
			assignments[scene.uuid] = byName->second;
			assignmentNames[scene.uuid] = scene.name;
		}
	}
}

QString WallDock::ProfileFilePath()
{
	char *dir = obs_frontend_get_current_profile_path();
	if (!dir)
		return QString();
	const QString path = QString::fromUtf8(dir) + "/scene-previo-groups.json";
	bfree(dir);
	return path;
}

static QString CurrentCollectionName()
{
	char *name = obs_frontend_get_current_scene_collection();
	const QString result = name ? QString::fromUtf8(name) : QString();
	bfree(name);
	return result;
}

void WallDock::WriteProfileData() const
{
	const QString path = ProfileFilePath();
	if (path.isEmpty())
		return;
	const std::string p = path.toUtf8().toStdString();
	obs_data_t *root = obs_data_create_from_json_file_safe(p.c_str(), "bak");
	if (!root)
		root = obs_data_create();
	obs_data_t *collections = obs_data_get_obj(root, "collections");
	if (!collections)
		collections = obs_data_create();

	obs_data_t *entry = SaveCollectionData();
	const QString collection = CurrentCollectionName();
	if (!collection.isEmpty())
		obs_data_set_obj(collections, collection.toUtf8().constData(), entry);
	obs_data_set_obj(root, "collections", collections);
	obs_data_set_obj(root, "last", entry);
	if (!obs_data_save_json_safe(root, p.c_str(), "tmp", "bak"))
		blog(LOG_WARNING, "[tools-scenes] no se pudieron guardar los grupos en el perfil: %s", p.c_str());
	obs_data_release(entry);
	obs_data_release(collections);
	obs_data_release(root);
}

bool WallDock::ReadProfileData()
{
	const QString path = ProfileFilePath();
	if (path.isEmpty())
		return false;
	obs_data_t *root = obs_data_create_from_json_file_safe(path.toUtf8().constData(), "bak");
	if (!root)
		return false;
	bool found = false;
	obs_data_t *collections = obs_data_get_obj(root, "collections");
	const QString collection = CurrentCollectionName();
	obs_data_t *entry = (collections && !collection.isEmpty())
				    ? obs_data_get_obj(collections, collection.toUtf8().constData())
				    : nullptr;
	if (!entry)
		entry = obs_data_get_obj(root, "last");
	if (entry) {
		found = ParseGroupsData(entry);
		obs_data_release(entry);
	}
	obs_data_release(collections);
	obs_data_release(root);
	return found;
}

void WallDock::LoadCollectionData(obs_data_t *data)
{
	/* Prioridad: datos de la colección; si no hay grupos, los del perfil. */
	if (ParseGroupsData(data)) {
		blog(LOG_INFO, "[tools-scenes] grupos cargados de la colección: %zu", groups.size());
	} else if (ReadProfileData()) {
		blog(LOG_INFO, "[tools-scenes] grupos recuperados del perfil: %zu", groups.size());
		/* Volver a dejarlos también en la colección. */
		QTimer::singleShot(0, this, []() { obs_frontend_save(); });
	} else {
		blog(LOG_INFO, "[tools-scenes] sin grupos guardados (colección %s)", data ? "con datos" : "sin datos");
	}
	ResolvePendingAssignments();
	RebuildTabs();
	if (!loading)
		ReloadScenes();
}

obs_data_t *WallDock::SaveCollectionData() const
{
	obs_data_t *data = obs_data_create();

	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &g : groups) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", g.id.toUtf8().constData());
		obs_data_set_string(item, "name", g.name.toUtf8().constData());
		obs_data_set_string(item, "color", g.color.name().toUtf8().constData());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "groups", arr);
	obs_data_array_release(arr);

	arr = obs_data_array_create();
	for (const auto &a : assignments) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "scene", a.first.c_str());
		obs_data_set_string(item, "group", a.second.toUtf8().constData());
		auto name = assignmentNames.find(a.first);
		if (name != assignmentNames.end())
			obs_data_set_string(item, "scene_name", name->second.toUtf8().constData());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "assignments", arr);
	obs_data_array_release(arr);

	obs_data_set_string(data, "current_tab", currentTab.toUtf8().constData());
	return data;
}
