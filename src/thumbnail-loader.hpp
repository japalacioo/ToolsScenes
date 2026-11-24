/*
 * Tools Scenes - miniaturas de archivos en un hilo aparte (nunca bloquea la
 * interfaz ni el hilo gráfico de OBS). Imágenes con QImageReader; en Windows,
 * videos/PDF/otros con las miniaturas del Explorador (IShellItemImageFactory).
 */
#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class ThumbnailLoader {
public:
	using Callback = std::function<void(const QString &path, const QImage &image)>;

	/* `context` debe vivir más que este objeto: el callback se ejecuta en
	 * su hilo (UI) mediante una llamada encolada. */
	ThumbnailLoader(QObject *context, Callback callback);
	~ThumbnailLoader();

	void Request(const QString &path);
	void Clear();

private:
	void Run();

	QObject *context;
	Callback callback;
	std::thread worker;
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<QString> queue;
	bool stop = false;
};
