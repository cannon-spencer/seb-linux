#include "webkitgtk_profile.h"

#if !SEB_HAS_QTWEBENGINE && SEB_HAS_WEBKITGTK

#ifdef signals
#pragma push_macro("signals")
#undef signals
#define SEB_WEBKITGTK_RESTORE_SIGNALS_MACRO
#endif
#include <webkit2/webkit2.h>
#ifdef SEB_WEBKITGTK_RESTORE_SIGNALS_MACRO
#pragma pop_macro("signals")
#undef SEB_WEBKITGTK_RESTORE_SIGNALS_MACRO
#endif

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVector>
#include <QtGlobal>

#include <cstdlib>

#include "browser/request_filter.h"
#include "browser/request_interceptor.h"

#ifndef SEB_WEBKIT_EXTENSION_DIR
#define SEB_WEBKIT_EXTENSION_DIR "/usr/lib/seb-linux/webkit-extensions"
#endif

namespace seb::browser {

class WebKitGtkProfile::Private
{
public:
    WebKitWebContext *context = nullptr;
    QString userAgent;
    GVariant *extensionData = nullptr;  // floating until handed to WebKit
};

namespace {

constexpr const char *kExtensionFile = "libseb-web-extension.so";

// SEB_WEBKIT_EXTENSION_DIR env var, then next to the binary, then the
// install location. Empty if the extension is nowhere to be found.
QString findExtensionDirectory()
{
    QStringList candidates;
    const QString fromEnv = qEnvironmentVariable("SEB_WEBKIT_EXTENSION_DIR");
    if (!fromEnv.isEmpty()) candidates << fromEnv;
    candidates << QCoreApplication::applicationDirPath() + QStringLiteral("/webkit-extensions");
    candidates << QStringLiteral(SEB_WEBKIT_EXTENSION_DIR);
    for (const QString &dir : candidates) {
        if (QFileInfo::exists(QDir(dir).filePath(QString::fromLatin1(kExtensionFile)))) {
            return dir;
        }
    }
    return {};
}

QString loadApiScript()
{
    QFile file(QStringLiteral(":/browser/content/Api.js"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QString script = QString::fromUtf8(file.readAll());
    if (script.startsWith(QChar(0xFEFF))) script.remove(0, 1);
    return script.replace(QStringLiteral("%%_VERSION_%%"), QCoreApplication::applicationVersion());
}

GVariant *buildExtensionData(const RequestInterceptor &interceptor)
{
    const seb::SebSettings &settings = interceptor.settings();
    GVariantBuilder rules;
    g_variant_builder_init(&rules, G_VARIANT_TYPE("a(sb)"));
    for (const seb::FilterRuleSettings &rule : settings.browser.filter.rules) {
        const QByteArray pattern = RequestFilter::patternFor(rule).toUtf8();
        g_variant_builder_add(&rules, "(sb)", pattern.constData(),
                              static_cast<gboolean>(rule.result == seb::FilterResult::Allow));
    }

    const QByteArray configKey = interceptor.configurationKey().toUtf8();
    const QByteArray browserExamKey = interceptor.browserExamKey().toUtf8();
    const QByteArray api = loadApiScript().toUtf8();

    GVariantBuilder dict;
    g_variant_builder_init(&dict, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&dict, "{sv}", "configKey", g_variant_new_string(configKey.constData()));
    g_variant_builder_add(&dict, "{sv}", "browserExamKey", g_variant_new_string(browserExamKey.constData()));
    g_variant_builder_add(&dict, "{sv}", "sendConfigKey", g_variant_new_boolean(settings.browser.sendConfigurationKey));
    g_variant_builder_add(&dict, "{sv}", "sendBrowserExamKey", g_variant_new_boolean(settings.browser.sendBrowserExamKey));
    g_variant_builder_add(&dict, "{sv}", "filterMain", g_variant_new_boolean(settings.browser.filter.processMainRequests));
    g_variant_builder_add(&dict, "{sv}", "filterContent", g_variant_new_boolean(settings.browser.filter.processContentRequests));
    g_variant_builder_add(&dict, "{sv}", "rules", g_variant_builder_end(&rules));
    g_variant_builder_add(&dict, "{sv}", "apiScript", g_variant_new_string(api.constData()));
    return g_variant_builder_end(&dict);
}

}  // namespace

WebKitGtkProfile::WebKitGtkProfile(QObject *parent)
    : IWebProfile(parent)
    , d(std::make_unique<Private>())
{
    // Create an ephemeral or persistent WebKitContext
    d->context = webkit_web_context_new();

    // WebKit asks for the extension's data when it spawns a web process,
    // which happens after setUrlRequestInterceptor() has filled it in.
    g_signal_connect(
        d->context, "initialize-web-extensions",
        G_CALLBACK(+[](WebKitWebContext *context, gpointer data) {
            auto *priv = static_cast<Private *>(data);
            if (priv->extensionData) {
                webkit_web_context_set_web_extensions_initialization_user_data(context, priv->extensionData);
            }
        }),
        d.get());
}

WebKitGtkProfile::~WebKitGtkProfile()
{
    if (d->context) {
        g_object_unref(d->context);
    }
    if (d->extensionData) {
        g_variant_unref(d->extensionData);
    }
}

WebKitWebContext *WebKitGtkProfile::context() const
{
    return d->context;
}

QString WebKitGtkProfile::userAgent() const
{
    return d->userAgent;
}

void WebKitGtkProfile::setPersistentStoragePath(const QString &path)
{
    // WebKitGTK data dir config
}

void WebKitGtkProfile::setCachePath(const QString &path)
{
    // WebKitGTK cache dir config
}

void WebKitGtkProfile::setDownloadPath(const QString &path)
{
    // Handled at download request level in WebKitGTK
}

void WebKitGtkProfile::setPdfViewerEnabled(bool enabled)
{
    // Not directly supported out of the box in WebKitGTK without Evince plugin
}

void WebKitGtkProfile::setSpellCheckEnabled(bool enabled)
{
    if (d->context) {
        webkit_web_context_set_spell_checking_enabled(d->context, enabled);
    }
}

void WebKitGtkProfile::setSpellCheckLanguages(const QStringList &languages)
{
    if (!d->context) {
        return;
    }

    QVector<QByteArray> encodedLanguages;
    encodedLanguages.reserve(languages.size());

    QVector<const gchar *> spellCheckingLanguages;
    spellCheckingLanguages.reserve(languages.size() + 1);

    for (const QString &language : languages) {
        const QString trimmedLanguage = language.trimmed();
        if (trimmedLanguage.isEmpty()) {
            continue;
        }

        encodedLanguages.push_back(trimmedLanguage.toUtf8());
        spellCheckingLanguages.push_back(encodedLanguages.constLast().constData());
    }

    spellCheckingLanguages.push_back(nullptr);

    webkit_web_context_set_spell_checking_languages(
        d->context,
        spellCheckingLanguages.size() > 1 ? spellCheckingLanguages.constData() : nullptr);
}

void WebKitGtkProfile::setHttpUserAgent(const QString &userAgent)
{
    // Applied to each view's WebKitSettings when the view is created.
    d->userAgent = userAgent;
}

void WebKitGtkProfile::setUrlRequestInterceptor(seb::browser::contracts::IRequestInterceptor *interceptor)
{
    // WebKitGTK loads pages in a separate web process, so filtering and the
    // SEB key headers run in a web process extension that gets the same
    // rules and keys as RequestInterceptor.
    auto *sebInterceptor = dynamic_cast<RequestInterceptor *>(interceptor);
    if (!sebInterceptor) {
        return;
    }

    const seb::SebSettings &settings = sebInterceptor->settings();
    const bool needed = settings.browser.filter.processMainRequests ||
                        settings.browser.filter.processContentRequests ||
                        settings.browser.sendBrowserExamKey ||
                        settings.browser.sendConfigurationKey;

    const QString dir = findExtensionDirectory();
    if (dir.isEmpty()) {
        if (needed) {
            // Without the extension there is no URL filter and no key
            // headers. An exam browser must not run like that.
            qCritical("seb-linux: %s not found (looked in $SEB_WEBKIT_EXTENSION_DIR, %s/webkit-extensions, %s). "
                      "Refusing to start: URL filtering and SEB keys would be off.",
                      kExtensionFile, qPrintable(QCoreApplication::applicationDirPath()), SEB_WEBKIT_EXTENSION_DIR);
            std::exit(2);
        }
        qWarning("seb-linux: %s not found; continuing without URL filter or SEB keys", kExtensionFile);
        return;
    }

    if (d->extensionData) {
        g_variant_unref(d->extensionData);
    }
    d->extensionData = g_variant_ref_sink(buildExtensionData(*sebInterceptor));
    webkit_web_context_set_web_extensions_directory(d->context, QFile::encodeName(dir).constData());
    qInfo("seb-linux: WebKitGTK extension from %s, %lld filter rules", qPrintable(dir),
          static_cast<long long>(settings.browser.filter.rules.size()));
}

void WebKitGtkProfile::deleteAllCookies()
{
    WebKitCookieManager *cookieManager = webkit_web_context_get_cookie_manager(d->context);
    webkit_cookie_manager_delete_all_cookies(cookieManager);
}

}  // namespace seb::browser

#endif // !SEB_HAS_QTWEBENGINE && SEB_HAS_WEBKITGTK
