#include "veyra/ui/OnlineUpdater.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <cstring>

using veyra::ui::OnlineUpdater;
using veyra::ui::UpdateAsset;
namespace {
QJsonObject release(const QByteArray& payload = "verified zip", QString flavor = "NVIDIA", QString tag = "v2.0.8") {
    const auto name = QStringLiteral("Veyra-%1-%2-win64-portable.zip").arg(tag.mid(1), flavor);
    return {{"tag_name", tag}, {"draft", false}, {"prerelease", false}, {"assets", QJsonArray{QJsonObject{
        {"name", name}, {"size", payload.size()}, {"digest", "sha256:" + QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())},
        {"browser_download_url", "https://github.com/Likely7/Veyra-NRVideo/releases/download/" + tag + "/" + name}}}}};
}
class Reply final : public QNetworkReply {
public:
    Reply(const QNetworkRequest& req, QByteArray body, int status, QObject* parent)
        : QNetworkReply(parent), body_(std::move(body)) {
        setRequest(req); setUrl(req.url()); setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (status != 200) setError(QNetworkReply::ContentAccessDenied, QStringLiteral("Denied"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (isFinished()) return;
            emit readyRead();
            if (!isFinished()) { setFinished(true); emit finished(); }
        });
    }
    void abort() override {
        if (isFinished()) return;
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("Canceled"));
        setFinished(true); emit finished();
    }
    qint64 bytesAvailable() const override { return body_.size() - position_ + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* data, qint64 max) override {
        const auto count = qMin(max, qint64(body_.size()) - position_);
        if (!count) return -1;
        std::memcpy(data, body_.constData() + position_, std::size_t(count)); position_ += count; return count;
    }
private:
    QByteArray body_; qint64 position_ = 0;
};
class Network final : public QNetworkAccessManager {
public:
    QByteArray metadata = QJsonDocument(release()).toJson(), payload = "verified zip";
    int status = 200, requests = 0;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& req, QIODevice*) override {
        ++requests;
        return new Reply(req, req.url().host() == "api.github.com" ? metadata : payload, status, this);
    }
};
void manifest(const QString& directory, const QString& flavor = "NVIDIA") {
    QFile file(directory + "/package-manifest.json");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{{"gpuPackage", flavor}}).toJson());
}
}
class Tests final : public QObject {
    Q_OBJECT
private slots:
    void versions_data() {
        QTest::addColumn<QString>("local"); QTest::addColumn<QString>("remote"); QTest::addColumn<bool>("update");
        QTest::newRow("new patch") << "2.0.7" << "v2.0.8" << true;
        QTest::newRow("equal stable") << "2.0.8" << "v2.0.8" << false;
        QTest::newRow("no downgrade") << "2.1.0" << "v2.0.8" << false;
        QTest::newRow("numeric order") << "2.0.9" << "v2.0.10" << true;
        QTest::newRow("preview to stable") << "2.0.8-test-r2" << "v2.0.8" << true;
    }
    void versions() {
        QFETCH(QString, local); QFETCH(QString, remote); QFETCH(bool, update);
        UpdateAsset asset; QString error;
        QVERIFY(OnlineUpdater::selectRelease(release("verified zip", "NVIDIA", remote), local, "NVIDIA", asset, error));
        QCOMPARE(!asset.name.isEmpty(), update);
    }
    void packageSelection() {
        auto data = release();
        auto assets = data["assets"].toArray(); assets.append(release("verified zip", "AMD")["assets"].toArray()[0]);
        assets.append(QJsonObject{{"name", "Veyra-2.0.8-source.zip"}}); data["assets"] = assets;
        UpdateAsset asset; QString error;
        QVERIFY(OnlineUpdater::selectRelease(data, "2.0.7", "AMD", asset, error));
        QCOMPARE(asset.name, QStringLiteral("Veyra-2.0.8-AMD-win64-portable.zip"));
    }
    void rejectMetadata_data() {
        QTest::addColumn<QString>("mutation");
        for (const auto& value : {"draft", "prerelease", "preview", "missing-digest", "unsafe-url", "http", "oversize", "duplicate", "invalid-duplicate", "source-only", "invalid-version", "unknown-flavor"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }
    void rejectMetadata() {
        QFETCH(QString, mutation);
        auto data = release(); auto rows = data["assets"].toArray(); auto row = rows[0].toObject();
        if (mutation == "draft" || mutation == "prerelease") data[mutation] = true;
        if (mutation == "preview") data["tag_name"] = "v2.0.8-test";
        if (mutation == "missing-digest") row.remove("digest");
        if (mutation == "unsafe-url") row["browser_download_url"] = "https://example.com/package.zip";
        if (mutation == "http") row["browser_download_url"] = row["browser_download_url"].toString().replace("https:", "http:");
        if (mutation == "oversize") row["size"] = 9.0 * 1024 * 1024 * 1024;
        if (mutation == "source-only") row["name"] = "Veyra-2.0.8-source.zip";
        if (mutation == "invalid-version") data["tag_name"] = "v2.0.8-sources";
        rows[0] = row; if (mutation == "duplicate") rows.append(row); data["assets"] = rows;
        if (mutation == "invalid-duplicate") {
            row.remove("digest"); rows.append(row); data["assets"] = rows;
        }
        UpdateAsset asset; QString error;
        QVERIFY(!OnlineUpdater::selectRelease(data, "2.0.7", mutation == "unknown-flavor" ? "Intel" : "NVIDIA", asset, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(asset.name.isEmpty());
    }
    void missingPackage() {
        QTemporaryDir dir; Network network; OnlineUpdater updater("2.0.7", dir.path(), nullptr, &network);
        updater.check(); QCOMPARE(updater.state(), QStringLiteral("failed")); QCOMPARE(network.requests, 0);
    }
    void officialPackageWithPreviewLabel() {
        QTemporaryDir dir;
        QFile file(dir.filePath("package-manifest.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"gpuPackage\":\"NVIDIA\",\"version\":\"2.0.7\"}"); file.close();
        Network network; network.metadata = QJsonDocument(release("verified zip", "NVIDIA", "v2.0.7")).toJson();
        OnlineUpdater updater("2.0.7-test-r2", dir.path(), nullptr, &network);
        updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("current"));
        QCOMPARE(updater.latestVersion(), QString());
    }
    void downloadVerified() {
        QTemporaryDir dir; manifest(dir.path()); Network network;
        OnlineUpdater updater("2.0.7", dir.path(), nullptr, &network);
        updater.check(); updater.check(); QCOMPARE(network.requests, 1);
        QTRY_COMPARE(updater.state(), QStringLiteral("available"));
        updater.download(); updater.download(); QCOMPARE(network.requests, 2);
        QTRY_COMPARE(updater.state(), QStringLiteral("ready")); QCOMPARE(updater.progress(), 1.0);
    }
    void badDownload_data() {
        QTest::addColumn<QByteArray>("payload");
        QTest::newRow("wrong hash same size") << QByteArray("modified zip");
        QTest::newRow("truncated") << QByteArray("zip");
        QTest::newRow("too large") << QByteArray("zip longer than expected");
    }
    void badDownload() {
        QFETCH(QByteArray, payload); QTemporaryDir dir; manifest(dir.path()); Network network;
        OnlineUpdater updater("2.0.7", dir.path(), nullptr, &network);
        updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("available"));
        network.payload = payload; updater.download(); QTRY_COMPARE(updater.state(), QStringLiteral("failed"));
        QSignalSpy install(&updater, &OnlineUpdater::installRequested);
        updater.install(); QCOMPARE(install.count(), 0);
    }
    void deniedAndRetry() {
        QTemporaryDir dir; manifest(dir.path()); Network network; network.status = 403;
        OnlineUpdater updater("2.0.7", dir.path(), nullptr, &network);
        updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("failed"));
        network.status = 200; updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("available"));
    }
    void cancelAndRetry() {
        QTemporaryDir dir; manifest(dir.path()); Network network;
        OnlineUpdater updater("2.0.7", dir.path(), nullptr, &network);
        updater.check(); updater.cancel(); QCOMPARE(updater.state(), QStringLiteral("idle"));
        updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("available"));
        updater.download(); updater.cancel(); QCOMPARE(updater.state(), QStringLiteral("idle"));
        updater.check(); QTRY_COMPARE(updater.state(), QStringLiteral("available"));
        updater.download(); QTRY_COMPARE(updater.state(), QStringLiteral("ready"));
    }
};
QTEST_GUILESS_MAIN(Tests)
#include "OnlineUpdaterTests.moc"
