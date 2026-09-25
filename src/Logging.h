#pragma once

#include <QLoggingCategory>
#include <QString>

Q_DECLARE_LOGGING_CATEGORY(lcApp)
Q_DECLARE_LOGGING_CATEGORY(lcPlayer)
Q_DECLARE_LOGGING_CATEGORY(lcCdg)
Q_DECLARE_LOGGING_CATEGORY(lcUi)

namespace logging {

// Sends Qt log output to stderr and to a log file in the user's application
// data folder. Returns the log file path (empty if the file could not be opened).
QString install();

} // namespace logging
