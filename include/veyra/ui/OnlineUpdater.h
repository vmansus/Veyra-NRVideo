#pragma once
#include <QObject>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QUrl>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace veyra::ui {
struct UpdateAsset {
    QString version, name, sha256;
    QUrl url;
    qint64 size = 0;
};

// Independent of the player: networking never runs on its processing thread.
class OnlineUpdater final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
public:
    OnlineUpdater(QString currentVersion, QString installDirectory, QObject* parent = nullptr,
                  QNetworkAccessManager* network = nullptr);
    ~OnlineUpdater() override;
    QString state() const { return state_; }
    QString message() const { return message_; }
    QString latestVersion() const { return asset_.version; }
    double progress() const { return progress_; }
    Q_INVOKABLE void check();
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancel();
    // Called by the bridge after it checks playback/export state.
    void install();
    static QString packageFlavor(const QString& directory);
    static bool selectRelease(const QJsonObject& release, const QString& current,
                              const QString& flavor, UpdateAsset& asset, QString& error);
signals:
    void changed();
    void installRequested();
private:
    void setState(const QString& state, const QString& message);
    void fail(const QString& message);
    QString current_, directory_, flavor_, state_ = QStringLiteral("idle"), message_;
    UpdateAsset asset_;
    double progress_ = 0;
    qint64 received_ = 0;
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    std::unique_ptr<QTemporaryDir> work_;
    QFile archive_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
};
}
