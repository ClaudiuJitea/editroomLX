#include "Updates.h"
#include "Alert.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>

namespace er {

QString Updates::releasesURL() { return QStringLiteral("https://github.com/%1/releases").arg(QLatin1String(repository)); }
QString Updates::latestURL() { return QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(QLatin1String(repository)); }

Updates& Updates::shared() {
    static Updates* u = new Updates;
    return *u;
}

bool Updates::automatic() const { return QSettings().value(QStringLiteral("updatesAutomatic"), false).toBool(); }
void Updates::setAutomatic(bool on) { QSettings().setValue(QStringLiteral("updatesAutomatic"), on); }

std::optional<QDateTime> Updates::lastCheck() const {
    const QDateTime t = QSettings().value(QStringLiteral("updatesLastChecked")).toDateTime();
    if (!t.isValid()) return std::nullopt;
    return t;
}

int Updates::compare(const QString& a, const QString& b) {
    auto parts = [](QString v) {
        v = v.trimmed();
        if (v.startsWith(u'v') || v.startsWith(u'V')) v = v.mid(1);
        v = v.section(QRegularExpression(QStringLiteral("[-+ ]")), 0, 0);
        std::vector<int> out;
        for (const QString& p : v.split(u'.')) out.push_back(p.toInt());
        return out;
    };
    const auto x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

Updates::Result Updates::read(const QByteArray& json, int httpStatus, const QString& installed) {
    Result r;
    if (httpStatus == 404) {
        // No release published yet: nothing newer than this one.
        r.ok = true;
        r.none = true;
        r.version = installed;
        return r;
    }
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    const QString tag = o.value("tag_name").toString();
    if (httpStatus != 200 || tag.isEmpty()) {
        r.error = o.value("message").toString(QStringLiteral("GitHub didn't answer as expected."));
        return r;
    }
    r.ok = true;
    r.version = tag.startsWith(u'v') || tag.startsWith(u'V') ? tag.mid(1) : tag;
    r.name = o.value("name").toString(tag);
    r.notes = o.value("body").toString();
    r.url = o.value("html_url").toString(releasesURL());
    r.newer = compare(r.version, installed) > 0;
    return r;
}

void Updates::check() {
    if (checking_) return;
    checking_ = true;
    if (!net_) net_ = new QNetworkAccessManager(this);
    QNetworkRequest req{QUrl(latestURL())};
    req.setRawHeader("Accept", "application/vnd.github+json");
    // A generic name: nothing about the person goes with the request.
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Editroom"));
    req.setTransferTimeout(15000);
    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        checking_ = false;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        Result r;
        if (reply->error() != QNetworkReply::NoError && status != 404 && status == 0) {
            r.error = reply->errorString();
        } else {
            r = read(reply->readAll(), status, QCoreApplication::applicationVersion());
        }
        if (r.ok) QSettings().setValue(QStringLiteral("updatesLastChecked"), QDateTime::currentDateTimeUtc());
        emit checked(r);
    });
}

namespace {
void tell(QWidget* parent, const Updates::Result& r, bool quiet) {
    const QString installed = QCoreApplication::applicationVersion();
    if (!r.ok) {
        if (!quiet) Alert::run(parent, QStringLiteral("Update Error!"), QStringLiteral("EditroomLX couldn't check for updates: %1").arg(r.error), {QStringLiteral("OK")});
        return;
    }
    if (r.none) {
        if (!quiet) Alert::run(parent, QStringLiteral("No releases yet"),
                               QStringLiteral("There's no EditroomLX release on GitHub to update to yet. You have %1.").arg(installed), {QStringLiteral("OK")});
        return;
    }
    if (!r.newer) {
        if (!quiet) Alert::run(parent, QStringLiteral("You're up to date!"), QStringLiteral("EditroomLX %1 is currently the newest version available.").arg(installed), {QStringLiteral("OK")});
        return;
    }
    QString notes = r.notes.trimmed();
    if (notes.size() > 600) notes = notes.left(600).trimmed() + QStringLiteral("…");
    const QString text = QStringLiteral("EditroomLX %1 is now available—you have %2. Would you like to download it now?").arg(r.version, installed)
                       + (notes.isEmpty() ? QString() : QStringLiteral("\n\n") + notes);
    if (Alert::run(parent, QStringLiteral("A new version of EditroomLX is available!"), text, {QStringLiteral("View Release"), QStringLiteral("Remind Me Later")}) == 0)
        QDesktopServices::openUrl(QUrl(r.url));
}
} // namespace

void Updates::checkAndTell(QWidget* parent) {
    QPointer<QWidget> p(parent);
    auto* ctx = new QObject(this);
    connect(this, &Updates::checked, ctx, [ctx, p](const Result& r) {
        ctx->deleteLater();
        tell(p, r, false);
    });
    check();
}

void Updates::checkIfDue(QWidget* parent) {
    if (!automatic()) return;
    if (auto last = lastCheck(); last && last->secsTo(QDateTime::currentDateTimeUtc()) < 24 * 3600) return;
    QPointer<QWidget> p(parent);
    auto* ctx = new QObject(this);
    connect(this, &Updates::checked, ctx, [ctx, p](const Result& r) {
        ctx->deleteLater();
        tell(p, r, true);
    });
    check();
}

} // namespace er
