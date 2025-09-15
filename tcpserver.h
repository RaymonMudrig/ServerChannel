#ifndef TCPSERVER_H
#define TCPSERVER_H

#pragma once

#include <QRunnable>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QTcpSocket>
#include <QTcpServer>
#include <QMutex>

//--------------------------------------------------------------------------------

class WorkerTask : public QRunnable {
public:
    WorkerTask(QByteArray data, QPointer<QTcpSocket> socket);

    void run() override;

private:
    QByteArray data;
    QPointer<QTcpSocket> socket;
};

//--------------------------------------------------------------------------------

class ConnectionHandler : public QObject {
    Q_OBJECT
public:
    explicit ConnectionHandler(QTcpSocket *socket, QObject *parent = nullptr);

    void setClientId(qint64 id) { client_id = id; }

private slots:
    void onReadyRead();
    void onDisconnected();

private:
    QTcpSocket *socket;

    qint64 client_id;
};

//--------------------------------------------------------------------------------

class ConnectionManager : public QObject {
    Q_OBJECT
public:
    static ConnectionManager &instance();

    void registerConnection(qint64 id, QTcpSocket *socket);
    void unregisterConnection(qint64 id);
    void sendToConnection(qint64 id, const QByteArray &data);
    void broadcast(const QByteArray &data);

private:

    // Maps socket to connectionID
    QMap<qint64, QPointer<QTcpSocket>> connections;
    QMutex mutex;

    // Maps socket to UserNID
    QMap<int, QPointer<QTcpSocket>> connectionByUserNIDs;

    ConnectionManager() = default;
};

//--------------------------------------------------------------------------------

class TcpServer : public QTcpServer {
    Q_OBJECT
public:
    explicit TcpServer(QObject *parent = nullptr);

protected:
    void incomingConnection(qintptr socketDescriptor) override;
};

//--------------------------------------------------------------------------------

#endif // TCPSERVER_H
