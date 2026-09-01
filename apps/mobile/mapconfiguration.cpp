#include "mapconfiguration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTextStream>
#include <QUrl>

namespace {
const QSet<QString> mobileSettingNames = {
    QStringLiteral("TENCENT_MAP_JS_KEY"),
    QStringLiteral("TENCENT_MAP_JS_ORIGIN"),
    QStringLiteral("TENCENT_MAP_PAGE_URL")};

QString unquote(QString value) {
  value = value.trimmed();
  if (value.size() >= 2) {
    const QChar first = value.front();
    const QChar last = value.back();
    if ((first == QLatin1Char('"') && last == QLatin1Char('"')) ||
        (first == QLatin1Char('\'') && last == QLatin1Char('\''))) {
      value = value.mid(1, value.size() - 2);
    }
  }
  return value;
}

void loadDotEnv(const QString &path, QHash<QString, QString> &settings) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return;
  }

  QTextStream stream(&file);
  while (!stream.atEnd()) {
    QString line = stream.readLine().trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
      continue;
    }
    if (line.startsWith(QStringLiteral("export "))) {
      line.remove(0, 7);
    }

    const qsizetype separator = line.indexOf(QLatin1Char('='));
    if (separator <= 0) {
      continue;
    }
    const QString name = line.left(separator).trimmed();
    if (mobileSettingNames.contains(name)) {
      settings.insert(name, unquote(line.mid(separator + 1)));
    }
  }
}
} // namespace

MapConfiguration::MapConfiguration(QObject *parent) : QObject(parent) {
  loadEnvironmentFile();
  remotePageUrl_ = setting("TENCENT_MAP_PAGE_URL");
  javascriptKey_ = setting("TENCENT_MAP_JS_KEY");
  javascriptOrigin_ = setting("TENCENT_MAP_JS_ORIGIN");
  if (javascriptOrigin_.isEmpty()) {
    javascriptOrigin_ = QStringLiteral("http://localhost/");
  }

  if (!remotePageUrl_.isEmpty()) {
    const QUrl pageUrl(remotePageUrl_);
    if (!pageUrl.isValid() || (pageUrl.scheme() != QStringLiteral("https") &&
                               pageUrl.scheme() != QStringLiteral("http"))) {
      remotePageUrl_.clear();
      statusMessage_ =
          QStringLiteral("地图页面地址无效，请检查 TENCENT_MAP_PAGE_URL。");
    } else {
      statusMessage_ = QStringLiteral("正在加载服务器地图页面…");
    }
  } else if (!javascriptKey_.isEmpty() &&
             javascriptKey_ != QStringLiteral("replace_me")) {
    statusMessage_ = QStringLiteral("正在使用本机开发配置加载腾讯地图…");
  } else {
    statusMessage_ = QStringLiteral(
        "尚未检测到地图配置，请在本机 .env 中设置腾讯地图 JS Key。");
  }
}

bool MapConfiguration::configured() const {
  return !remotePageUrl_.isEmpty() ||
         (!javascriptKey_.isEmpty() &&
          javascriptKey_ != QStringLiteral("replace_me"));
}

QString MapConfiguration::mapHtml() const {
  if (javascriptKey_.isEmpty() ||
      javascriptKey_ == QStringLiteral("replace_me")) {
    return {};
  }

  QFile templateFile(QStringLiteral(":/resources/tencent-map.html"));
  if (!templateFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }

  QString html = QString::fromUtf8(templateFile.readAll());
  html.replace(QStringLiteral("__TENCENT_MAP_JS_KEY__"),
               QString::fromLatin1(QUrl::toPercentEncoding(javascriptKey_)));
  return html;
}

QString MapConfiguration::remotePageUrl() const { return remotePageUrl_; }

QString MapConfiguration::javascriptOrigin() const { return javascriptOrigin_; }

QString MapConfiguration::statusMessage() const { return statusMessage_; }

void MapConfiguration::loadEnvironmentFile() {
  const QString explicitPath = qEnvironmentVariable("NCS_ENV_FILE");
  if (!explicitPath.isEmpty()) {
    loadDotEnv(explicitPath, fileSettings_);
    return;
  }

  const QStringList candidates = {
      QDir::current().filePath(QStringLiteral(".env")),
      QDir(QString::fromUtf8(NCS_PROJECT_ROOT))
          .filePath(QStringLiteral(".env")),
      QDir(QCoreApplication::applicationDirPath())
          .filePath(QStringLiteral(".env"))};
  for (const QString &candidate : candidates) {
    if (QFile::exists(candidate)) {
      loadDotEnv(candidate, fileSettings_);
      return;
    }
  }
}

QString MapConfiguration::setting(const char *name) const {
  const QString environmentValue = QString::fromUtf8(qgetenv(name)).trimmed();
  return environmentValue.isEmpty()
             ? fileSettings_.value(QString::fromLatin1(name)).trimmed()
             : environmentValue;
}
