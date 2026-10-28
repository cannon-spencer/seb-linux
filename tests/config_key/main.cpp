// Config Key check against files saved by SEB itself (vectors from Moodle's
// quizaccess_seb tests). Build: see CMake-free command in README.
#include "settings/settings_parsing.h"

#include <QCoreApplication>
#include <QFile>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // `main <file> <expected-key>` checks one extra file (e.g. one written by
    // the exam server); `main <dir>` runs the built-in SEB vectors.
    if (argc == 3) {
        QFile f(QString::fromLocal8Bit(argv[1]));
        if (!f.open(QIODevice::ReadOnly)) return 2;
        QString error;
        const QString key = seb::settingsinternal::calculateConfigurationKey(seb::settingsinternal::parsePlist(f.readAll(), &error));
        std::printf("%s %s\n", key == QLatin1String(argv[2]) ? "ok  " : "FAIL", qPrintable(key));
        return key == QLatin1String(argv[2]) ? 0 : 1;
    }
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    const struct { const char *file; const char *key; } cases[] = {
        {"unencrypted_mac_001.seb", "4fa9af8ec8759eb7c680752ef4ee5eaf1a860628608fccae2715d519849f9292"},
        {"unencrypted_win_223.seb", "2534e4e9f3188f9f9133bf7cf7b4c5d898292bbd7e8d0230f39d1176636a1431"},
    };
    int failures = 0;
    for (const auto &c : cases) {
        QFile f(dir + QLatin1Char('/') + QLatin1String(c.file));
        if (!f.open(QIODevice::ReadOnly)) { std::printf("FAIL %s: cannot open\n", c.file); ++failures; continue; }
        QString error;
        const QVariantMap data = seb::settingsinternal::parsePlist(f.readAll(), &error);
        const QString key = seb::settingsinternal::calculateConfigurationKey(data);
        const bool ok = error.isEmpty() && key == QLatin1String(c.key);
        std::printf("%s %s %s\n", ok ? "ok  " : "FAIL", c.file, qPrintable(key));
        failures += ok ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}
