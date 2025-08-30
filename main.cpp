#include <QCoreApplication>
#include <QtConcurrent/QtConcurrent>

#include "channel.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    Channel chn;
    Channel ctx;

    chn.send(new int(5));

    Select sel({
        {1, &chn},
        {9, &ctx}
    });

    auto f = QtConcurrent::run([&sel](){
        sel.capture([](int i, void* val) -> int {
            switch(i)
            {
            case 1:
                return 0;
            case 9:
                return 1;
            }
            return 0;
        });
    });

    return a.exec();
}
