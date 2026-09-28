#include "AppStorage.h"

#include <QStandardPaths>

namespace appstorage {

QString folder()
{
    // Local, not roaming: a karaoke library and its catalogue belong to this
    // computer. On macOS both names give the same folder.
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

} // namespace appstorage
