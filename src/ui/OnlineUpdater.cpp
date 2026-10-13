#include "veyra/ui/OnlineUpdater.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxyFactory>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <array>
#include <optional>

static void initUpdaterResources() { Q_INIT_RESOURCE(veyra_updater); }
namespace veyra::ui {
namespace {
class SystemProxyFactory final : public QNetworkProxyFactory {
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        return QNetworkProxyFactory::systemProxyForQuery(query);
    }
};
struct Version { std::array<quint32, 3> parts; bool preview; QString text; };
QJsonObject packageInfo(const QString& directory) {
    QFile file(QDir(directory).filePath(QStringLiteral("package-manifest.json")));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
std::optional<Version> version(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("^v?(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-[A-Za-z0-9][A-Za-z0-9.-]*)?$"));
    const auto match = pattern.match(text);
    if (!match.hasMatch()) return {};
    Version result{};
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        result.parts[static_cast<std::size_t>(i)] = match.captured(i + 1).toUInt(&ok);
        if (!ok) return {};
    }
    result.preview = !match.captured(4).isEmpty();
    result.text = text.startsWith('v') ? text.mid(1) : text;
    return result;
}
QNetworkRequest request(const QUrl& url) {
    QNetworkRequest result(url);
    result.setRawHeader("User-Agent", "Veyra-OnlineUpdater");
    result.setRawHeader("Accept", "application/vnd.github+json");
    result.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    result.setTransferTimeout(30000);
    return result;
}
#ifdef Q_OS_WIN
bool saveJson(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson()) > 0 && file.commit();
}
#endif
}

QString OnlineUpdater::packageFlavor(const QString& directory) {
    const auto object = packageInfo(directory);
    const auto flavor = object.value("gpuPackage").toString();
    // The installed package, not the first GPU in a multi-adapter machine.
    return flavor == "NVIDIA" || flavor == "AMD" ? flavor : QString();
}

bool OnlineUpdater::selectRelease(const QJsonObject& release, const QString& current,
                                 const QString& flavor, UpdateAsset& asset, QString& error) {
    asset = {};
    error.clear();
    const auto local = version(current), remote = version(release.value("tag_name").toString());
    if (!local || !remote || remote->preview || !release.value("draft").isBool() ||
        !release.value("prerelease").isBool() || release.value("draft").toBool() ||
        release.value("prerelease").toBool() || !release.value("assets").isArray()) {
        error = tr("发布信息无效，请前往发布页手动更新"); return false;
    }
    if (remote->parts < local->parts || (remote->parts == local->parts && !local->preview)) return true;
    if (flavor != "NVIDIA" && flavor != "AMD") {
        error = tr("无法识别当前便携包类型，请前往发布页手动更新"); return false;
    }
    const auto name = QStringLiteral("Veyra-%1-%2-win64-portable.zip").arg(remote->text, flavor);
    int matches = 0;
    UpdateAsset candidate;
    for (const auto& value : release.value("assets").toArray()) {
        const auto row = value.toObject();
        if (row.value("name").toString() != name) continue;
        ++matches;
        const auto digest = row.value("digest").toString();
        const QUrl url(row.value("browser_download_url").toString());
        const auto expectedPath = QStringLiteral("/Likely7/Veyra-NRVideo/releases/download/%1/%2")
            .arg(release.value("tag_name").toString(), name);
        const double size = row.value("size").toDouble();
        static const QRegularExpression checksum(QStringLiteral("^sha256:[a-fA-F0-9]{64}$"));
        if (!checksum.match(digest).hasMatch() || url.scheme() != "https" || url.host() != "github.com" ||
            !url.userInfo().isEmpty() || url.port() != -1 || !url.query().isEmpty() || !url.fragment().isEmpty() ||
            url.path() != expectedPath || size <= 0 || size > 8.0 * 1024 * 1024 * 1024 || size != double(qint64(size))) {
            error = tr("更新包缺少有效 SHA-256 或下载地址，请前往发布页手动更新"); return false;
        }
        candidate = {remote->text, name, digest.mid(7).toLower(), url, qint64(size)};
    }
    if (matches != 1) {
        asset = {}; error = tr("未找到唯一匹配的更新包，请前往发布页手动更新"); return false;
    }
    asset = candidate;
    return true;
}

OnlineUpdater::OnlineUpdater(QString currentVersion, QString installDirectory, QObject* parent,
                             QNetworkAccessManager* network)
    : QObject(parent), current_(std::move(currentVersion)), directory_(std::move(installDirectory)),
      flavor_(packageFlavor(directory_)), network_(network ? network : new QNetworkAccessManager(this)) {
    initUpdaterResources();
    // Official 2.0.7 packages retain the original "test-r2" display label.
    // Their numeric package identity prevents offering the same ZIP forever.
    const auto installed = version(packageInfo(directory_).value("version").toString());
    if (installed && !installed->preview) current_ = installed->text;
    if (!network) network_->setProxyFactory(new SystemProxyFactory);
    message_ = tr("点击检查正式版本更新");
}
OnlineUpdater::~OnlineUpdater() {
    if (reply_) { disconnect(reply_, nullptr, this, nullptr); reply_->abort(); }
}
void OnlineUpdater::setState(const QString& state, const QString& message) {
    state_ = state; message_ = message; emit changed();
}
void OnlineUpdater::fail(const QString& message) {
    archive_.close();
    setState(QStringLiteral("failed"), message);
}
void OnlineUpdater::check() {
    if (reply_ || state_ == "installing") return;
    asset_ = {}; progress_ = 0;
    if (flavor_.isEmpty()) {
        fail(tr("无法识别当前便携包类型，请前往发布页手动更新")); return;
    }
    setState(QStringLiteral("checking"), tr("正在检查更新…"));
    reply_ = network_->get(request(QUrl(QStringLiteral("https://api.github.com/repos/Likely7/Veyra-NRVideo/releases/latest"))));
    auto* reply = reply_.data();
    QTimer::singleShot(30000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 4 * 1024 * 1024) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply_ = nullptr; reply->deleteLater();
        if (state_ != "checking") return;
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            fail(tr("检查更新失败，请检查网络或稍后重试"));
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        QString error;
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail(tr("发布信息无效，请前往发布页手动更新")); return;
        }
        if (!selectRelease(document.object(), current_, flavor_, asset_, error)) { fail(error); return; }
        setState(asset_.name.isEmpty() ? QStringLiteral("current") : QStringLiteral("available"),
                 asset_.name.isEmpty() ? tr("当前已是最新正式版本") : tr("发现新版本 %1").arg(asset_.version));
    });
}
void OnlineUpdater::download() {
    if (reply_ || (state_ != "available" && state_ != "failed") || asset_.name.isEmpty()) return;
    work_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/veyra-update-XXXXXX"));
    if (!work_->isValid()) { fail(tr("无法创建更新临时目录")); return; }
    archive_.setFileName(work_->filePath(QStringLiteral("package.zip")));
    if (!archive_.open(QIODevice::WriteOnly)) { fail(tr("无法写入更新包，请检查磁盘空间")); return; }
    hash_.reset(); received_ = 0; progress_ = 0;
    setState(QStringLiteral("downloading"), tr("正在下载并校验更新包…"));
    auto req = request(asset_.url); req.setRawHeader("Accept", "application/octet-stream");
    reply_ = network_->get(req);
    auto* reply = reply_.data();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        const auto bytes = reply->readAll();
        received_ += bytes.size();
        if (received_ > asset_.size || archive_.write(bytes) != bytes.size()) {
            fail(tr("更新包写入失败或大小异常")); reply->abort(); return;
        }
        hash_.addData(bytes);
        progress_ = double(received_) / double(asset_.size); emit changed();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply_ = nullptr; reply->deleteLater();
        const bool flushed = archive_.isOpen() && archive_.flush(); archive_.close();
        if (state_ != "downloading") return;
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || !flushed) {
            fail(tr("下载失败，请检查网络和磁盘空间后重试")); return;
        }
        if (received_ != asset_.size || QString::fromLatin1(hash_.result().toHex()) != asset_.sha256) {
            fail(tr("更新包校验失败，已拒绝安装")); return;
        }
        setState(QStringLiteral("ready"), tr("更新包已校验，停止播放和导出后可退出安装"));
    });
}
void OnlineUpdater::cancel() {
    if (state_ == "installing" && work_) {
        QFile cancelFile(work_->filePath(QStringLiteral("cancel")));
        cancelFile.open(QIODevice::WriteOnly);
    }
    setState(QStringLiteral("idle"), tr("更新已取消"));
    if (reply_) reply_->abort();
    archive_.close();
}
void OnlineUpdater::install() {
    if (state_ != "ready" || !work_) return;
#ifndef Q_OS_WIN
    fail(tr("自动替换仅支持 Windows 便携包"));
#else
    QFile script(QStringLiteral(":/update/install-update.ps1"));
    QFile output(work_->filePath(QStringLiteral("install.ps1")));
    if (!script.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        fail(tr("无法准备更新安装程序")); return;
    }
    const auto scriptBytes = script.readAll();
    if (output.write(scriptBytes) != scriptBytes.size() || !output.flush()) {
        fail(tr("无法准备更新安装程序")); return;
    }
    output.close();
    const QJsonObject context{{"pid", double(QCoreApplication::applicationPid())}, {"directory", directory_},
        {"archive", archive_.fileName()}, {"sha256", asset_.sha256}, {"version", asset_.version},
        {"flavor", flavor_}, {"executable", QCoreApplication::applicationFilePath()},
        {"arguments", QJsonArray::fromStringList(QCoreApplication::arguments().mid(1))}};
    if (!saveJson(work_->filePath(QStringLiteral("context.json")), context)) { fail(tr("无法准备更新安装程序")); return; }
    const auto powershell = QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    if (!QProcess::startDetached(powershell, {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
        QStringLiteral("-File"), output.fileName(), QStringLiteral("-Context"), work_->filePath(QStringLiteral("context.json"))})) {
        fail(tr("无法启动更新安装程序")); return;
    }
    // The helper acknowledges after validating the context and capturing our process.
    // Never close Veyra merely because a PowerShell process was spawned.
    work_->setAutoRemove(false);
    setState(QStringLiteral("installing"), tr("正在准备退出安装…"));
    auto* timer = new QTimer(this); timer->setInterval(200);
    connect(timer, &QTimer::timeout, this, [this, timer, ticks = 0]() mutable {
        if (state_ != "installing") { timer->stop(); timer->deleteLater(); return; }
        if (QFileInfo::exists(work_->filePath(QStringLiteral("ready")))) {
            timer->stop(); timer->deleteLater(); emit installRequested();
        } else if (++ticks >= 1500 || QFileInfo::exists(work_->filePath(QStringLiteral("failed")))) {
            timer->stop(); timer->deleteLater();
            QFile cancelFile(work_->filePath(QStringLiteral("cancel"))); cancelFile.open(QIODevice::WriteOnly);
            fail(tr("安装程序未就绪，软件保持运行；请查看临时目录中的 update.log"));
        }
    });
    timer->start();
#endif
}
}
