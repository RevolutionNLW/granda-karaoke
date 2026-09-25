#include "SongPair.h"

#include "Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

// Returns the companion file with the given extension, preferring an exact
// base-name match and falling back to a case-insensitive one.
QString findCompanion(const QFileInfo& selected, const QString& wantedSuffix)
{
    const QString base = selected.completeBaseName();
    for (const QString& suffix : {wantedSuffix.toLower(), wantedSuffix.toUpper()}) {
        const QString path = selected.dir().absoluteFilePath(base + '.' + suffix);
        const QFileInfo info(path);
        if (QFileInfo::exists(path) && info.isFile() && !info.isSymLink()) {
            // Canonicalisation recovers the stored filename case on macOS.
            // Keep the selected directory spelling (including directory links).
            return selected.dir().absoluteFilePath(QFileInfo(info.canonicalFilePath()).fileName());
        }
    }
    const QFileInfoList entries = selected.dir().entryInfoList(QDir::Files | QDir::Hidden);

    QString caseInsensitiveMatch;
    for (const QFileInfo& entry : entries) {
        if (entry.suffix().compare(wantedSuffix, Qt::CaseInsensitive) != 0)
            continue;
        if (entry.completeBaseName() == base)
            return entry.absoluteFilePath();
        if (caseInsensitiveMatch.isEmpty()
            && entry.completeBaseName().compare(base, Qt::CaseInsensitive) == 0)
            caseInsensitiveMatch = entry.absoluteFilePath();
    }
    return caseInsensitiveMatch;
}

QString checkReadable(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return QStringLiteral("The file \"%1\" could not be found.").arg(info.fileName());
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcApp) << "Cannot open" << path << ":" << file.errorString();
        return QStringLiteral("The file \"%1\" could not be opened. It may be damaged, "
                              "or you may not have permission to read it.").arg(info.fileName());
    }
    if (file.size() == 0)
        return QStringLiteral("The file \"%1\" is empty.").arg(info.fileName());
    return {};
}

} // namespace

QString SongPair::displayName() const
{
    return QFileInfo(mp3Path).completeBaseName();
}

SongPairResult resolveSongPair(const QString& selectedPath)
{
    const QFileInfo selected(selectedPath);
    const QString suffix = selected.suffix().toLower();

    SongPairResult result;
    if (suffix != QLatin1String("mp3") && suffix != QLatin1String("cdg")) {
        result.error = QStringLiteral("\"%1\" is not a karaoke song. Please choose an .mp3 or .cdg file.")
                           .arg(selected.fileName());
        return result;
    }
    if (!selected.exists()) {
        result.error = QStringLiteral("The file \"%1\" could not be found.").arg(selected.fileName());
        return result;
    }

    const bool pickedMp3 = suffix == QLatin1String("mp3");
    const QString companionSuffix = pickedMp3 ? QStringLiteral("cdg") : QStringLiteral("mp3");
    const QString companion = findCompanion(selected, companionSuffix);
    if (companion.isEmpty()) {
        result.error = pickedMp3
            ? QStringLiteral("The lyrics file (.cdg) for \"%1\" is missing. "
                             "It must be in the same folder with the same name.")
                  .arg(selected.completeBaseName())
            : QStringLiteral("The music file (.mp3) for \"%1\" is missing. "
                             "It must be in the same folder with the same name.")
                  .arg(selected.completeBaseName());
        qCWarning(lcApp) << "No companion ." << companionSuffix << "for" << selected.absoluteFilePath();
        return result;
    }

    SongPair pair;
    pair.mp3Path = pickedMp3 ? selected.absoluteFilePath() : companion;
    pair.cdgPath = pickedMp3 ? companion : selected.absoluteFilePath();

    for (const QString& path : {pair.mp3Path, pair.cdgPath}) {
        const QString error = checkReadable(path);
        if (!error.isEmpty()) {
            result.error = error;
            return result;
        }
    }

    result.pair = pair;
    return result;
}
