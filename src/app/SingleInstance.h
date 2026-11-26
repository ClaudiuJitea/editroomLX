// One Editroom per person, as on the Mac: a second launch (opening a file from the file manager, say) hands its
// files to the copy that's running, which brings its window forward, and quits.
#pragma once
#include <QObject>
#include <QStringList>

class QLocalServer;

namespace er {

class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject* parent = nullptr);
    /// True when another copy is running and took `files` (this one should quit).
    static bool handOff(const QStringList& files);
    /// Starts listening for later launches.
    bool listen();
signals:
    /// A later launch asked for these to be opened (empty: just come forward).
    void opened(const QStringList& files);
private:
    static QString name();
    QLocalServer* server_ = nullptr;
};

} // namespace er
