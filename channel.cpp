#include "channel.h"

void Channel::send(void* value) {
    if(selector)
    {
        selector->send(selectId, value);
        return;
    }

    QMutexLocker locker(&mutex);
    queue.enqueue(value);
    cond.wakeOne();
}

void* Channel::recv() {
    QMutexLocker locker(&mutex);
    while (queue.isEmpty()) {
        cond.wait(&mutex);
    }
    return queue.dequeue();
}

void Channel::capture(int id, Select* s)
{
    selectId = id;
    selector = s;
}

Select::Select(QList<SelectChannel> c)
{
    for(auto &cc: c)
    {
        cc.c->capture(cc.id, this);
        sources.append(cc.c);
    }
}
Select::~Select()
{
    for(auto &c: sources)
    {
        c->capture(0, nullptr);
    }
}

void Select::send(int id, void* value)
{
    QMutexLocker locker(&mutex);
    queue.enqueue({id, value});
    cond.wakeOne();
}

Select::ChannelData Select::recv()
{
    QMutexLocker locker(&mutex);
    while (queue.isEmpty()) {
        cond.wait(&mutex);
    }
    return queue.dequeue();
}

void Select::capture(int (*funcptr)(int id, void* d))
{
    while(true)
    {
        auto d = recv();

        if(funcptr(d.id, d.data))
            break;
    }
}
