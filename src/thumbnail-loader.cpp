/*
 * Tools Scenes - miniaturas en segundo plano (ver thumbnail-loader.hpp).
 */
#include "thumbnail-loader.hpp"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#endif

namespace {

constexpr int kThumbW = 320;
constexpr int kThumbH = 180;

QImage ReadImage(const QString &path)
{
	QImageReader reader(path);
	reader.setAutoTransform(true);
	const QSize size = reader.size();
	if (size.isValid() && (size.width() > kThumbW || size.height() > kThumbH))
		reader.setScaledSize(size.scaled(kThumbW, kThumbH, Qt::KeepAspectRatio));
	QImage img = reader.read();
	if (!img.isNull() && (img.width() > kThumbW || img.height() > kThumbH))
		img = img.scaled(kThumbW, kThumbH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	return img;
}

#ifdef _WIN32
QImage ShellThumbnail(const QString &path)
{
	const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
	IShellItemImageFactory *factory = nullptr;
	if (FAILED(SHCreateItemFromParsingName(native.c_str(), nullptr, IID_PPV_ARGS(&factory))) || !factory)
		return QImage();
	HBITMAP bmp = nullptr;
	SIZE size = {kThumbW, kThumbH};
	const HRESULT hr = factory->GetImage(size, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bmp);
	factory->Release();
	if (FAILED(hr) || !bmp)
		return QImage();
	QImage img = QImage::fromHBITMAP(bmp);
	DeleteObject(bmp);
	if (!img.isNull() && (img.width() > kThumbW || img.height() > kThumbH))
		img = img.scaled(kThumbW, kThumbH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	return img;
}
#endif

} // namespace

ThumbnailLoader::ThumbnailLoader(QObject *context_, Callback callback_)
	: context(context_),
	  callback(std::move(callback_))
{
	worker = std::thread([this]() { Run(); });
}

ThumbnailLoader::~ThumbnailLoader()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		stop = true;
		queue.clear();
	}
	cv.notify_all();
	if (worker.joinable())
		worker.join();
}

void ThumbnailLoader::Request(const QString &path)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		queue.push_back(path);
	}
	cv.notify_one();
}

void ThumbnailLoader::Clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	queue.clear();
}

void ThumbnailLoader::Run()
{
#ifdef _WIN32
	const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
	for (;;) {
		QString path;
		{
			std::unique_lock<std::mutex> lock(mutex);
			cv.wait(lock, [this]() { return stop || !queue.empty(); });
			if (stop)
				break;
			path = queue.front();
			queue.pop_front();
		}

		QImage img;
		const QString ext = QFileInfo(path).suffix().toLower();
		static const QStringList imageExt = {"png", "jpg", "jpeg", "bmp", "gif", "webp", "tga"};
		if (imageExt.contains(ext))
			img = ReadImage(path);
#ifdef _WIN32
		if (img.isNull())
			img = ShellThumbnail(path);
#endif
		if (img.isNull())
			continue;

		std::lock_guard<std::mutex> lock(mutex);
		if (stop)
			break;
		/* Se encola mientras se tiene el mutex: el destructor no puede
		 * completar mientras tanto, y Qt descarta la llamada si el contexto
		 * se destruye antes de ejecutarla. */
		Callback cb = callback;
		QMetaObject::invokeMethod(
			context, [cb, path, img]() { cb(path, img); }, Qt::QueuedConnection);
	}
#ifdef _WIN32
	if (SUCCEEDED(com))
		CoUninitialize();
#endif
}
