#include "tcpserver.h"
#include <QTcpSocket>
#include <QMetaObject>
#include <QThreadPool>
#include <QDateTime>

//--------------------------------------------------------------------------------

WorkerTask::WorkerTask(QByteArray data, QPointer<QTcpSocket> socket)
    : data(std::move(data)), socket(socket) {}

void WorkerTask::run() {
    // Simulate CPU work (replace with real logic)
    QByteArray result = "Processed: " + data.toUpper();

    if (socket) {
        // Post result back to socket's thread
        QMetaObject::invokeMethod(socket, [this, result]() {
            if (socket->state() == QTcpSocket::ConnectedState) {
                socket->write(result);
            }
        });
    }
}

//--------------------------------------------------------------------------------

ConnectionHandler::ConnectionHandler(QTcpSocket *sock, QObject *parent)
    : QObject(parent), socket(sock) {
    connect(socket, &QTcpSocket::readyRead, this, &ConnectionHandler::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &ConnectionHandler::onDisconnected);
}

void ConnectionHandler::onReadyRead() {
    QByteArray data = socket->readAll();
    auto *task = new WorkerTask(data, socket);
    QThreadPool::globalInstance()->start(task);
}

void ConnectionHandler::onDisconnected() {
    qDebug() << "Disconnected:" << socket->peerAddress();
    socket->deleteLater();
    deleteLater();
}

//--------------------------------------------------------------------------------

ConnectionManager &ConnectionManager::instance() {
    static ConnectionManager instance;
    return instance;
}

void ConnectionManager::registerConnection(qint64 id, QTcpSocket *socket) {
    QMutexLocker locker(&mutex);
    connections[id] = socket;

    // Auto-unregister when the socket disconnects or is destroyed
    connect(socket, &QTcpSocket::disconnected, this, [this, id]{
        unregisterConnection(id);
    });
    connect(socket, &QObject::destroyed, this, [this, id]{
        unregisterConnection(id);
    });
}

void ConnectionManager::unregisterConnection(qint64 id) {
    QMutexLocker locker(&mutex);

    if(connections.contains(id))
    {
        auto conn = connections[id];
        auto strUserNID = conn->objectName();
        if(strUserNID.length() > 0)
        {
            auto userNID = strUserNID.toInt();
            connectionByUserNIDs.remove(userNID);
        }
        connections.remove(id);
    }
}

void ConnectionManager::sendToConnection(qint64 id, const QByteArray &data) {
    QMutexLocker locker(&mutex);
    if (connections.contains(id) && connections[id]) {
        QMetaObject::invokeMethod(connections[id], [sock = connections[id], data]() {
            if (sock && sock->state() == QTcpSocket::ConnectedState)
                sock->write(data);
        });
    }
}

void ConnectionManager::broadcast(const QByteArray &data) {
    QMutexLocker locker(&mutex);
    auto allConns = connections.values();
    for (auto &sock : allConns) {
        if (sock) {
            QMetaObject::invokeMethod(sock, [sock, data]() {
                if (sock && sock->state() == QTcpSocket::ConnectedState)
                    sock->write(data);
            });
        }
    }
}

//--------------------------------------------------------------------------------

TcpServer::TcpServer(QObject *parent)
    : QTcpServer(parent) {}

static qint64 nextId = QDateTime::currentMSecsSinceEpoch();

void TcpServer::incomingConnection(qintptr descriptor) {
    auto *socket = new QTcpSocket(this);
    if (socket->setSocketDescriptor(descriptor)) {
        qint64 clientId = nextId++;
        ConnectionManager::instance().registerConnection(clientId, socket);

        auto *handler = new ConnectionHandler(socket, this);
        handler->setClientId(clientId); // optional

        qDebug() << "Client" << clientId << "connected from" << socket->peerAddress();
    } else {
        delete socket;
    }
}

//--------------------------------------------------------------------------------
