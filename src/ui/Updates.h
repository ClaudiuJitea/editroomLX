// Updates (Updates.swift and UpdateDriver.swift, Sparkle on the Mac): checking GitHub for a newer Editroom LX. It asks
// the GitHub API for the latest release of ClaudiuJitea/editroomLX and compares version numbers. Nothing goes online
// unless the person asks (Help › Check for Updates…, or the button in Settings › Updates), or turns on automatic
// checks there (off until then; at most once a day, when Editroom starts).
#pragma once
#include <QDateTime>
#include <QObject>
#include <QString>
#include <optional>

class QNetworkAccessManager;
class QWidget;

namespace er {

class Updates : public QObject {
    Q_OBJECT
public:
    static constexpr const char* repository = "ClaudiuJitea/editroomLX";
    static QString releasesURL();
    static QString latestURL();
    static Updates& shared();

    struct Result {
        bool ok = false;
        bool newer = false;
        QString version;      // the latest release's version ("1.1.0")
        QString name, notes, url;
        QString error;
        /// GitHub has no release to offer (none published yet, or the repository isn't public).
        bool none = false;
    };

    bool automatic() const;
    void setAutomatic(bool on);
    std::optional<QDateTime> lastCheck() const;
    bool checking() const { return checking_; }

    /// Asks GitHub now; `checked` follows.
    void check();
    /// Help › Check for Updates…: checks, then says what it found, over `parent`.
    void checkAndTell(QWidget* parent);
    /// When Editroom starts: checks quietly if automatic checks are on and the last was a day ago or more, and
    /// says something only when there's a newer version.
    void checkIfDue(QWidget* parent);

    /// "1.2.10" against "1.2.9" (a leading v and anything after a dash or plus are left out).
    static int compare(const QString& a, const QString& b);
    /// The answer of GitHub's releases/latest, read (tests).
    static Result read(const QByteArray& json, int httpStatus, const QString& installed);

signals:
    void checked(const Result& result);

private:
    Updates() = default;
    QNetworkAccessManager* net_ = nullptr;
    bool checking_ = false;
};

} // namespace er
