#include "SingleInstance.h"
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>

namespace er {

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}

QString SingleInstance::name() {
    // Per person (the socket lives in the runtime directory, which is theirs).
    return QStringLiteral("editroom-lx-") + qEnvironmentVariable("USER", QStringLiteral("user"));
}

bool SingleInstance::handOff(const QStringList& files) {
    QLocalSocket s;
    s.connectToServer(name());
    if (!s.waitForConnected(300)) return false;
    QStringList absolute;
    for (const QString& f : files) absolute << QFileInfo(f).absoluteFilePath();
    s.write(absolute.join(u'\n').toUtf8() + "\n\n");
    s.flush();
    s.waitForBytesWritten(1000);
    s.disconnectFromServer();
    return true;
}

bool SingleInstance::listen() {
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(name())) {
        // A copy that crashed left its socket behind.
        QLocalServer::removeServer(name());
        if (!server_->listen(name())) return false;
    }
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* s = server_->nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            connect(s, &QLocalSocket::readyRead, this, [this, s, buffer] {
                *buffer += s->readAll();
                if (!buffer->endsWith("\n\n")) return;
                QStringList files;
                for (const QByteArray& line : buffer->split('\n')) if (!line.isEmpty()) files << QString::fromUtf8(line);
                emit opened(files);
                s->disconnectFromServer();
            });
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
        }
    });
    return true;
}

} // namespace er
