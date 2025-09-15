#ifndef SINGLEACCESS_H
#define SINGLEACCESS_H

#include <QObject>
#include <QThread>
#include <QPointer>
#include <QSharedPointer>
#include <QReadWriteLock>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QEventLoop>
#include <QMap>
#include <QSet>
#include <QAtomicInt>
#include <QFile>
#include <memory>
#include <type_traits>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QThreadStorage>
#include <QVector>

//--------------------------------------------------------------------------------
class SingleAccess {
    QSharedPointer<QReadWriteLock> lock_;
public:
    SingleAccess() : lock_(new QReadWriteLock) {}
    virtual ~SingleAccess() = default;

    QSharedPointer<QReadWriteLock> Lock() const { return lock_; }

    virtual QByteArray Serialize() const = 0;            // const
    virtual void Deserialize(const QByteArray&) = 0;     // const-ref
};

//--------------------------------------------------------------------------------
template<class T>
class SingleAccessPtr : public QPointer<T>
{
    static_assert(std::is_base_of_v<QObject, T>, "T must inherit QObject");
    static_assert(std::is_base_of_v<SingleAccess, T>, "T must inherit SingleAccess");

    QSharedPointer<QReadWriteLock> lock_;
    std::unique_ptr<QReadLocker>   locker_;  // RAII, movable

public:
    explicit SingleAccessPtr(T* obj = nullptr)
        : QPointer<T>(obj)
        , lock_(obj ? obj->Lock() : QSharedPointer<QReadWriteLock>())
        , locker_(lock_ ? std::make_unique<QReadLocker>(lock_.data()) : nullptr) {}

    // const-only surface
    const T* operator->() const { return QPointer<T>::data(); }
    const T& operator*()  const { return *QPointer<T>::data(); }

    // forbid non-const access through the read guard
    T* operator->() = delete;
    T& operator*()  = delete;

    // Non-copyable (locker is non-copyable).
    SingleAccessPtr(const SingleAccessPtr&) = delete;
    SingleAccessPtr& operator=(const SingleAccessPtr&) = delete;

    // Movable is fine.
    SingleAccessPtr(SingleAccessPtr&& other) noexcept
        : QPointer<T>(other.data())
        , lock_(std::move(other.lock_))
        , locker_(std::move(other.locker_)) {
        other.QPointer<T>::operator=(nullptr);
    }
    SingleAccessPtr& operator=(SingleAccessPtr&& other) noexcept {
        if (this != &other) {
            QPointer<T>::operator=(other.data());
            lock_   = std::move(other.lock_);
            locker_ = std::move(other.locker_);
            other.QPointer<T>::operator=(nullptr);
        }
        return *this;
    }
};

//--------------------------------------------------------------------------------
template<class T>
class SingleAccessWPtr : public QPointer<T>
{
    static_assert(std::is_base_of_v<QObject, T>, "T must inherit QObject");
    static_assert(std::is_base_of_v<SingleAccess, T>, "T must inherit SingleAccess");

    QSharedPointer<QReadWriteLock> lock_;
    std::unique_ptr<QWriteLocker>  locker_;

public:
    explicit SingleAccessWPtr(T* obj = nullptr)
        : QPointer<T>(obj)
        , lock_(obj ? obj->Lock() : QSharedPointer<QReadWriteLock>())
        , locker_(lock_ ? std::make_unique<QWriteLocker>(lock_.data()) : nullptr) {}

    // Non-copyable (locker is non-copyable).
    SingleAccessWPtr(const SingleAccessWPtr&) = delete;
    SingleAccessWPtr& operator=(const SingleAccessWPtr&) = delete;

    // Movable is fine.
    SingleAccessWPtr(SingleAccessWPtr&& other) noexcept
        : QPointer<T>(other.data())
        , lock_(std::move(other.lock_))
        , locker_(std::move(other.locker_)) {
        other.QPointer<T>::operator=(nullptr);
    }
    SingleAccessWPtr& operator=(SingleAccessWPtr&& other) noexcept {
        if (this != &other) {
            QPointer<T>::operator=(other.data());
            lock_   = std::move(other.lock_);
            locker_ = std::move(other.locker_);
            other.QPointer<T>::operator=(nullptr);
        }
        return *this;
    }
};

//--------------------------------------------------------------------------------
/*
 * Example of class that can be managed by SingleAccessRepo.
 *
 * class Entity : public QObject, public SingleAccess {
 *     Q_OBJECT
 * public:
 *     using QObject::QObject;
 *     int DataInt;
 *     QString DataStr;
 * };
*/
//--------------------------------------------------------------------------------

// ---- SQLite-backed SingleAccessRepo (C++17) ----

template<class E>
class SingleAccessRepo {
    static_assert(std::is_base_of_v<QObject, E>,      "E must inherit QObject");
    static_assert(std::is_base_of_v<SingleAccess, E>, "E must inherit SingleAccess");

    // RAM-resident only
    QMap<int, E*>   allEntity;
    // ids currently being serialized (in-flight)
    QSet<int>       swappingOut;

    // Protects allEntity + swappingOut
    QReadWriteLock  lock_;

    // For wait/wake (in-flight swaps)
    QMutex          inflightMx;
    QWaitCondition  inflightCv;

    // DB bits
    QByteArray      name;           // table name (sanitized)
    QString         dbPath;         // sqlite filename (e.g. "/var/lib/mydb.sqlite3")

    // Per-thread connection factory
    QSqlDatabase dbForThread() {
        // One connection per thread; connection names must be unique per thread.
        static QThreadStorage<QString> tlsConnName;
        if (!tlsConnName.hasLocalData()) {
            QString conn = QStringLiteral("repo_%1_%2")
                               .arg(reinterpret_cast<qulonglong>(this), 0, 16)
                               .arg(reinterpret_cast<qulonglong>(QThread::currentThreadId()), 0, 16);
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(dbPath);
            if (!db.open()) {
                // You may want to handle or log db.lastError() here.
            } else {
                // Pragmas for read-mostly + decent concurrency
                QSqlQuery q(db);
                q.exec(QStringLiteral("PRAGMA journal_mode=WAL;"));
                q.exec(QStringLiteral("PRAGMA synchronous=NORMAL;"));
                q.exec(QStringLiteral("PRAGMA temp_store=MEMORY;"));
                q.exec(QStringLiteral("PRAGMA mmap_size=268435456;")); // 256 MB, adjust as needed
                q.exec(QStringLiteral("PRAGMA page_size=4096;"));      // match FS, adjust if you init DB fresh
            }
            tlsConnName.setLocalData(conn);
        }
        return QSqlDatabase::database(tlsConnName.localData());
    }

    QString tableName() const {
        // sanitize name -> [A-Za-z0-9_]+
        QByteArray src = name;
        QByteArray out;
        out.reserve(src.size());
        for (char c : src) {
            if ((c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_') out.push_back(c);
            else out.push_back('_');
        }
        if (out.isEmpty()) out = "entities";
        return QString::fromUtf8(out);
    }

    bool ensureTable() {
        QSqlDatabase db = dbForThread();
        if (!db.isOpen()) return false;
        QSqlQuery q(db);
        const QString sql =
            QStringLiteral("CREATE TABLE IF NOT EXISTS %1("
                           "Id INTEGER PRIMARY KEY, "
                           "raw BLOB NOT NULL)").arg(tableName());
        return q.exec(sql);
    }

    // Wait until 'id' is no longer in swappingOut
    void waitWhileSwapping(int id) {
        for (;;) {
            {
                QReadLocker r(&lock_);
                if (!swappingOut.contains(id)) break;
            }
            QMutexLocker g(&inflightMx);
            inflightCv.wait(&inflightMx, 10);
        }
    }

    // Load raw blob for id from SQLite; returns false if not found or error
    bool dbLoad(int id, QByteArray &outRaw) {
        if (!ensureTable()) return false;
        QSqlDatabase db = dbForThread();
        QSqlQuery q(db);
        const QString sql = QStringLiteral("SELECT raw FROM %1 WHERE Id=?").arg(tableName());
        if (!q.prepare(sql)) return false;
        q.addBindValue(id);
        if (!q.exec()) return false;
        if (!q.next()) return false;
        outRaw = q.value(0).toByteArray();
        return true;
    }

    // Upsert raw blob for id into SQLite (used by SwapOut and/or snapshots)
    bool dbUpsert(int id, const QByteArray &raw) {
        if (!ensureTable()) return false;
        QSqlDatabase db = dbForThread();
        QSqlQuery q(db);
        const QString sql = QStringLiteral("INSERT OR REPLACE INTO %1(Id,raw) VALUES(?,?)")
                                .arg(tableName());
        if (!q.prepare(sql)) return false;
        q.addBindValue(id);
        q.addBindValue(raw);
        return q.exec();
    }

    // Delete row for id from SQLite
    bool dbDelete(int id) {
        if (!ensureTable()) return false;
        QSqlDatabase db = dbForThread();
        QSqlQuery q(db);
        const QString sql = QStringLiteral("DELETE FROM %1 WHERE Id=?").arg(tableName());
        if (!q.prepare(sql)) return false;
        q.addBindValue(id);
        return q.exec();
    }

    // Bulk purge table (used by Clear/ClearAndWait)
    bool dbDeleteAll() {
        if (!ensureTable()) return false;
        QSqlDatabase db = dbForThread();
        QSqlQuery q(db);
        const QString sql = QStringLiteral("DELETE FROM %1").arg(tableName());
        return q.exec(sql);
    }

public:
    explicit SingleAccessRepo(QByteArray tableNameUtf8, const QString& sqlitePath)
        : name(std::move(tableNameUtf8)), dbPath(sqlitePath) {}

    int Count() {
        QReadLocker _(&lock_);
        return allEntity.count() + swappingOut.count();
    }

    // --- Read guard ---
    SingleAccessPtr<E> Get(int id) {
        // 1) RAM hit?
        {
            QReadLocker mapR(&lock_);
            typename QMap<int,E*>::const_iterator it = allEntity.find(id);
            if (it != allEntity.end())
                return SingleAccessPtr<E>(it.value());
        }

        // 2) If mid-swap, wait and retry RAM
        waitWhileSwapping(id);
        {
            QReadLocker mapR(&lock_);
            typename QMap<int,E*>::const_iterator it = allEntity.find(id);
            if (it != allEntity.end())
                return SingleAccessPtr<E>(it.value());
        }

        // 3) Try DB
        QByteArray raw;
        if (!dbLoad(id, raw)) {
            return SingleAccessPtr<E>(nullptr); // not found in DB
        }

        // 4) Materialize and insert
        std::unique_ptr<E> fresh(new E);
        fresh->Deserialize(raw);
        E* e = fresh.release();
        {
            QWriteLocker mapW(&lock_);
            typename QMap<int,E*>::iterator it = allEntity.find(id);
            if (it != allEntity.end()) { delete e; return SingleAccessPtr<E>(it.value()); }
            allEntity.insert(id, e);
        }
        return SingleAccessPtr<E>(e);
    }

    // --- Write guard ---
    SingleAccessWPtr<E> GetW(int id) {
        // 1) RAM hit?
        {
            QReadLocker mapR(&lock_);
            typename QMap<int,E*>::const_iterator it = allEntity.find(id);
            if (it != allEntity.end())
                return SingleAccessWPtr<E>(it.value());
        }

        // 2) If mid-swap, wait and retry RAM
        waitWhileSwapping(id);
        {
            QReadLocker mapR(&lock_);
            typename QMap<int,E*>::const_iterator it = allEntity.find(id);
            if (it != allEntity.end())
                return SingleAccessWPtr<E>(it.value());
        }

        // 3) Prefer DB row if present; else create empty
        QByteArray raw;
        std::unique_ptr<E> fresh(new E);
        if (dbLoad(id, raw)) {
            fresh->Deserialize(raw);
        }
        E* e = fresh.release();
        {
            QWriteLocker mapW(&lock_);
            typename QMap<int,E*>::iterator it = allEntity.find(id);
            if (it != allEntity.end()) { delete e; return SingleAccessWPtr<E>(it.value()); }
            allEntity.insert(id, e);
        }
        return SingleAccessWPtr<E>(e);
    }

    // --- Create-or-load (respects DB, then RAM) ---
    SingleAccessWPtr<E> Create(int id,
                               QThread* targetThread = QThread::currentThread(),
                               QObject* parentForEntity = nullptr) {
        // RAM?
        {
            QReadLocker mapR(&lock_);
            typename QMap<int,E*>::const_iterator it = allEntity.find(id);
            if (it != allEntity.end())
                return SingleAccessWPtr<E>(it.value());
        }

        // If mid-swap, wait & retry RAM
        waitWhileSwapping(id);
        {
            QReadLocker mapR(&lock_);
            typename QMap<int,E*>::const_iterator it = allEntity.find(id);
            if (it != allEntity.end())
                return SingleAccessWPtr<E>(it.value());
        }

        // Load from DB if exists, else create fresh
        QByteArray raw;
        std::unique_ptr<E> fresh(new E(parentForEntity));
        if (fresh->thread() != targetThread)
            fresh->moveToThread(targetThread);

        if (dbLoad(id, raw)) {
            fresh->Deserialize(raw);
        }

        E* e = fresh.release();
        {
            QWriteLocker mapW(&lock_);
            typename QMap<int,E*>::iterator it = allEntity.find(id);
            if (it != allEntity.end()) {
                QMetaObject::invokeMethod(e, [e]{ e->deleteLater(); }, Qt::QueuedConnection);
                return SingleAccessWPtr<E>(it.value());
            }
            allEntity.insert(id, e);
        }
        return SingleAccessWPtr<E>(e);
    }

    // --- Swap entity from RAM into SQLite (and delete RAM copy) ---
    bool SwapOut(int id) {
        E* e = nullptr;
        QSharedPointer<QReadWriteLock> elock;

        {
            QWriteLocker mapW(&lock_);
            typename QMap<int,E*>::iterator it = allEntity.find(id);
            if (it == allEntity.end())
                return false;

            e     = it.value();
            elock = e->Lock();

            allEntity.erase(it);          // block new guards
            swappingOut.insert(id);       // announce in-flight
        } // release map lock

        // Wait out active users; then serialize to DB
        QWriteLocker entityW(elock.data());
        const QByteArray raw = e->Serialize();
        (void)dbUpsert(id, raw);          // best effort; handle failure per your policy

        // Delete in object's thread
        QMetaObject::invokeMethod(e, [e]{ e->deleteLater(); }, Qt::QueuedConnection);

        // Flip state, wake waiters
        {
            QWriteLocker mapW(&lock_);
            swappingOut.remove(id);
        }
        {
            QMutexLocker g(&inflightMx);
            inflightCv.wakeAll();
        }
        return true;
    }

    // Swap a single id from SQLite into RAM if present in DB.
    // Returns true if the entity is now in RAM (either already was, or loaded).
    bool SwapIn(int id,
                QThread* targetThread = QThread::currentThread(),
                QObject* parentForEntity = nullptr)
    {
        // Already in RAM?
        {
            QReadLocker mapR(&lock_);
            typename QMap<int,E*>::const_iterator it = allEntity.find(id);
            if (it != allEntity.end()) return true;
        }

        // If it's currently being swapped out, wait and re-check RAM
        waitWhileSwapping(id);
        {
            QReadLocker mapR(&lock_);
            typename QMap<int,E*>::const_iterator it = allEntity.find(id);
            if (it != allEntity.end()) return true;
        }

        // Load from DB; if not found, nothing to do
        QByteArray raw;
        if (!dbLoad(id, raw)) return false;

        // Materialize and insert
        std::unique_ptr<E> fresh(new E(parentForEntity));
        if (fresh->thread() != targetThread)
            fresh->moveToThread(targetThread);

        fresh->Deserialize(raw);
        E* e = fresh.release();

        {
            QWriteLocker mapW(&lock_);
            typename QMap<int,E*>::iterator it = allEntity.find(id);
            if (it != allEntity.end()) {
                // Another thread inserted while we were loading
                QMetaObject::invokeMethod(e, [e]{ e->deleteLater(); }, Qt::QueuedConnection);
                return true;
            }
            allEntity.insert(id, e);
        }
        return true;
    }

    // Bulk prefetch: attempts to swap in many ids.
    // Returns the number of ids that ended up resident in RAM after the call.
    int SwapInMany(const QVector<int>& ids,
                   QThread* targetThread = QThread::currentThread(),
                   QObject* parentForEntity = nullptr)
    {
        int brought = 0;

        // Fast filter: skip ids already resident
        QVector<int> toLoad; toLoad.reserve(ids.size());
        {
            QReadLocker mapR(&lock_);
            for (int id : ids) {
                if (!allEntity.contains(id)) toLoad.push_back(id);
            }
        }
        if (toLoad.isEmpty()) return 0;

        // Respect in-flight swaps: wait for any id currently swapping out
        for (int id : toLoad) waitWhileSwapping(id);

        // Re-check after waits (another thread may have loaded some)
        QVector<int> stillToLoad; stillToLoad.reserve(toLoad.size());
        {
            QReadLocker mapR(&lock_);
            for (int id : toLoad) {
                if (!allEntity.contains(id)) stillToLoad.push_back(id);
            }
        }
        if (stillToLoad.isEmpty()) return 0;

        // OPTIONAL: wrap the loads in a read-only transaction to improve locality.
        // (safe to skip; SQLite will auto-handle reads fine)
        {
            QSqlDatabase db = dbForThread();
            QSqlQuery qBegin(db); qBegin.exec(QStringLiteral("BEGIN;"));

            for (int id : stillToLoad) {
                QByteArray raw;
                if (!dbLoad(id, raw)) continue; // not in DB; skip

                std::unique_ptr<E> fresh(new E(parentForEntity));
                if (fresh->thread() != targetThread)
                    fresh->moveToThread(targetThread);
                fresh->Deserialize(raw);
                E* e = fresh.release();

                bool inserted = false;
                {
                    QWriteLocker mapW(&lock_);
                    if (!allEntity.contains(id)) {
                        allEntity.insert(id, e);
                        inserted = true;
                    }
                }
                if (inserted) {
                    ++brought;
                } else {
                    // Lost the race; discard duplicate safely in its thread
                    QMetaObject::invokeMethod(e, [e]{ e->deleteLater(); }, Qt::QueuedConnection);
                }
            }

            QSqlQuery qEnd(db); qEnd.exec(QStringLiteral("COMMIT;"));
        }

        return brought;
    }

    // --- Remove from RAM and SQLite ---
    bool Remove(int id) {
        // If in RAM: erase and delete
        {
            E* e = nullptr;
            QSharedPointer<QReadWriteLock> elock;
            {
                QWriteLocker mapW(&lock_);
                typename QMap<int,E*>::iterator it = allEntity.find(id);
                if (it != allEntity.end()) {
                    e     = it.value();
                    elock = e->Lock();
                    allEntity.erase(it);
                }
            }
            if (e) {
                QWriteLocker entityW(elock.data());
                QMetaObject::invokeMethod(e, [e]{ e->deleteLater(); }, Qt::QueuedConnection);
                dbDelete(id); // purge from DB too
                return true;
            }
        }

        // If mid-swap: wait, then continue
        waitWhileSwapping(id);

        // Not in RAM: delete DB row if present
        return dbDelete(id);
    }

    // --- Clear RAM + purge SQLite table (no wait for deletes to complete) ---
    void Clear() {
        // Snapshot RAM
        QList< QPair<E*, QSharedPointer<QReadWriteLock> > > snapshot;
        {
            QWriteLocker mapW(&lock_);
            for (typename QMap<int,E*>::iterator it = allEntity.begin(); it != allEntity.end(); ++it)
                snapshot.append(qMakePair(it.value(), it.value()->Lock()));
            allEntity.clear();
        }
        // Delete RAM entities safely
        for (int i = 0; i < snapshot.size(); ++i) {
            E* e = snapshot[i].first;
            QSharedPointer<QReadWriteLock> elock = snapshot[i].second;
            QWriteLocker entityW(elock.data());
            QMetaObject::invokeMethod(e, [e]{ e->deleteLater(); }, Qt::QueuedConnection);
        }

        // Wait out in-flight swaps then purge DB table
        for (;;) {
            bool any;
            {
                QReadLocker r(&lock_);
                any = !swappingOut.isEmpty();
            }
            if (!any) break;
            QMutexLocker g(&inflightMx);
            inflightCv.wait(&inflightMx, 10);
        }
        dbDeleteAll();
    }

    // --- Clear and wait until all QObject destructions complete, then purge DB ---
    void ClearAndWait() {
        // Snapshot RAM
        QList< QPair<E*, QSharedPointer<QReadWriteLock> > > snapshot;
        {
            QWriteLocker mapW(&lock_);
            for (typename QMap<int,E*>::iterator it = allEntity.begin(); it != allEntity.end(); ++it)
                snapshot.append(qMakePair(it.value(), it.value()->Lock()));
            allEntity.clear();
        }

        // Wait for actual destruction
        QAtomicInt remaining(snapshot.size());
        QEventLoop loop;
        for (int i = 0; i < snapshot.size(); ++i) {
            E* e = snapshot[i].first;
            QSharedPointer<QReadWriteLock> elock = snapshot[i].second;

            QObject::connect(e, &QObject::destroyed, &loop,
                [&remaining,&loop](QObject*) {
                    if (remaining.fetchAndAddAcquire(-1) - 1 == 0)
                        loop.quit();
                }, Qt::QueuedConnection);

            QWriteLocker entityW(elock.data());
            QMetaObject::invokeMethod(e, [e]{ e->deleteLater(); }, Qt::QueuedConnection);
        }
        if (remaining.loadAcquire() > 0)
            loop.exec();

        // Wait out in-flight swaps, then purge DB table
        for (;;) {
            bool any;
            {
                QReadLocker r(&lock_);
                any = !swappingOut.isEmpty();
            }
            if (!any) break;
            QMutexLocker g(&inflightMx);
            inflightCv.wait(&inflightMx, 10);
        }
        dbDeleteAll();
    }
};

//--------------------------------------------------------------------------------
#endif // SINGLEACCESS_H
