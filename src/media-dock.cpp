/*
 * Tools Scenes - panel "Multimedia y capas" (ver media-dock.hpp).
 */
#include "media-dock.hpp"
#include "flow-layout.hpp"
#include "media-card.hpp"
#include "transform-editor.hpp"
#include "thumbnail-loader.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QFileSystemWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#ifdef SCENE_PREVIO_DEBUG
#include <QDockWidget>
#include <QPainter>
#endif

#include <algorithm>
#include <cmath>

namespace {

constexpr const char *kSaveKey = "scene-previo-media";

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const QStringList kImageExt = {"png", "jpg", "jpeg", "bmp", "tga", "gif", "webp", "psd", "jxr"};
const QStringList kVideoExt = {"mp4", "m4v", "mov", "mkv", "avi", "webm", "flv", "ts", "mts", "m2ts",
			       "mpg", "mpeg", "wmv", "mxf", "3gp"};
const QStringList kAudioExt = {"mp3", "wav", "aac", "m4a", "flac", "ogg", "opus", "wma"};
const QStringList kHtmlExt = {"html", "htm"};
const QStringList kPdfExt = {"pdf"};

struct InputType {
	const char *label;
	std::vector<const char *> ids;
};
const std::vector<InputType> kInputTypes = {
	{"Input.Ndi", {"ndi_source"}},
	{"Input.AudioIn", {"wasapi_input_capture", "coreaudio_input_capture", "pulse_input_capture"}},
	{"Input.AudioOut", {"wasapi_output_capture", "coreaudio_output_capture", "pulse_output_capture"}},
	{"Input.Video", {"dshow_input", "macos-avcapture", "av_capture_input_v2", "av_capture_input", "v4l2_input"}},
	{"Input.Media", {"ffmpeg_source"}},
	{"Input.Browser", {"browser_source"}},
	{"Input.Text", {"text_gdiplus_v3", "text_gdiplus_v2", "text_ft2_source_v2"}},
	{"Input.Color", {"color_source_v3"}},
};

const char *FirstAvailable(const std::vector<const char *> &ids)
{
	for (const char *id : ids)
		if (obs_source_get_display_name(id))
			return id;
	return nullptr;
}

QString UniqueSourceName(const QString &base)
{
	QString name = base;
	for (int i = 2; i < 1000; i++) {
		OBSSourceAutoRelease existing = obs_get_source_by_name(name.toUtf8().constData());
		if (!existing)
			return name;
		name = QString("%1 (%2)").arg(base).arg(i);
	}
	return name;
}

std::string ToStd(const QString &s)
{
	return s.toUtf8().toStdString();
}

QString AbsoluteUrl(QString path)
{
	path.replace('\\', '/');
	return "http://absolute/" + QString::fromUtf8(QUrl::toPercentEncoding(path, "/:"));
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

} // namespace

/* ======================================================================== */

MediaDock::MediaDock(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(220, 220);
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(4);

	/* Barra (se acomoda en varias filas si el panel es angosto) */
	auto *barHost = new QWidget(this);
	auto *bar = new FlowLayout(barHost, 3);
	bar->setContentsMargins(0, 0, 0, 0);
	auto *addFolder = new QPushButton(T("Media.AddFolder"), barHost);
	auto *addFiles = new QPushButton(T("Media.AddFiles"), barHost);
	auto *addInput = new QPushButton(T("Media.AddInput"), barHost);
	bar->addWidget(addFolder);
	bar->addWidget(addFiles);
	bar->addWidget(addInput);
	auto *sizeBox = new QWidget(barHost);
	auto *sizeRow = new QHBoxLayout(sizeBox);
	sizeRow->setContentsMargins(6, 0, 0, 0);
	sizeRow->addWidget(new QLabel(T("Size"), sizeBox));
	cardSize = new QSlider(Qt::Horizontal, sizeBox);
	cardSize->setRange(130, 360);
	cardSize->setValue(cardWidth);
	cardSize->setFixedWidth(100);
	sizeRow->addWidget(cardSize);
	bar->addWidget(sizeBox);
	auto *fadeBox = new QWidget(barHost);
	auto *fadeRow = new QHBoxLayout(fadeBox);
	fadeRow->setContentsMargins(6, 0, 0, 0);
	fadeRow->addWidget(new QLabel(T("Layer.Transition"), fadeBox));
	transitionSpin = new QSpinBox(fadeBox);
	transitionSpin->setRange(0, 10000);
	transitionSpin->setSingleStep(100);
	transitionSpin->setSuffix(" ms");
	transitionSpin->setValue(LiveEngine::Get().TransitionMs());
	fadeRow->addWidget(transitionSpin);
	bar->addWidget(fadeBox);
	const int rowH = addFolder->sizeHint().height();
	sizeBox->setFixedHeight(rowH);
	fadeBox->setFixedHeight(std::max(rowH, transitionSpin->sizeHint().height()));
	root->addWidget(barHost);

	/* Elemento seleccionado: nombre, Editar y controles (también fluye) */
	auto *itemHost = new QWidget(this);
	auto *itemBar = new FlowLayout(itemHost, 3);
	itemBar->setContentsMargins(0, 0, 0, 0);
	itemTitle = new QLabel(T("Media.SelectHint"), itemHost);
	itemTitle->setStyleSheet("font-weight:bold;padding-top:4px;");
	itemTitle->setMaximumWidth(260);
	btnEdit = new QPushButton(T("Media.Edit"), itemHost);
	btnEdit->setToolTip(T("Media.EditTip"));
	btnEdit->setStyleSheet("QPushButton{font-weight:bold;}");
	btnPlay = new QPushButton(QString::fromUtf8("\u25B6 / \u275A\u275A"), itemHost);
	btnPlay->setToolTip(T("Media.PlayPause"));
	btnRestart = new QPushButton(T("Media.Restart"), itemHost);
	btnLoop = new QPushButton(T("Media.Loop"), itemHost);
	btnLoop->setCheckable(true);
	btnPrevPage = new QPushButton(QString::fromUtf8("\u25C0 ") + T("Media.Page"), itemHost);
	btnNextPage = new QPushButton(T("Media.Page") + QString::fromUtf8(" \u25B6"), itemHost);
	itemBar->addWidget(itemTitle);
	for (QPushButton *btn : {btnEdit, btnPlay, btnRestart, btnLoop, btnPrevPage, btnNextPage})
		itemBar->addWidget(btn);
	root->addWidget(itemHost);

	/* ---- Tarjetas ---- */
	scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setMinimumHeight(110);
	cardsHost = new QWidget(scroll);
	cardsLayout = new QVBoxLayout(cardsHost);
	cardsLayout->setContentsMargins(0, 0, 0, 0);
	cardsLayout->setSpacing(2);
	scroll->setWidget(cardsHost);
	root->addWidget(scroll, 1);

	/* ---- Capas (compacto) ---- */
	auto *group = new QGroupBox(T("Layers"), this);
	auto *grid = new QGridLayout(group);
	grid->setContentsMargins(6, 4, 6, 4);
	grid->setHorizontalSpacing(4);
	grid->setVerticalSpacing(2);
	for (int i = 0; i < kLayerCount; i++) {
		LayerRow &row = layerRows[(size_t)i];
		row.toggle = new QPushButton(QString("%1 %2").arg(T("Layer")).arg(i + 1), group);
		row.toggle->setCheckable(true);
		row.toggle->setMinimumWidth(64);
		row.toggle->setToolTip(T("Layer.ToggleTooltip"));
		row.content = new QLabel(T("Layer.Empty"), group);
		row.clear = new QPushButton("X", group);
		row.clear->setToolTip(T("Layer.Clear"));
		row.clear->setFixedWidth(30);
		row.clear->setStyleSheet("QPushButton{padding:0px;font-weight:bold;}");
		const int r = i / 2, c = (i % 2) * 3;
		grid->addWidget(row.toggle, r, c);
		grid->addWidget(row.content, r, c + 1);
		grid->addWidget(row.clear, r, c + 2);
		connect(row.toggle, &QPushButton::clicked, this, [this, i](bool checked) {
			LiveEngine::Get().SetLayerVisible(i, checked);
			RefreshLayerPanel();
		});
		connect(row.clear, &QPushButton::clicked, this, [i]() { LiveEngine::Get().ClearLayer(i); });
	}
	grid->setColumnStretch(1, 1);
	grid->setColumnStretch(4, 1);
	group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	root->addWidget(group);
	layersGroup = group;
	group->setVisible(!obs_frontend_preview_program_mode_active());

	watcher = new QFileSystemWatcher(this);
	thumbs = std::make_unique<ThumbnailLoader>(this, [this](const QString &path, const QImage &img) {
		thumbCache[path] = img;
		const QString key = "f:" + path;
		for (MediaCard *c : cards)
			if (c->Key() == key)
				c->SetThumbnail(img);
	});

	/* Conexiones */
	connect(addFolder, &QPushButton::clicked, this, [this]() { AddFolder(); });
	connect(addFiles, &QPushButton::clicked, this, [this]() { AddFiles(); });
	connect(addInput, &QPushButton::clicked, this, [this]() { ShowAddInputMenu(); });
	connect(cardSize, &QSlider::valueChanged, this, [this](int v) {
		cardWidth = v;
		for (MediaCard *c : cards)
			c->SetCardWidth(v);
	});
	connect(btnEdit, &QPushButton::clicked, this, [this]() { OpenEditor(selectedKey); });
	connect(btnPlay, &QPushButton::clicked, this, [this]() { MediaPlayPause(); });
	connect(btnRestart, &QPushButton::clicked, this, [this]() { MediaRestart(); });
	connect(btnLoop, &QPushButton::clicked, this, [this]() { ToggleLoop(); });
	connect(btnPrevPage, &QPushButton::clicked, this, [this]() { PdfPage(-1); });
	connect(btnNextPage, &QPushButton::clicked, this, [this]() { PdfPage(1); });
	connect(transitionSpin, qOverload<int>(&QSpinBox::valueChanged), this,
		[](int v) { LiveEngine::Get().SetTransitionMs(v); });
	connect(watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &) {
		QTimer::singleShot(400, this, [this]() { RebuildCards(); });
	});

	LiveEngine::Get().AddListener(this, [this]() {
		RefreshLayerPanel();
		RefreshCardStates();
	});

	obs_frontend_add_event_callback(FrontendEvent, this);
	obs_frontend_add_save_callback(FrontendSave, this);
	callbacksRegistered = true;

	RebuildCards();
	RefreshLayerPanel();
	UpdateControls();
}

MediaDock::~MediaDock()
{
	Shutdown();
}

void MediaDock::Shutdown()
{
	if (callbacksRegistered) {
		obs_frontend_remove_event_callback(FrontendEvent, this);
		obs_frontend_remove_save_callback(FrontendSave, this);
		callbacksRegistered = false;
	}
	LiveEngine::Get().RemoveListener(this);
	shuttingDown = true;
	thumbs.reset();
	if (editor) {
		editor->onClosed = nullptr;
		editor->onChanged = nullptr;
		editor->Detach();
		editor->hide();
	}
	previewTemp = nullptr;
}

/* ------------------------------------------------------------------------ */

void MediaDock::FrontendEvent(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<MediaDock *>(data);
	if (self->shuttingDown)
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		/* No en CHANGING: OBS guarda la colección después de ese evento. */
		if (self->editor && self->editor->isVisible())
			self->editor->close();
		self->previewTemp = nullptr;
		self->previewTempKey.clear();
		self->selectedKey.clear();
		self->folders.clear();
		self->files.clear();
		self->inputs.clear();
		self->fileSources.clear();
		self->transforms.clear();
		self->RebuildCards();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		self->layersGroup->setVisible(!obs_frontend_preview_program_mode_active());
		self->RebuildCards();
		self->RefreshLayerPanel();
#ifdef SCENE_PREVIO_DEBUG
		if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && getenv("SCENE_PREVIO_SELFTEST"))
			self->SelfTest(getenv("SCENE_PREVIO_SELFTEST"));
#endif
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		/* En Modo Estudio las capas van entre Vista previa y Programa. */
		self->layersGroup->setVisible(event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED);
		self->RefreshCardStates();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		self->RefreshCardStates();
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		self->Shutdown();
		break;
	default:
		break;
	}
}

void MediaDock::FrontendSave(obs_data_t *saveData, bool saving, void *data)
{
	auto *self = static_cast<MediaDock *>(data);
	if (saving) {
		obs_data_t *obj = obs_data_create();
		self->Save(obj);
		LiveEngine::Get().Save(obj);
		obs_data_set_obj(saveData, kSaveKey, obj);
		obs_data_release(obj);
	} else {
		obs_data_t *obj = obs_data_get_obj(saveData, kSaveKey);
		self->Load(obj);
		LiveEngine::Get().Load(obj);
		obs_data_release(obj);
		self->transitionSpin->setValue(LiveEngine::Get().TransitionMs());
		self->RefreshLayerPanel();
		self->RefreshCardStates();
	}
}

/* ------------------------------------------------------------------------ */
/* Tipos                                                                     */

QString MediaDock::FileType(const QString &path)
{
	const QString ext = QFileInfo(path).suffix().toLower();
	if (kImageExt.contains(ext))
		return "image";
	if (kVideoExt.contains(ext))
		return "video";
	if (kAudioExt.contains(ext))
		return "audio";
	if (kPdfExt.contains(ext))
		return "pdf";
	if (kHtmlExt.contains(ext))
		return "html";
	return QString();
}

QStringList MediaDock::SupportedFilters()
{
	QStringList out;
	for (const QStringList *list : {&kImageExt, &kVideoExt, &kAudioExt, &kPdfExt, &kHtmlExt})
		for (const QString &e : *list)
			out << "*." + e << "*." + e.toUpper();
	return out;
}

QString MediaDock::TypeTagForFile(const QString &path)
{
	const QString type = FileType(path);
	if (type == "image")
		return T("Tag.Image");
	if (type == "video")
		return T("Tag.Video");
	if (type == "audio")
		return T("Tag.Audio");
	if (type == "pdf")
		return "PDF";
	if (type == "html")
		return "WEB";
	return QString();
}

QString MediaDock::TypeTagForSource(obs_source_t *source)
{
	if (!source)
		return "?";
	const QString id = QString::fromUtf8(obs_source_get_id(source));
	if (id == "ndi_source")
		return "NDI";
	if (id.contains("audio") || id.contains("wasapi"))
		return T("Tag.Audio");
	if (id.contains("dshow") || id.contains("avcapture") || id.contains("capture_input") || id.contains("v4l2"))
		return T("Tag.Camera");
	if (id == "browser_source")
		return "WEB";
	if (id == "ffmpeg_source")
		return T("Tag.Video");
	if (id.startsWith("text"))
		return T("Tag.Text");
	if (id.startsWith("color"))
		return "COLOR";
	if (obs_scene_from_source(source))
		return T("Tag.Scene");
	const char *disp = obs_source_get_display_name(obs_source_get_id(source));
	return disp ? QString::fromUtf8(disp) : id;
}

/* ------------------------------------------------------------------------ */
/* Tarjetas                                                                  */

void MediaDock::RebuildCards()
{
	if (shuttingDown)
		return;
	cards.clear();
	while (QLayoutItem *item = cardsLayout->takeAt(0)) {
		if (QWidget *w = item->widget())
			w->deleteLater();
		delete item;
	}
	if (!watcher->directories().isEmpty())
		watcher->removePaths(watcher->directories());

	auto addSection = [this](const QString &id, const QString &title, const QString &tooltip) -> FlowLayout * {
		auto *header = new QPushButton(cardsHost);
		const bool isCollapsed = collapsed.count(id) > 0;
		header->setText(QString::fromUtf8(isCollapsed ? "▸  " : "▾  ") + title);
		header->setToolTip(tooltip);
		header->setFlat(true);
		header->setStyleSheet("QPushButton{text-align:left;font-weight:bold;padding:3px 4px;}");
		connect(header, &QPushButton::clicked, this, [this, id]() {
			if (collapsed.count(id))
				collapsed.erase(id);
			else
				collapsed.insert(id);
			RebuildCards();
		});
		cardsLayout->addWidget(header);
		auto *box = new QWidget(cardsHost);
		auto *flow = new FlowLayout(box, 6);
		box->setVisible(!isCollapsed);
		cardsLayout->addWidget(box);
		return flow;
	};

	auto addCard = [this](FlowLayout *flow, const QString &key, const QString &title, const QString &tag) {
		auto *card = new MediaCard(key, title, tag, flow->parentWidget());
		card->SetCardWidth(cardWidth);
		card->onSelect = [this, key]() { SelectKey(key); };
		card->onDoubleClick = [this, key]() {
			if (obs_frontend_preview_program_mode_active())
				SendToPreview(key);
			else
				SendToProgram(key);
		};
		card->onPreview = [this, key]() { SendToPreview(key); };
		card->onProgram = [this, key]() { SendToProgram(key); };
		card->onLayer = [this, key](int layer) { ToggleLayer(key, layer); };
		card->onContextMenu = [this, key](const QPoint &pos) { ShowCardMenu(key, pos); };
		if (key.startsWith("f:")) {
			const QString path = key.mid(2);
			auto it = thumbCache.find(path);
			if (it != thumbCache.end())
				card->SetThumbnail(it->second);
			else
				thumbs->Request(path);
		}
		flow->addWidget(card);
		cards.push_back(card);
	};

	/* Entradas primero (como en vMix) */
	{
		FlowLayout *flow = addSection("inputs", T("Media.InputsRoot"), QString());
		for (const std::string &uuid : inputs) {
			OBSSourceAutoRelease src = obs_get_source_by_uuid(uuid.c_str());
			const QString title = src ? QString::fromUtf8(obs_source_get_name(src)) : T("Media.InputMissing");
			addCard(flow, "i:" + QString::fromStdString(uuid), title, TypeTagForSource(src));
		}
	}

	/* Carpetas */
	for (const QString &folder : folders) {
		const QFileInfo fi(folder);
		const QDir dir(folder);
		const bool exists = dir.exists();
		FlowLayout *flow = addSection("d:" + folder, fi.fileName() + (exists ? QString() : " " + T("Media.Missing")),
					      folder);
		if (!exists)
			continue;
		watcher->addPath(folder);
		const QFileInfoList entries =
			dir.entryInfoList(SupportedFilters(), QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
		bool hasImages = false;
		for (const QFileInfo &e : entries)
			hasImages = hasImages || FileType(e.filePath()) == "image";
		if (hasImages && obs_source_get_display_name("slideshow"))
			addCard(flow, "d:" + folder, T("Media.Slideshow") + ": " + fi.fileName(), T("Tag.Slideshow"));
		for (const QFileInfo &e : entries)
			addCard(flow, "f:" + e.filePath(), e.fileName(), TypeTagForFile(e.filePath()));
	}

	/* Archivos sueltos */
	if (!files.empty()) {
		FlowLayout *flow = addSection("files", T("Media.FilesRoot"), QString());
		for (const QString &f : files)
			addCard(flow, "f:" + f, QFileInfo(f).fileName(), TypeTagForFile(f));
	}

	if (inputs.empty() && folders.empty() && files.empty()) {
		auto *hint = new QLabel(T("Media.EmptyHint"), cardsHost);
		hint->setWordWrap(true);
		hint->setAlignment(Qt::AlignCenter);
		hint->setStyleSheet("color:#9aa0aa;padding:16px;");
		cardsLayout->addWidget(hint);
	}
	cardsLayout->addStretch(1);

	/* Si el elemento seleccionado desapareció, limpiar la selección. */
	bool found = false;
	for (MediaCard *c : cards)
		found = found || c->Key() == selectedKey;
	if (!found && !selectedKey.isEmpty()) {
		selectedKey.clear();
		if (editor && editor->isVisible())
			editor->close();
	}
	RefreshCardStates();
	UpdateControls();
}

void MediaDock::RefreshCardStates()
{
	if (shuttingDown || cards.empty())
		return;
	LiveEngine &engine = LiveEngine::Get();
	const bool studio = obs_frontend_preview_program_mode_active();
	OBSSourceAutoRelease program = obs_frontend_get_current_scene();
	obs_source_t *programContent = engine.ManagedContent(program);
	obs_source_t *previewContent = nullptr;
	if (studio) {
		OBSSourceAutoRelease prev = obs_frontend_get_current_preview_scene();
		previewContent = engine.ManagedContent(prev);
	}

	for (MediaCard *card : cards) {
		MediaCard::State s;
		s.selected = card->Key() == selectedKey;
		s.studio = studio;
		OBSSourceAutoRelease src = SourceForKey(card->Key(), false);
		if (src) {
			s.program = programContent == src.Get() || program.Get() == src.Get();
			s.preview = studio && !s.program && previewContent == src.Get();
			for (int i = 0; i < kLayerCount; i++) {
				s.layerLoaded[(size_t)i] = engine.LayerContent(i) == src.Get();
				s.layerVisible[(size_t)i] = s.layerLoaded[(size_t)i] && engine.LayerVisible(i);
			}
		}
		card->SetState(s);
	}
}

void MediaDock::RefreshLayerPanel()
{
	LiveEngine &engine = LiveEngine::Get();
	for (int i = 0; i < kLayerCount; i++) {
		LayerRow &row = layerRows[(size_t)i];
		const bool visible = engine.LayerVisible(i);
		const QString name = engine.LayerContentName(i);
		row.toggle->setChecked(visible);
		row.toggle->setStyleSheet(visible ? "QPushButton{background-color:#e08a00;color:black;font-weight:bold;}"
						  : QString());
		row.toggle->setEnabled(!name.isEmpty() || visible);
		row.content->setText(name.isEmpty() ? T("Layer.Empty") : row.content->fontMetrics().elidedText(
										 name, Qt::ElideMiddle, 140));
		row.content->setToolTip(name);
		row.clear->setEnabled(!name.isEmpty());
	}
}

/* ------------------------------------------------------------------------ */
/* Selección y visor                                                         */

void MediaDock::SelectKey(const QString &key)
{
	if (key == selectedKey)
		return;
	selectedKey = key;
	UpdateEditor();
	RefreshCardStates();
	UpdateControls();
}

ItemTransform MediaDock::TransformFor(const QString &key) const
{
	auto it = transforms.find(key);
	return it != transforms.end() ? it->second : ItemTransform();
}

void MediaDock::OpenEditor(const QString &key)
{
	if (shuttingDown || key.isEmpty())
		return;
	if (key != selectedKey) {
		selectedKey = key;
		RefreshCardStates();
		UpdateControls();
	}
	if (!editor) {
		editor = new TransformEditor(this);
		editor->onChanged = [this](const ItemTransform &t, bool final) {
			if (selectedKey.isEmpty())
				return;
			transforms[selectedKey] = t;
			/* Si ya está al aire (Programa, Vista previa o capa) se ajusta en vivo. */
			OBSSourceAutoRelease src = SourceForKey(selectedKey, false);
			if (src)
				LiveEngine::Get().UpdateTransform(src, t);
			if (final)
				QTimer::singleShot(0, this, []() { obs_frontend_save(); });
		};
		editor->onClosed = [this]() {
			previewTemp = nullptr;
			previewTempKey.clear();
			UpdateControls();
		};
	}

	OBSSourceAutoRelease src = SourceForKey(key, false);
	bool activate = false;
	obs_source_t *target = src;
	/* Un video que no está al aire (o que ya terminó) mide 0x0 y el editor
	 * caería al tamaño del lienzo (16:9): se muestra una copia privada que sí
	 * carga y da el tamaño real del contenido. Los cambios se aplican igual a
	 * la fuente pública. */
	const bool needsTemp = src && (key.startsWith("f:") || key.startsWith("d:")) &&
			       (!obs_source_get_width(src) || !obs_source_get_height(src));
	if (!src || needsTemp) {
		/* Aún no existe en OBS: fuente privada temporal (sin audio en la salida). */
		if (previewTempKey != key) {
			editor->Detach();
			previewTemp = CreateTempPreviewSource(key);
			previewTempKey = previewTemp ? key : QString();
		}
		target = previewTemp;
		activate = true;
	} else {
		previewTemp = nullptr;
		previewTempKey.clear();
	}
	if (!target)
		return;
	QString title;
	for (MediaCard *c : cards)
		if (c->Key() == key)
			title = c->toolTip();
	editor->Open(target, TransformFor(key), activate, title);
	UpdateControls();
}

void MediaDock::UpdateEditor()
{
	if (editor && editor->isVisible() && !selectedKey.isEmpty())
		OpenEditor(selectedKey);
}

void MediaDock::UpdateControls()
{
	const bool has = !selectedKey.isEmpty();
	btnEdit->setEnabled(has);
	QString title = T("Media.SelectHint");
	for (MediaCard *c : cards)
		if (c->Key() == selectedKey)
			title = c->toolTip();
	itemTitle->setText(itemTitle->fontMetrics().elidedText(title, Qt::ElideMiddle, 250));
	itemTitle->setToolTip(title);

	QString type;
	if (selectedKey.startsWith("f:"))
		type = FileType(selectedKey.mid(2));
	OBSSourceAutoRelease ctl = has ? ControlSource() : nullptr;
	const bool media = ctl && (obs_source_get_output_flags(ctl) & OBS_SOURCE_CONTROLLABLE_MEDIA);
	btnPlay->setEnabled(media);
	btnRestart->setEnabled(media);
	const bool loopable = ctl && strcmp(obs_source_get_id(ctl), "ffmpeg_source") == 0;
	btnLoop->setEnabled(loopable);
	if (loopable) {
		OBSDataAutoRelease s = obs_source_get_settings(ctl);
		btnLoop->setChecked(obs_data_get_bool(s, "looping"));
	} else {
		btnLoop->setChecked(false);
	}
	btnPrevPage->setVisible(type == "pdf");
	btnNextPage->setVisible(type == "pdf");
}

/* ------------------------------------------------------------------------ */
/* Agregar / quitar                                                          */

void MediaDock::AddFolder()
{
	const QString dir = QFileDialog::getExistingDirectory(this, T("Media.AddFolder"));
	if (dir.isEmpty())
		return;
	if (std::find(folders.begin(), folders.end(), dir) == folders.end())
		folders.push_back(dir);
	RebuildCards();
}

void MediaDock::AddFiles()
{
	const QString filter = T("Media.FilterAll") + " (" + SupportedFilters().join(' ') + ")";
	const QStringList list = QFileDialog::getOpenFileNames(this, T("Media.AddFiles"), QString(), filter);
	QString last;
	for (const QString &f : list) {
		if (std::find(files.begin(), files.end(), f) == files.end())
			files.push_back(f);
		last = f;
	}
	if (!list.isEmpty()) {
		RebuildCards();
		SelectKey("f:" + last);
		/* Un solo archivo: abrir el editor para ubicarlo y darle tamaño. */
		if (list.size() == 1)
			OpenEditor("f:" + last);
	}
}

void MediaDock::ShowAddInputMenu()
{
	QMenu menu(this);
	for (const InputType &type : kInputTypes) {
		const char *id = FirstAvailable(type.ids);
		if (!id)
			continue;
		const QString label = T(type.label);
		connect(menu.addAction(label), &QAction::triggered, this, [this, id, label]() { CreateInput(id, label); });
	}
	if (!obs_source_get_display_name("ndi_source"))
		menu.addAction(T("Input.NdiMissing"))->setEnabled(false);
	menu.addSeparator();
	connect(menu.addAction(T("Input.Existing")), &QAction::triggered, this, [this]() { AddExistingSource(); });
	menu.exec(QCursor::pos());
}

void MediaDock::CreateInput(const char *id, const QString &label)
{
	bool ok = false;
	const QString name = QInputDialog::getText(this, T("Media.AddInput"), T("Input.Name"), QLineEdit::Normal,
						   UniqueSourceName(label), &ok)
				     .trimmed();
	if (!ok || name.isEmpty())
		return;
	OBSSourceAutoRelease src = obs_source_create(id, UniqueSourceName(name).toUtf8().constData(), nullptr, nullptr);
	if (!src) {
		QMessageBox::warning(this, T("Media.AddInput"), T("Input.CreateFailed"));
		return;
	}
	/* Sin esto OBS destruye la entrada en cuanto se suelta la referencia. */
	LiveEngine::Get().HoldInLibrary(src);
	const std::string uuid = obs_source_get_uuid(src);
	inputs.push_back(uuid);
	RebuildCards();
	SelectKey("i:" + QString::fromStdString(uuid));
	obs_frontend_open_source_properties(src);
	OpenEditor("i:" + QString::fromStdString(uuid));
}

void MediaDock::AddExistingSource()
{
	struct Ctx {
		QStringList names;
	} ctx;
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			if (obs_source_get_type(src) == OBS_SOURCE_TYPE_INPUT)
				static_cast<Ctx *>(param)->names << QString::fromUtf8(obs_source_get_name(src));
			return true;
		},
		&ctx);
	obs_enum_scenes(
		[](void *param, obs_source_t *src) {
			if (!LiveEngine::IsLibraryScene(src) && !LiveEngine::Get().IsManagedScene(src))
				static_cast<Ctx *>(param)->names << QString::fromUtf8(obs_source_get_name(src));
			return true;
		},
		&ctx);
	ctx.names.removeDuplicates();
	ctx.names.sort(Qt::CaseInsensitive);
	if (ctx.names.isEmpty())
		return;
	bool ok = false;
	const QString name = QInputDialog::getItem(this, T("Input.Existing"), T("Input.Pick"), ctx.names, 0, false, &ok);
	if (!ok)
		return;
	OBSSourceAutoRelease src = obs_get_source_by_name(name.toUtf8().constData());
	if (!src)
		return;
	const std::string uuid = obs_source_get_uuid(src);
	if (std::find(inputs.begin(), inputs.end(), uuid) == inputs.end())
		inputs.push_back(uuid);
	RebuildCards();
	SelectKey("i:" + QString::fromStdString(uuid));
	OpenEditor("i:" + QString::fromStdString(uuid));
}

void MediaDock::RemoveKey(const QString &key)
{
	if (key.startsWith("i:")) {
		inputs.erase(std::remove(inputs.begin(), inputs.end(), ToStd(key.mid(2))), inputs.end());
		OBSSourceAutoRelease src = obs_get_source_by_uuid(ToStd(key.mid(2)).c_str());
		if (src)
			LiveEngine::Get().ReleaseFromLibrary(src);
	} else if (key.startsWith("f:")) {
		auto it = std::find(files.begin(), files.end(), key.mid(2));
		if (it == files.end()) {
			QMessageBox::information(this, T("Media.Remove"), T("Media.RemoveInFolder"));
			return;
		}
		files.erase(it);
	} else if (key.startsWith("d:")) {
		folders.erase(std::remove(folders.begin(), folders.end(), key.mid(2)), folders.end());
	}
	transforms.erase(key);
	RebuildCards();
}

void MediaDock::ShowCardMenu(const QString &key, const QPoint &globalPos)
{
	QMenu menu(this);
	connect(menu.addAction(T("Media.Edit")), &QAction::triggered, this, [this, key]() { OpenEditor(key); });
	menu.addSeparator();
	const bool studio = obs_frontend_preview_program_mode_active();
	QAction *toPrev = menu.addAction(T("Media.ToPreview"));
	toPrev->setEnabled(studio);
	connect(toPrev, &QAction::triggered, this, [this, key]() { SendToPreview(key); });
	connect(menu.addAction(T("Media.ToProgram")), &QAction::triggered, this, [this, key]() { SendToProgram(key); });
	QMenu *layerMenu = menu.addMenu(T("Media.LoadLayer"));
	for (int i = 0; i < kLayerCount; i++)
		connect(layerMenu->addAction(QString("%1 %2").arg(T("Layer")).arg(i + 1)), &QAction::triggered, this,
			[this, key, i]() { LoadToLayer(key, i, false); });
	menu.addSeparator();
	connect(menu.addAction(T("Media.Properties")), &QAction::triggered, this, [this, key]() {
		OBSSourceAutoRelease src = SourceForKey(key, true);
		if (src)
			obs_frontend_open_source_properties(src);
	});
	connect(menu.addAction(T("Menu.Filters")), &QAction::triggered, this, [this, key]() {
		OBSSourceAutoRelease src = SourceForKey(key, true);
		if (src)
			obs_frontend_open_source_filters(src);
	});
	menu.addSeparator();
	if (key.startsWith("d:")) {
		connect(menu.addAction(T("Media.RemoveFolder")), &QAction::triggered, this,
			[this, key]() { RemoveKey(key); });
	} else {
		QAction *rm = menu.addAction(T("Media.Remove"));
		rm->setEnabled(key.startsWith("i:") ||
			       std::find(files.begin(), files.end(), key.mid(2)) != files.end());
		connect(rm, &QAction::triggered, this, [this, key]() { RemoveKey(key); });
	}
	menu.addSeparator();
	connect(menu.addAction(T("Media.Refresh")), &QAction::triggered, this, [this]() { RebuildCards(); });
	menu.exec(globalPos);
}

/* ------------------------------------------------------------------------ */
/* Fuentes                                                                   */

/* Tamaño en puntos de la página de un PDF (primer /MediaBox o /CropBox), sin
 * bibliotecas: así la fuente toma la forma de la página y no la del lienzo.
 * Si los objetos están comprimidos (ObjStm) se descomprimen. Vacío si no se
 * encuentra. */
static QSizeF PdfPageSize(const QString &path)
{
	static QHash<QString, QPair<QDateTime, QSizeF>> cache;
	const QDateTime modified = QFileInfo(path).lastModified();
	auto cached = cache.constFind(path);
	if (cached != cache.constEnd() && cached->first == modified)
		return cached->second;

	static const QRegularExpression boxRe(
		R"(/(?:MediaBox|CropBox)\s*\[\s*(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s*\])");
	static const QRegularExpression rotateRe(R"(/Rotate\s+(-?\d+))");
	QSizeF result;
	auto scan = [&](const QByteArray &data) {
		const QString text = QString::fromLatin1(data);
		const QRegularExpressionMatch m = boxRe.match(text);
		if (!m.hasMatch())
			return false;
		const double w = std::abs(m.captured(3).toDouble() - m.captured(1).toDouble());
		const double h = std::abs(m.captured(4).toDouble() - m.captured(2).toDouble());
		if (w <= 0.0 || h <= 0.0)
			return false;
		/* /Rotate del mismo objeto página (cerca del /MediaBox). */
		const qsizetype from = std::max<qsizetype>(0, m.capturedStart() - 2000);
		const QRegularExpressionMatch r = rotateRe.match(text.mid(from, 4000 + m.capturedLength()));
		const int rot = r.hasMatch() ? ((r.captured(1).toInt() % 360) + 360) % 360 : 0;
		result = (rot == 90 || rot == 270) ? QSizeF(h, w) : QSizeF(w, h);
		return true;
	};

	QFile file(path);
	if (file.open(QIODevice::ReadOnly)) {
		/* Por bloques: en PDF grandes la primera página suele estar al inicio. */
		const qint64 chunk = qint64(4) << 20;
		QByteArray tail;
		bool found = false;
		while (!found && !file.atEnd()) {
			const QByteArray block = tail + file.read(chunk);
			found = scan(block);
			tail = block.right(512);
		}
		if (!found && file.size() <= (qint64(128) << 20) && file.seek(0)) {
			const QByteArray all = file.readAll();
			qsizetype pos = 0;
			while (!found && (pos = all.indexOf("/ObjStm", pos)) >= 0) {
				qsizetype start = all.indexOf("stream", pos);
				if (start < 0)
					break;
				start += 6;
				if (start < all.size() && all[start] == '\r')
					start++;
				if (start < all.size() && all[start] == '\n')
					start++;
				const qsizetype end = all.indexOf("endstream", start);
				if (end < 0)
					break;
				/* qUncompress espera 4 bytes con el tamaño (solo una pista). */
				const quint32 hint = (quint32)std::min<qsizetype>((end - start) * 8, qsizetype(64) << 20);
				QByteArray z;
				z.append((char)(hint >> 24)).append((char)(hint >> 16)).append((char)(hint >> 8)).append((char)hint);
				z.append(all.constData() + start, end - start);
				const QByteArray inflated = qUncompress(z);
				found = !inflated.isEmpty() && scan(inflated);
				pos = end;
			}
		}
	}
	cache.insert(path, qMakePair(modified, result));
	return result;
}

/* Tamaño de la fuente de navegador de un PDF: la página ajustada al lienzo. */
static void PdfSourceSize(const QString &path, uint32_t cw, uint32_t ch, uint32_t &w, uint32_t &h)
{
	w = cw;
	h = ch;
	const QSizeF page = PdfPageSize(path);
	if (page.isEmpty() || !cw || !ch)
		return;
	const double a = page.width() / page.height();
	if (a >= (double)cw / ch)
		h = (uint32_t)std::max(2L, std::lround(cw / a));
	else
		w = (uint32_t)std::max(2L, std::lround(ch * a));
}

bool MediaDock::BuildFileSource(const QString &path, const char *&id, obs_data_t *s)
{
	const QString type = FileType(path);
	uint32_t cw, ch;
	CanvasSize(cw, ch);
	const std::string p = ToStd(path);
	id = nullptr;
	if (type == "image") {
		id = "image_source";
		obs_data_set_string(s, "file", p.c_str());
	} else if (type == "video" || type == "audio") {
		id = "ffmpeg_source";
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_string(s, "local_file", p.c_str());
		obs_data_set_bool(s, "looping", false);
		obs_data_set_bool(s, "restart_on_activate", true);
		obs_data_set_bool(s, "close_when_inactive", false);
		obs_data_set_bool(s, "hw_decode", true);
	} else if (type == "html") {
		id = "browser_source";
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_string(s, "local_file", p.c_str());
		obs_data_set_int(s, "width", cw);
		obs_data_set_int(s, "height", ch);
	} else if (type == "pdf") {
		id = "browser_source";
		char *viewer = obs_module_file("pdf/viewer.html");
		if (!viewer)
			return false;
		const QString url = AbsoluteUrl(QString::fromUtf8(viewer)) + "?file=" +
				    QString::fromUtf8(QUrl::toPercentEncoding(AbsoluteUrl(path))) + "&bg=black";
		bfree(viewer);
		obs_data_set_bool(s, "is_local_file", false);
		obs_data_set_string(s, "url", ToStd(url).c_str());
		uint32_t pw, ph;
		PdfSourceSize(path, cw, ch, pw, ph);
		obs_data_set_int(s, "width", pw);
		obs_data_set_int(s, "height", ph);
		/* Se apaga cuando no está visible (ahorra recursos); el visor
		 * recuerda la página. */
		obs_data_set_bool(s, "shutdown", true);
		obs_data_set_bool(s, "restart_when_active", false);
		obs_data_set_bool(s, "reroute_audio", false);
	}
	return id && obs_source_get_display_name(id);
}

/* PDF creados antes (al tamaño del lienzo): llevarlos a la forma de la página. */
void MediaDock::FitPdfSource(const QString &path, obs_source_t *src)
{
	uint32_t cw, ch, pw, ph;
	CanvasSize(cw, ch);
	PdfSourceSize(path, cw, ch, pw, ph);
	OBSDataAutoRelease cur = obs_source_get_settings(src);
	if ((uint32_t)obs_data_get_int(cur, "width") == pw && (uint32_t)obs_data_get_int(cur, "height") == ph)
		return;
	OBSDataAutoRelease upd = obs_data_create();
	obs_data_set_int(upd, "width", pw);
	obs_data_set_int(upd, "height", ph);
	obs_source_update(src, upd);
	blog(LOG_INFO, "[tools-scenes] PDF ajustado a la forma de la página: %ux%u", pw, ph);
}

obs_source_t *MediaDock::SourceForFile(const QString &path, bool create)
{
	auto it = fileSources.find(path);
	if (it != fileSources.end()) {
		obs_source_t *src = obs_get_source_by_uuid(it->second.c_str());
		if (src) {
			if (FileType(path) == "pdf")
				FitPdfSource(path, src);
			return src;
		}
		fileSources.erase(it);
	}
	if (!create)
		return nullptr;
	if (!QFileInfo::exists(path)) {
		QMessageBox::warning(this, T("Media.FileMissingTitle"), T("Media.FileMissing").arg(path));
		return nullptr;
	}
	const char *id = nullptr;
	OBSDataAutoRelease s = obs_data_create();
	if (!BuildFileSource(path, id, s)) {
		QMessageBox::warning(this, T("Media.FileMissingTitle"), T("Media.TypeUnavailable"));
		return nullptr;
	}
	const QString name = UniqueSourceName("SP: " + QFileInfo(path).fileName());
	obs_source_t *src = obs_source_create(id, name.toUtf8().constData(), s, nullptr);
	if (src) {
		LiveEngine::Get().HoldInLibrary(src); /* si no, OBS la destruye al dejar de usarse */
		fileSources[path] = obs_source_get_uuid(src);
		/* La fuente pública reemplaza a la temporal del visor. */
		if (previewTempKey == "f:" + path)
			QTimer::singleShot(0, this, [this]() { UpdateEditor(); });
	}
	return src;
}

static void FillSlideshowSettings(const QString &folder, obs_data_t *s)
{
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	QDir dir(folder);
	QStringList filters;
	for (const QString &e : kImageExt)
		filters << "*." + e << "*." + e.toUpper();
	for (const QFileInfo &e : dir.entryInfoList(filters, QDir::Files, QDir::Name | QDir::IgnoreCase)) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "value", ToStd(e.filePath()).c_str());
		obs_data_array_push_back(arr, item);
	}
	obs_data_set_array(s, "files", arr);
	obs_data_set_bool(s, "loop", true);
	obs_data_set_string(s, "playback_behavior", "always_play");
}

obs_source_t *MediaDock::SourceForFolderSlideshow(const QString &folder, bool create)
{
	const QString key = "slideshow:" + folder;
	auto it = fileSources.find(key);
	obs_source_t *src = nullptr;
	if (it != fileSources.end())
		src = obs_get_source_by_uuid(it->second.c_str());
	if (src || !create)
		return src;
	OBSDataAutoRelease s = obs_data_create();
	FillSlideshowSettings(folder, s);
	const QString name = UniqueSourceName("SP: " + QFileInfo(folder).fileName() + " (" + T("Media.Slideshow") + ")");
	src = obs_source_create("slideshow", name.toUtf8().constData(), s, nullptr);
	if (src) {
		LiveEngine::Get().HoldInLibrary(src);
		fileSources[key] = obs_source_get_uuid(src);
		if (previewTempKey == "d:" + folder)
			QTimer::singleShot(0, this, [this]() { UpdateEditor(); });
	}
	return src;
}

obs_source_t *MediaDock::SourceForKey(const QString &key, bool create)
{
	if (key.startsWith("f:"))
		return SourceForFile(key.mid(2), create);
	if (key.startsWith("i:"))
		return obs_get_source_by_uuid(ToStd(key.mid(2)).c_str());
	if (key.startsWith("d:"))
		return SourceForFolderSlideshow(key.mid(2), create);
	return nullptr;
}

obs_source_t *MediaDock::CreateTempPreviewSource(const QString &key)
{
	OBSDataAutoRelease s = obs_data_create();
	const char *id = nullptr;
	if (key.startsWith("f:")) {
		if (!BuildFileSource(key.mid(2), id, s))
			return nullptr;
	} else if (key.startsWith("d:")) {
		id = "slideshow";
		FillSlideshowSettings(key.mid(2), s);
	} else {
		return nullptr;
	}
	return obs_source_create_private(id, "SP vista previa", s);
}

obs_source_t *MediaDock::ControlSource()
{
	obs_source_t *src = SourceForKey(selectedKey, false);
	if (src)
		return src;
	return previewTemp ? obs_source_get_ref(previewTemp) : nullptr;
}

/* ------------------------------------------------------------------------ */
/* Acciones                                                                  */

/* Fuente que se ve ahora en Programa / Vista previa (identidad). */
static obs_source_t *ContentOf(bool preview)
{
	LiveEngine &engine = LiveEngine::Get();
	if (preview) {
		if (!obs_frontend_preview_program_mode_active())
			return nullptr;
		OBSSourceAutoRelease scene = obs_frontend_get_current_preview_scene();
		return engine.ManagedContent(scene);
	}
	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	obs_source_t *content = engine.ManagedContent(scene);
	return content ? content : scene.Get();
}

void MediaDock::SendToPreview(const QString &key)
{
	OBSSourceAutoRelease existing = SourceForKey(key, false);
	/* Conmutador: si ya está en Vista previa, se quita. */
	if (existing && ContentOf(true) == existing.Get()) {
		LiveEngine::Get().ClearPreview();
		RefreshCardStates();
		return;
	}
	OBSSourceAutoRelease src = SourceForKey(key, true);
	if (src)
		LiveEngine::Get().SendToPreview(src, TransformFor(key));
	RefreshCardStates();
	UpdateControls();
}

void MediaDock::SendToProgram(const QString &key)
{
	OBSSourceAutoRelease existing = SourceForKey(key, false);
	/* Conmutador: si ya está al aire, se quita (Programa pasa a vacío). */
	if (existing && ContentOf(false) == existing.Get()) {
		LiveEngine::Get().ClearProgram();
		RefreshCardStates();
		return;
	}
	OBSSourceAutoRelease src = SourceForKey(key, true);
	if (src)
		LiveEngine::Get().SendToProgram(src, TransformFor(key));
	RefreshCardStates();
	UpdateControls();
}

void MediaDock::LoadToLayer(const QString &key, int layer, bool show)
{
	OBSSourceAutoRelease src = SourceForKey(key, true);
	if (!src)
		return;
	LiveEngine &engine = LiveEngine::Get();
	engine.LoadLayer(layer, src, TransformFor(key));
	if (show)
		engine.SetLayerVisible(layer, true);
	RefreshLayerPanel();
	RefreshCardStates();
	UpdateControls();
}

void MediaDock::ToggleLayer(const QString &key, int layer)
{
	/* Como en vMix: si el elemento ya está visible en esa capa, se quita;
	 * si no, se carga en la capa y se muestra. */
	LiveEngine &engine = LiveEngine::Get();
	OBSSourceAutoRelease existing = SourceForKey(key, false);
	if (existing && engine.LayerContent(layer) == existing.Get() && engine.LayerVisible(layer)) {
		engine.SetLayerVisible(layer, false);
		RefreshLayerPanel();
		RefreshCardStates();
		return;
	}
	LoadToLayer(key, layer, true);
}

void MediaDock::MediaPlayPause()
{
	OBSSourceAutoRelease src = ControlSource();
	if (!src)
		return;
	const obs_media_state state = obs_source_media_get_state(src);
	if (state == OBS_MEDIA_STATE_PLAYING)
		obs_source_media_play_pause(src, true);
	else if (state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_NONE)
		obs_source_media_restart(src);
	else
		obs_source_media_play_pause(src, false);
}

void MediaDock::MediaRestart()
{
	OBSSourceAutoRelease src = ControlSource();
	if (src)
		obs_source_media_restart(src);
}

void MediaDock::ToggleLoop()
{
	OBSSourceAutoRelease src = ControlSource();
	if (!src)
		return;
	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_bool(s, "looping", btnLoop->isChecked());
	obs_source_update(src, s);
}

void MediaDock::PdfPage(int delta)
{
	OBSSourceAutoRelease src = ControlSource();
	if (!src)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(src);
	if (!ph)
		return;
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "eventName", "scenePrevioPage");
	const std::string json = "{\"delta\":" + std::to_string(delta) + "}";
	calldata_set_string(&cd, "jsonString", json.c_str());
	proc_handler_call(ph, "javascript_event", &cd);
	calldata_free(&cd);
}

/* ------------------------------------------------------------------------ */
/* Persistencia                                                              */

void MediaDock::Save(obs_data_t *data) const
{
	auto writeStrings = [&](const char *key, const char *field, const auto &list, auto toStr) {
		OBSDataArrayAutoRelease arr = obs_data_array_create();
		for (const auto &v : list) {
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(item, field, toStr(v).c_str());
			obs_data_array_push_back(arr, item);
		}
		obs_data_set_array(data, key, arr);
	};
	writeStrings("folders", "path", folders, [](const QString &s) { return ToStd(s); });
	writeStrings("files", "path", files, [](const QString &s) { return ToStd(s); });
	writeStrings("inputs", "uuid", inputs, [](const std::string &s) { return s; });

	OBSDataArrayAutoRelease fs = obs_data_array_create();
	for (const auto &entry : fileSources) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "path", ToStd(entry.first).c_str());
		obs_data_set_string(item, "uuid", entry.second.c_str());
		obs_data_array_push_back(fs, item);
	}
	obs_data_set_array(data, "file_sources", fs);

	OBSDataArrayAutoRelease tr = obs_data_array_create();
	for (const auto &entry : transforms) {
		if (!entry.second.custom && !entry.second.HasExtras())
			continue;
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "key", ToStd(entry.first).c_str());
		entry.second.Save(item);
		obs_data_array_push_back(tr, item);
	}
	obs_data_set_array(data, "transforms", tr);
	obs_data_set_int(data, "card_width", cardWidth);
}

void MediaDock::Load(obs_data_t *data)
{
	folders.clear();
	files.clear();
	inputs.clear();
	fileSources.clear();
	transforms.clear();
	if (data) {
		auto readArray = [&](const char *key, auto fn) {
			OBSDataArrayAutoRelease arr = obs_data_get_array(data, key);
			const size_t n = obs_data_array_count(arr);
			for (size_t i = 0; i < n; i++) {
				OBSDataAutoRelease item = obs_data_array_item(arr, i);
				fn(item.Get());
			}
		};
		readArray("folders", [&](obs_data_t *item) {
			folders.push_back(QString::fromUtf8(obs_data_get_string(item, "path")));
		});
		readArray("files", [&](obs_data_t *item) {
			files.push_back(QString::fromUtf8(obs_data_get_string(item, "path")));
		});
		readArray("inputs", [&](obs_data_t *item) { inputs.push_back(obs_data_get_string(item, "uuid")); });
		readArray("file_sources", [&](obs_data_t *item) {
			fileSources[QString::fromUtf8(obs_data_get_string(item, "path"))] =
				obs_data_get_string(item, "uuid");
		});
		readArray("transforms", [&](obs_data_t *item) {
			transforms[QString::fromUtf8(obs_data_get_string(item, "key"))] = ItemTransform::Load(item);
		});
		/* Asegurar que las fuentes propias estén en la biblioteca (colecciones
		 * creadas con versiones anteriores). */
		for (const auto &entry : fileSources) {
			OBSSourceAutoRelease src = obs_get_source_by_uuid(entry.second.c_str());
			if (src)
				LiveEngine::Get().HoldInLibrary(src);
		}
		const int w = (int)obs_data_get_int(data, "card_width");
		if (w >= 130 && w <= 360) {
			cardWidth = w;
			cardSize->setValue(w);
		}
	}
	RebuildCards();
}

/* ------------------------------------------------------------------------ */

#ifdef SCENE_PREVIO_DEBUG
/* Prueba automática (solo compilación de diagnóstico). */
void MediaDock::SelfTest(const char *dirPath)
{
	const QString dir = QString::fromUtf8(dirPath);
	QStringList imgs;
	const QColor colors[] = {QColor(200, 40, 40), QColor(40, 160, 60), QColor(40, 80, 200)};
	for (int i = 0; i < 3; i++) {
		const QString path = dir + QString("/imagen %1.png").arg(QChar('A' + i));
		QImage img(640, 360, QImage::Format_RGB32);
		img.fill(colors[i]);
		QPainter p(&img);
		QFont f;
		f.setPixelSize(200);
		f.setBold(true);
		p.setFont(f);
		p.setPen(Qt::white);
		p.drawText(img.rect(), Qt::AlignCenter, QString(QChar('A' + i)));
		p.end();
		img.save(path);
		imgs << path;
	}
	files.clear();
	for (const QString &f : imgs)
		files.push_back(f);

	/* Entrada como la crea el botón "+ Entrada" (sin el diálogo de nombre). */
	OBSDataAutoRelease cs = obs_data_create();
	obs_data_set_int(cs, "color", 0xFF00A5FF);
	obs_data_set_int(cs, "width", 800);
	obs_data_set_int(cs, "height", 450);
	OBSSourceAutoRelease input = obs_source_create("color_source_v3", UniqueSourceName("Entrada prueba").toUtf8().constData(), cs, nullptr);
	LiveEngine::Get().HoldInLibrary(input);
	const QString inputKey = "i:" + QString::fromUtf8(obs_source_get_uuid(input));
	inputs.push_back(ToStd(inputKey.mid(2)));
	RebuildCards();

	auto dump = [this](const char *what) {
		QString line;
		for (MediaCard *c : cards) {
			const MediaCard::State &st = c->GetState();
			QString layers;
			for (int i = 0; i < kLayerCount; i++)
				layers += st.layerVisible[(size_t)i] ? QString::number(i + 1) : "-";
			line += QString(" [%1 P%2 V%3 L%4]").arg(c->toolTip().left(12)).arg(st.program).arg(st.preview).arg(layers);
		}
		blog(LOG_INFO, "[selftest] %s:%s", what, line.toUtf8().constData());
	};
	auto step = [this](int ms, std::function<void()> fn) { QTimer::singleShot(ms, this, fn); };
	step(800, [this, imgs, dump]() { SendToPreview("f:" + imgs[0]); dump("1 vista A"); });
	step(2500, [this, imgs, dump]() { SendToPreview("f:" + imgs[1]); dump("2 vista B"); });
	step(4200, [this, imgs, dump]() { SendToPreview("f:" + imgs[2]); dump("3 vista C"); });
	step(6000, [this, inputKey, dump]() {
		OBSSourceAutoRelease alive = SourceForKey(inputKey, false);
		blog(LOG_INFO, "[selftest] 4 entrada viva=%d", alive ? 1 : 0);
		SendToProgram(inputKey);
		dump("4 entrada al aire");
	});
	step(8000, [this, inputKey, dump]() { ToggleLayer(inputKey, 1); dump("5 entrada en capa 2"); });
	step(9500, [this, inputKey]() {
		OpenEditor(inputKey);
		blog(LOG_INFO, "[selftest] 6 editor de la entrada: fuente=%d", editor && editor->Source() ? 1 : 0);
	});
	step(11500, [this, inputKey, dump]() {
		editor->close();
		SendToProgram(inputKey);
		dump("7 Aire otra vez (quitar)");
	});
	step(14000, [this, inputKey, dump]() {
		ToggleLayer(inputKey, 1);
		SendToPreview("f:" + QString(files[2]));
		dump("8 capa 2 fuera y Vista C otra vez (quitar)");
	});
}
#endif
