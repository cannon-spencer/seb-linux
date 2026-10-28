#include "settings_parsing.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QLocale>
#include <QDomDocument>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QVariant>

#include <zlib.h>

#include <algorithm>

namespace seb::settingsinternal {
namespace {

// ----- SEB-JSON (Config Key input) -------------------------------------------
//
// https://safeexambrowser.org/developer/seb-config-key.html
// Keys sorted case-insensitively (lowercase first on ties), recursively;
// originatorVersion removed at the root; empty dictionaries dropped; no
// whitespace; no escaping of '/', '\' or non-ASCII; data as base64; dates
// as ISO 8601. Checked against Config Keys of files saved by SEB itself.

bool isEmptyMap(const QVariant &value)
{
    return value.metaType().id() == QMetaType::QVariantMap && value.toMap().isEmpty();
}

void appendSebJsonString(QString &out, const QString &text)
{
    out += QLatin1Char('"');
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '"': out += QStringLiteral("\\\""); break;
        case '\n': out += QStringLiteral("\\n"); break;
        case '\r': out += QStringLiteral("\\r"); break;
        case '\t': out += QStringLiteral("\\t"); break;
        default:
            if (c.unicode() < 0x20) {
                out += QStringLiteral("\\u%1").arg(c.unicode(), 4, 16, QLatin1Char('0'));
            } else {
                out += c;
            }
        }
    }
    out += QLatin1Char('"');
}

void appendSebJson(QString &out, const QVariant &value);

void appendSebJsonMap(QString &out, const QVariantMap &map)
{
    QStringList keys;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (!isEmptyMap(it.value())) keys << it.key();
    }
    std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
        const int ci = QString::compare(a, b, Qt::CaseInsensitive);
        return ci != 0 ? ci < 0 : QString::compare(a, b, Qt::CaseSensitive) > 0;
    });
    out += QLatin1Char('{');
    for (qsizetype i = 0; i < keys.size(); ++i) {
        if (i > 0) out += QLatin1Char(',');
        appendSebJsonString(out, keys.at(i));
        out += QLatin1Char(':');
        appendSebJson(out, map.value(keys.at(i)));
    }
    out += QLatin1Char('}');
}

void appendSebJson(QString &out, const QVariant &value)
{
    switch (value.metaType().id()) {
    case QMetaType::QVariantMap:
        appendSebJsonMap(out, value.toMap());
        return;
    case QMetaType::QVariantList: {
        out += QLatin1Char('[');
        bool first = true;
        for (const QVariant &item : value.toList()) {
            if (isEmptyMap(item)) continue;
            if (!first) out += QLatin1Char(',');
            first = false;
            appendSebJson(out, item);
        }
        out += QLatin1Char(']');
        return;
    }
    case QMetaType::Bool:
        out += value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        return;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        out += QString::number(value.toLongLong());
        return;
    case QMetaType::Double:
        // Shortest representation that round-trips: 0.10000000000000001 -> 0.1.
        out += QString::number(value.toDouble(), 'g', QLocale::FloatingPointShortest);
        return;
    case QMetaType::QByteArray:
        appendSebJsonString(out, QString::fromLatin1(value.toByteArray().toBase64()));
        return;
    case QMetaType::QDateTime:
        appendSebJsonString(out, value.toDateTime().toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss+00:00")));
        return;
    default:
        appendSebJsonString(out, value.toString());
        return;
    }
}

QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QVariant parsePlistElement(const QDomElement &element)
{
    const QString tag = element.tagName();
    if (tag == "dict") {
        QVariantMap map;
        QDomElement child = element.firstChildElement();
        while (!child.isNull()) {
            if (child.tagName() != "key") {
                child = child.nextSiblingElement();
                continue;
            }

            const QString key = child.text();
            const QDomElement valueElement = child.nextSiblingElement();
            if (valueElement.isNull()) break;

            map.insert(key, parsePlistElement(valueElement));
            child = valueElement.nextSiblingElement();
        }
        return map;
    }

    if (tag == "array") {
        QVariantList values;
        for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            values.push_back(parsePlistElement(child));
        }
        return values;
    }

    if (tag == "true") return true;
    if (tag == "false") return false;
    if (tag == "integer") {
        bool ok = false;
        const int value = element.text().trimmed().toInt(&ok);
        return ok ? QVariant(value) : QVariant(element.text().trimmed());
    }
    if (tag == "real") {
        bool ok = false;
        const double value = element.text().trimmed().toDouble(&ok);
        return ok ? QVariant(value) : QVariant(element.text().trimmed());
    }
    if (tag == "data") return QByteArray::fromBase64(element.text().trimmed().toUtf8());
    if (tag == "date") {
        const QDateTime when = QDateTime::fromString(element.text().trimmed(), Qt::ISODate);
        if (when.isValid()) return when;
    }
    return element.text();
}

}  // namespace

QString sebJson(const QVariantMap &data)
{
    QVariantMap root = data;
    root.remove(QStringLiteral("originatorVersion"));
    QString out;
    appendSebJsonMap(out, root);
    return out;
}

QString calculateConfigurationKey(const QVariantMap &data)
{
    return sha256Hex(sebJson(data).toUtf8());
}

QByteArray inflateGzip(const QByteArray &data, QString *error)
{
    z_stream stream {};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());

    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        if (error) *error = QStringLiteral("Failed to initialize gzip decompression.");
        return {};
    }

    QByteArray output;
    char buffer[8192];
    int status = Z_OK;

    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        status = inflate(&stream, Z_NO_FLUSH);

        if (status != Z_OK && status != Z_STREAM_END) {
            if (error) *error = QStringLiteral("Failed to decompress gzip-compressed SEB data.");
            inflateEnd(&stream);
            return {};
        }

        output.append(buffer, static_cast<int>(sizeof(buffer) - stream.avail_out));
    }

    inflateEnd(&stream);
    return output;
}

QVariantMap parsePlist(const QByteArray &raw, QString *error)
{
    QDomDocument document;
    QString parseError;
    int line = 0;
    int column = 0;
    if (!document.setContent(raw, &parseError, &line, &column)) {
        if (error) {
            *error = QStringLiteral("Failed to parse XML property list at line %1, column %2: %3")
                         .arg(line)
                         .arg(column)
                         .arg(parseError);
        }
        return {};
    }

    const QDomElement root = document.documentElement();
    if (root.tagName() != "plist") {
        if (error) *error = QStringLiteral("Unsupported XML format: expected a plist root element.");
        return {};
    }

    const QDomElement payload = root.firstChildElement();
    if (payload.isNull() || payload.tagName() != "dict") {
        if (error) *error = QStringLiteral("Unsupported plist payload: expected a top-level dict.");
        return {};
    }

    return parsePlistElement(payload).toMap();
}

QVariantMap parseJson(const QByteArray &raw, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) *error = QStringLiteral("Failed to parse JSON: %1").arg(parseError.errorString());
        return {};
    }

    if (!document.isObject()) {
        if (error) *error = QStringLiteral("Expected a JSON object at the top level.");
        return {};
    }

    return document.object().toVariantMap();
}

}  // namespace seb::settingsinternal
