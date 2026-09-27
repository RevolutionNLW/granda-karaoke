#include "library/KnownLibraryRoots.h"

#include <QSettings>

namespace {

// Named explicitly so the application and the command-line tool share them.
const QString kOrganization = QStringLiteral("Granda");
const QString kApplication = QStringLiteral("FrankiesKaraokeStudio");
const QString kKey = QStringLiteral("library/knownRoots");

} // namespace

namespace KnownLibraryRoots {

QStringList load()
{
    return QSettings(kOrganization, kApplication).value(kKey).toStringList();
}

void remember(const QStringList& roots)
{
    QSettings settings(kOrganization, kApplication);
    QStringList known = settings.value(kKey).toStringList();
    const qsizetype before = known.size();
    for (const QString& root : roots) {
        if (!root.isEmpty() && !known.contains(root))
            known.append(root);
    }
    if (known.size() != before)
        settings.setValue(kKey, known);
}

} // namespace KnownLibraryRoots
