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

    void setClientId(int id) { client_id = id; }

private slots:
    void onReadyRead();
    void onDisconnected();

private:
    QTcpSocket *socket;

    int client_id;
};

//--------------------------------------------------------------------------------

class ConnectionManager : public QObject {
    Q_OBJECT
public:
    static ConnectionManager &instance();

    void registerClient(int id, QTcpSocket *socket);
    void unregisterClient(int id);
    void sendToClient(int id, const QByteArray &data);
    void broadcast(const QByteArray &data);

private:
    QMap<int, QPointer<QTcpSocket>> clients;
    QMutex mutex;

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
