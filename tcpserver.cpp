#include "tcpserver.h"
#include <QTcpSocket>
#include <QMetaObject>
#include <QThreadPool>

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

void ConnectionManager::registerClient(int id, QTcpSocket *socket) {
    QMutexLocker locker(&mutex);
    clients[id] = socket;
}

void ConnectionManager::unregisterClient(int id) {
    QMutexLocker locker(&mutex);
    clients.remove(id);
}

void ConnectionManager::sendToClient(int id, const QByteArray &data) {
    QMutexLocker locker(&mutex);
    if (clients.contains(id) && clients[id]) {
        QMetaObject::invokeMethod(clients[id], [sock = clients[id], data]() {
            if (sock && sock->state() == QTcpSocket::ConnectedState)
                sock->write(data);
        });
    }
}

void ConnectionManager::broadcast(const QByteArray &data) {
    QMutexLocker locker(&mutex);
    for (auto sock : clients.values()) {
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

static int nextId = 1;

void TcpServer::incomingConnection(qintptr descriptor) {
    auto *socket = new QTcpSocket(this);
    if (socket->setSocketDescriptor(descriptor)) {
        int clientId = nextId++;
        ConnectionManager::instance().registerClient(clientId, socket);
        auto *handler = new ConnectionHandler(socket, this);
        handler->setClientId(clientId); // optional
        qDebug() << "Client" << clientId << "connected from" << socket->peerAddress();
    } else {
        delete socket;
    }
}

//--------------------------------------------------------------------------------
