#include "util/FileLogger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

namespace bsfchat {

namespace {

constexpr qint64 kMaxLogBytes = 5 * 1024 * 1024;
constexpr int kKeepGenerations = 3;

QMutex g_mutex;
QFile g_file;
QtMessageHandler g_previousHandler = nullptr;

QString resolveLogDirectory()
{
#ifdef Q_OS_MACOS
    // macOS convention — next to the updater's log, where users (and
    // Console.app) expect application logs to live.
    return QDir::homePath() + "/Library/Logs/"
           + QCoreApplication::applicationName();
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + "/logs";
#endif
}

QString logFilePath()
{
    return resolveLogDirectory() + "/bsfchat.log";
}

// bsfchat.log -> .1 -> .2 -> .3, oldest dropped. Caller holds g_mutex
// and has closed g_file.
void rotate()
{
    const QString base = logFilePath();
    QFile::remove(base + "." + QString::number(kKeepGenerations));
    for (int i = kKeepGenerations - 1; i >= 1; --i) {
        QFile::rename(base + "." + QString::number(i),
                      base + "." + QString::number(i + 1));
    }
    QFile::rename(base, base + ".1");
}

void openLogFile()
{
    QDir().mkpath(resolveLogDirectory());
    g_file.setFileName(logFilePath());
    g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}

void messageHandler(QtMsgType type, const QMessageLogContext& context,
                    const QString& message)
{
    static const char* levels[] = {"DBG", "WRN", "CRT", "FTL", "INF"};
    const char* level = (type >= 0 && type <= 4) ? levels[type] : "???";

    {
        QMutexLocker lock(&g_mutex);
        if (g_file.isOpen()) {
            if (g_file.size() >= kMaxLogBytes) {
                g_file.close();
                rotate();
                openLogFile();
            }
            QString line = QDateTime::currentDateTime()
                               .toString("yyyy-MM-dd HH:mm:ss.zzz");
            line += ' ';
            line += QLatin1String(level);
            if (context.category && qstrcmp(context.category, "default") != 0) {
                line += " [";
                line += QLatin1String(context.category);
                line += ']';
            }
            line += ' ';
            line += message;
            line += '\n';
            g_file.write(line.toUtf8());
            g_file.flush();
        }
    }

    if (g_previousHandler) {
        g_previousHandler(type, context, message);
    }
}

} // namespace

void installFileLogger()
{
    QMutexLocker lock(&g_mutex);
    if (g_file.isOpen()) return; // already installed
    openLogFile();
    g_previousHandler = qInstallMessageHandler(messageHandler);
}

QString logDirectory()
{
    return resolveLogDirectory();
}

} // namespace bsfchat
