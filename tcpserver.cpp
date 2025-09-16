#include "tcpserver.h"
#include <QTcpSocket>
#include <QMetaObject>
#include <QThreadPool>
#include <QDateTime>

//--------------------------------------------------------------------------------

WorkerTask::WorkerTask(QByteArray data, QWeakPointer<ConnectionHandler> conn)
    : data(std::move(data)), connection(conn) {}

void WorkerTask::run()
{
    if (auto conn = connection)
    {
        if (auto sh = connection.lock()) {
            sh->service(data);
        }
    }
}

//--------------------------------------------------------------------------------

ConnectionHandler::ConnectionHandler(QTcpSocket *sock, qint64 id, QObject *parent)
    : QObject(parent), socket_(sock)
{
    connectionId = id;

    connect(socket_, &QTcpSocket::readyRead, this, &ConnectionHandler::onReadyRead);
    connect(socket_, &QTcpSocket::disconnected, this, &ConnectionHandler::onDisconnected);
}

void ConnectionHandler::send(const QByteArray &data)
{
    QPointer<QTcpSocket> sock = socket_;
    if (!sock) return;

    QMetaObject::invokeMethod(
        sock,
        [sock, data]() {
            if (sock && sock->state() == QAbstractSocket::ConnectedState) {
                sock->write(data);
            }
        },
        Qt::QueuedConnection  // ensure it runs in socket's thread
        );
}

void ConnectionHandler::onReadyRead() {
    QByteArray data = socket_->readAll();
    auto *task = new WorkerTask(data, self_);
    QThreadPool::globalInstance()->start(task);
}

void ConnectionHandler::onDisconnected() {
    qDebug() << "Disconnected:" << socket_->peerAddress();

    ConnectionManager::instance().unregisterConnection(connectionId);

    socket_->deleteLater();

    // No need to deleteLater since QSharedPointer will do that.
    //deleteLater();
}

//--------------------------------------------------------------------------------

ConnectionManager &ConnectionManager::instance() {
    static ConnectionManager instance;
    return instance;
}

void ConnectionManager::setSessionId(qint64 cId, qint64 sessId)
{
    QMutexLocker locker(&mutex);

    // Ensure the connection exists and is alive
    auto conn = connections.value(cId);
    if (!conn) return;

    // If this connection already had a session, remove that reverse mapping
    if (connectionToSessionIDs.contains(cId)) {
        auto oldSess = connectionToSessionIDs[cId];
        if (oldSess != sessId) {
            sessionToConnectionIDs.remove(oldSess);
        }
    }

    // If this session was bound to another connection, sever that first
    if (sessionToConnectionIDs.contains(sessId)) {
        auto oldConn = sessionToConnectionIDs[sessId];
        connectionToSessionIDs.remove(oldConn);
    }

    // Bind both ways
    sessionToConnectionIDs[sessId] = cId;
    connectionToSessionIDs[cId] = sessId;
}

QSharedPointer<ConnectionHandler> ConnectionManager::Connection(qint64 id)
{
    QMutexLocker locker(&mutex);
    if (connections.contains(id) && connections[id]) {
        return connections[id];
    }
    return QSharedPointer<ConnectionHandler>(nullptr);
}

QSharedPointer<ConnectionHandler> ConnectionManager::ConnectionBySession(qint64 sid)
{
    QMutexLocker locker(&mutex);
    if (sessionToConnectionIDs.contains(sid))
    {
        auto cId = sessionToConnectionIDs[sid];
        if (connections.contains(cId) && connections[cId]) {
            return connections[cId];
        }
    }
    return QSharedPointer<ConnectionHandler>(nullptr);
}

void ConnectionManager::registerConnection(qint64 id, ConnectionHandler *conn) {
    QMutexLocker locker(&mutex);
    connections[id] = QSharedPointer<ConnectionHandler>(conn);
    connections[id]->setSelfWeak(connections[id].toWeakRef());

    // Auto-unregister when the socket disconnects or is destroyed
    connect(conn->socket(), &QTcpSocket::disconnected, this, [this, id]{
        unregisterConnection(id);
    });
    connect(conn->socket(), &QObject::destroyed, this, [this, id]{
        unregisterConnection(id);
    });
}

void ConnectionManager::unregisterConnection(qint64 id) {
    QMutexLocker locker(&mutex);

    if (!connections.contains(id))
        return;

    // Drop session mapping if present
    if (connectionToSessionIDs.contains(id)) {
        qint64 sid = connectionToSessionIDs.take(id);
        sessionToConnectionIDs.remove(sid);
    }

    // The QSharedPointer delete ConnectionHandler object.
    QSharedPointer<ConnectionHandler> doomed;

    // Drop session mappings as you do now...
    doomed = connections.take(id);    // last strong ref may be here
}

void ConnectionManager::sendToConnection(qint64 id, const QByteArray &data) {    
    QSharedPointer<ConnectionHandler> conn;
    { QMutexLocker lk(&mutex); conn = connections.value(id); }
    if (conn) conn->send(data);
}

void ConnectionManager::sendToSession(qint64 sid, const QByteArray &data)
{
    qint64 cId = -1;
    {
        QMutexLocker locker(&mutex);
        cId = sessionToConnectionIDs.value(sid, -1);
    }
    if (cId < 0) return;
    sendToConnection(cId, data);
}

void ConnectionManager::broadcast(const QByteArray &data) {    
    QList<QSharedPointer<ConnectionHandler>> list;
    { QMutexLocker lk(&mutex); list = connections.values(); }
    for (auto& conn : list) {
        if (conn) conn->send(data);
    }
}

//--------------------------------------------------------------------------------

TcpServer::TcpServer(QObject *parent)
    : QTcpServer(parent) {}

static qint64 nextId = QDateTime::currentMSecsSinceEpoch();

void TcpServer::incomingConnection(qintptr descriptor) {
    auto *socket = new QTcpSocket(this);
    if (socket->setSocketDescriptor(descriptor)) {
        qint64 connId = nextId++;

        auto *conn = createHandler(socket, connId, this);
        ConnectionManager::instance().registerConnection(connId, conn);

        qDebug() << "Connection" << connId << "connected from" << socket->peerAddress();
    } else {
        delete socket;
    }
}

//--------------------------------------------------------------------------------
