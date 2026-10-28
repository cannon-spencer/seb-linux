#pragma once

#include "../seb_settings.h"
#include <QVariant>

namespace seb::settingsinternal {

// SEB-JSON string the Config Key is hashed from (exposed for tests).
QString sebJson(const QVariantMap &data);
QString calculateConfigurationKey(const QVariantMap &data);
QByteArray inflateGzip(const QByteArray &data, QString *error);
QVariantMap parseJson(const QByteArray &raw, QString *error);
QVariantMap parsePlist(const QByteArray &raw, QString *error);

}  // namespace seb::settingsinternal
