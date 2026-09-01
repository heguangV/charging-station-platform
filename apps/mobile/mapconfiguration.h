#pragma once

#include <QHash>
#include <QObject>
#include <QString>

class MapConfiguration final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool configured READ configured CONSTANT)
  Q_PROPERTY(QString mapHtml READ mapHtml CONSTANT)
  Q_PROPERTY(QString javascriptOrigin READ javascriptOrigin CONSTANT)
  Q_PROPERTY(QString remotePageUrl READ remotePageUrl CONSTANT)
  Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)

public:
  explicit MapConfiguration(QObject *parent = nullptr);

  [[nodiscard]] bool configured() const;
  [[nodiscard]] QString mapHtml() const;
  [[nodiscard]] QString javascriptOrigin() const;
  [[nodiscard]] QString remotePageUrl() const;
  [[nodiscard]] QString statusMessage() const;

private:
  void loadEnvironmentFile();
  [[nodiscard]] QString setting(const char *name) const;

  QString javascriptKey_;
  QString javascriptOrigin_;
  QString remotePageUrl_;
  QString statusMessage_;
  QHash<QString, QString> fileSettings_;
};
