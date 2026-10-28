#pragma once

#include "browser/webengine_compat.h"
#if !SEB_HAS_QTWEBENGINE && SEB_HAS_WEBKITGTK

#include "browser/contracts/i_webprofile.h"

typedef struct _WebKitWebContext WebKitWebContext;

namespace seb::browser {

class WebKitGtkProfile : public contracts::IWebProfile {
  Q_OBJECT

public:
  explicit WebKitGtkProfile(QObject *parent = nullptr);
  ~WebKitGtkProfile() override;

  void setPersistentStoragePath(const QString &path) override;
  void setCachePath(const QString &path) override;
  void setDownloadPath(const QString &path) override;

  void setPdfViewerEnabled(bool enabled) override;
  void setSpellCheckEnabled(bool enabled) override;
  void setSpellCheckLanguages(const QStringList &languages) override;
  void setHttpUserAgent(const QString &userAgent) override;
  void setDevBypass(bool enabled) override {}

  void setUrlRequestInterceptor(
      seb::browser::contracts::IRequestInterceptor *interceptor) override;
  void deleteAllCookies() override;

  // Shared by every view of this profile, so they all load the SEB web
  // process extension with the same configuration.
  WebKitWebContext *context() const;
  QString userAgent() const;

private:
  class Private;
  std::unique_ptr<Private> d;
};

} // namespace seb::browser

#endif // !SEB_HAS_QTWEBENGINE && SEB_HAS_WEBKITGTK
