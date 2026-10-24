#include "logger.h"

#include <QFile>
#include <QDateTime>
#include <QTextStream>
#include <QMutex>
#include <QDir>
#include <QDebug>

static QMutex g_logMutex;
static QFile g_logFile;

static const qint64 MAX_LOG_SIZE = 10 * 1024 * 1024;
static const int MAX_LOG_FILES = 5;

static void rotateLogs(const QString &baseName)
{
    QFile baseFile(baseName);

    if (baseFile.exists() && baseFile.size() >= MAX_LOG_SIZE)
    {
        QString oldFile = QString("%1_%2.txt")
        .arg(baseName.left(baseName.size()-4))
            .arg(MAX_LOG_FILES);

        if (QFile::exists(oldFile))
            QFile::remove(oldFile);

        for (int i = MAX_LOG_FILES - 1; i >= 1; --i)
        {
            QString src = QString("%1_%2.txt")
            .arg(baseName.left(baseName.size()-4))
                .arg(i);

            QString dst = QString("%1_%2.txt")
                              .arg(baseName.left(baseName.size()-4))
                              .arg(i+1);

            if (QFile::exists(src))
                QFile::rename(src, dst);
        }

        QString rotated =
            QString("%1_1.txt").arg(baseName.left(baseName.size()-4));

        baseFile.rename(rotated);
    }
}

static bool initLog()
{
    if (g_logFile.isOpen())
        return true;

    if (!QDir().mkpath("logs")) {
        qWarning() << "Cannot create log directory:" << QDir("logs").absolutePath();
        return false;
    }

    g_logFile.setFileName("logs/system_log.txt");
    if (!g_logFile.open(QIODevice::Append | QIODevice::Text)) {
        qWarning() << "Cannot open log file:" << g_logFile.fileName()
                   << g_logFile.errorString();
        return false;
    }

    return true;
}

void log(const QString &msg)
{
    QMutexLocker locker(&g_logMutex);

    QString line =
        QString("[%1] %2")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"))
            .arg(msg);

    qInfo().noquote() << line;

    if (!initLog())
        return;

    QTextStream out(&g_logFile);
    out << line << "\n";
    out.flush();

    if (g_logFile.size() >= MAX_LOG_SIZE)
    {
        QString name = g_logFile.fileName();
        g_logFile.close();

        rotateLogs(name);

        if (!g_logFile.open(QIODevice::Append | QIODevice::Text)) {
            qWarning() << "Cannot reopen log file after rotation:"
                       << g_logFile.fileName() << g_logFile.errorString();
        }
    }
}
