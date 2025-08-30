#ifndef CHANNEL_H
#define CHANNEL_H

#include <QMap>
#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

class Select;

class Channel {
public:
    void send(void* value);

    void* recv();

    void capture(int id, Select* s);

private:
    QQueue<void*> queue;
    QMutex mutex;
    QWaitCondition cond;

    int selectId;
    Select* selector{nullptr};
};

class Select
{
public:

    struct SelectChannel
    {
        int id;
        Channel* c;
    };

    Select(QList<SelectChannel>);
    ~Select();

    void send(int id, void* value);

    void capture(int (*funcptr)(int id, void* d));

    bool IsRun = true;

private:

    QList<Channel*> sources;

    struct ChannelData
    {
        int id;
        void* data;
    };

    ChannelData recv();

    QQueue<ChannelData> queue;
    QMutex mutex;
    QWaitCondition cond;
};

#endif // CHANNEL_H
