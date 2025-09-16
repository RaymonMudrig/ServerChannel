/*
Working model of this library.
- QTcpSocket based server.
- ConnectionManager class expose sendToConnection and broadcast.
- ConnectionHandler class handles readyRead and disconnect signals.
- WorkerThread class descendant of QRunnable spawned by ConnectionHandler to perform task on other threads.
- TcpServer class descendant of QTcpServer handles incomingConnection signals. Assign id for each incoming connection and put on ConnectionManager.

# Workflow

The client-server dynamic works in both pushed and query-response manner.
Server connected to upper layer of the system using single pipeline messaging, and accept many client connection from down layer.

diagram as follows:

UpperLayer <--> Server <--+-- Client
                          +-- Client
                          +-- Client
                          +-- Client

## Login and Session creation
Upon connected client will send Logon message, handled by pushing it upstream (with connectionId attached to the message).
When Logon accepted server should stamp this connection with clientId identification.

## Push data dynamic
When server accept data from upstream and have to pushed it down, it will search all relevant connection using clientId identification.
Selection criteria based on somekind of mapping, but its outside of this topic.

## Query-response dynamic
Client can send query to server.
Server can perform check based on clientId of the connection, if valid response will produced.

# Structure

There are 2 ids, first is connectionId which assigned during connection creation,
second sessionId which assigned when logon is succeeded. One sessionId will on have one connectionId (one to one).

ConnectionHandler (and its descendants) holds operational and session states (and data) as well as sending and receiving data from socket.
Descendant class of ConnectionHandler will provide handler of incoming data/message via service() function.

ConnectionManager holds and manage all ConnectionHandlers. In push data flow it provide means to find relevant connection,
based on connectionId or sessionId.

TcpServer handler inbound connection and create correct ConnectionHandler (or its descendant).
Its descendant should handle messaging with upper layer in the system

Usage of these classes is by deriving ConnectionHandler and TcpServer for specific purpose.

 */

#pragma once
#include <QRunnable>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QTcpSocket>
#include <QTcpServer>
#include <QMutex>

class ConnectionHandler;

//--------------------------------------------------------------------------------

class WorkerTask : public QRunnable {
public:
    WorkerTask(QByteArray data, QWeakPointer<ConnectionHandler> conn);

    void run() override;

private:
    QByteArray data;
    QWeakPointer<ConnectionHandler> connection;
};

//--------------------------------------------------------------------------------
/*!
 * \brief ConnectionHandler class provide 3 purposes:
 *        - Holds QTcpSocket object.
 *        - Handle send and receiving data.
 *        - Provide base class for statefull session object.
 *
 *        Implement sevice() function to handle incoming data with thread pool.
 */
class ConnectionHandler : public QObject {
    Q_OBJECT
public:
    explicit ConnectionHandler(QTcpSocket *socket, qint64 id, QObject *parent = nullptr);

    QPointer<QTcpSocket> socket() { return socket_; }

    void setSessionId(qint64 id) { sessionId = id; }

    void setSelfWeak(QWeakPointer<ConnectionHandler> w) { self_ = std::move(w); }

    void send(const QByteArray &data);

    //! Implement this to handle incoming messages
    virtual void service(const QByteArray &data) { Q_UNUSED(data); }

private slots:

    void onReadyRead();
    void onDisconnected();

private:

    QPointer<QTcpSocket> socket_;

    qint64 connectionId;
    qint64 sessionId;

    QWeakPointer<ConnectionHandler> self_;
};

//--------------------------------------------------------------------------------

class ConnectionManager : public QObject {
    Q_OBJECT
public:
    static ConnectionManager &instance();

    void registerConnection(qint64 id, ConnectionHandler *conn);
    void unregisterConnection(qint64 id);

    void setSessionId(qint64 connectionId, qint64 sessionId);

    QSharedPointer<ConnectionHandler> Connection(qint64 id);
    QSharedPointer<ConnectionHandler> ConnectionBySession(qint64 id);

    void sendToConnection(qint64 id, const QByteArray &data);
    void sendToSession(qint64 id, const QByteArray &data);

    void broadcast(const QByteArray &data);

private:

    QMutex mutex;

    // Maps connectionID to socket
    QMap<qint64, QSharedPointer<ConnectionHandler>> connections;

    // Maps sessionId to connectionID
    QMap<qint64, qint64> sessionToConnectionIDs;

    // Maps connectionID to sessionId
    QMap<qint64, qint64> connectionToSessionIDs;

    ConnectionManager() = default;
};

//--------------------------------------------------------------------------------

/*!
 * \brief TcpServer class provide QTcpServer wrapper for
 *        - Open listener port
 *        - Handle incoming connection
 *        - Creation of ConnectionHandler (or its descendant)
 *        Implement createHandler() function to provide specific handler.
 */
class TcpServer : public QTcpServer {
    Q_OBJECT
public:
    explicit TcpServer(QObject *parent = nullptr);

    //! Implement this to create your custom ConnectionHandler.
    virtual ConnectionHandler* createHandler(QTcpSocket *socket, qint64 id, QObject *parent = nullptr)
    {
        return new ConnectionHandler(socket, id, parent);
    }

protected:

    void incomingConnection(qintptr socketDescriptor) override;
};

//--------------------------------------------------------------------------------
