/*
 * user/qtcoretest.cpp — native PureUnix regression test for real Qt 6 Core
 * (docs/qt-port.md Phase 4), proving Qt Core actually runs — not just
 * links — on real PureUnix hardware/QEMU, on top of the real cross-built
 * libQt6Core.a from Phase 3.
 *
 * Real upstream Qt classes only, no reimplementation: QString,
 * QByteArray, QList, QObject with a genuine signal/slot connection,
 * QCoreApplication, QTimer, QFile, and a real event-loop execution via
 * QCoreApplication::exec(). Same numbered-checks harness convention as
 * user/cxxtest.c/libctest.c: every check is independent, a failure never
 * stops the run, and a summary prints at the end — except the signal/slot
 * and QTimer checks, which only resolve *inside* the running event loop
 * (that's the whole point of exercising exec()), so they're recorded
 * during callbacks and only summarized once exec() returns.
 */
#include <QCoreApplication>
#include <QObject>
#include <QTimer>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QFile>
#include <QTextStream>
#include <QElapsedTimer>
#include <cstdio>

static int g_num = 0;
static int g_pass = 0;
static int g_fail = 0;

static void t_begin(const char *desc)
{
    g_num++;
    std::printf("[%03d] %s\n", g_num, desc);
}

static void check_true(const char *desc, bool cond)
{
    t_begin(desc);
    if (cond) {
        std::printf("      PASS\n");
        g_pass++;
    } else {
        std::printf("      FAIL\n");
        g_fail++;
    }
}

static void check_str(const char *desc, const QString &expected, const QString &got)
{
    t_begin(desc);
    if (expected == got) {
        std::printf("      PASS\n");
        g_pass++;
    } else {
        std::printf("      FAIL: expected \"%s\", got \"%s\"\n",
                     expected.toUtf8().constData(), got.toUtf8().constData());
        g_fail++;
    }
}

static void check_int(const char *desc, long expected, long got)
{
    t_begin(desc);
    if (expected == got) {
        std::printf("      PASS\n");
        g_pass++;
    } else {
        std::printf("      FAIL: expected %ld, got %ld\n", expected, got);
        g_fail++;
    }
}

/* Real QObject subclass with a real signal and a real slot, connected via
 * QObject::connect() — proves Qt's moc-generated meta-object machinery
 * (signals/slots, not a hand-rolled callback list) works end to end on
 * this target. */
class Counter : public QObject
{
    Q_OBJECT
public:
    int value = 0;

public slots:
    void increment()
    {
        value++;
        emit valueChanged(value);
    }

signals:
    void valueChanged(int newValue);
};

class Receiver : public QObject
{
    Q_OBJECT
public:
    int lastSeen = -1;
    int callCount = 0;

public slots:
    void onValueChanged(int newValue)
    {
        lastSeen = newValue;
        callCount++;
    }
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    std::printf("=== PureUnix Qt Core test (qtcoretest) ===\n");

    /* QString */
    {
        QString s = QStringLiteral("Hello, ");
        s += QStringLiteral("PureUnix");
        check_str("QString concatenation", QStringLiteral("Hello, PureUnix"), s);
        check_int("QString::size()", 15, s.size());
        check_str("QString::toUpper()", QStringLiteral("HELLO, PUREUNIX"), s.toUpper());
        check_str("QString::arg()", QStringLiteral("value = 42"), QStringLiteral("value = %1").arg(42));
    }

    /* QByteArray */
    {
        QByteArray ba = "PureUnix";
        check_int("QByteArray::size()", 8, ba.size());
        check_true("QByteArray::contains()", ba.contains("Unix"));
        QByteArray b64 = ba.toBase64();
        check_true("QByteArray::toBase64() round-trips", QByteArray::fromBase64(b64) == ba);
    }

    /* QList */
    {
        QList<int> list;
        for (int i = 0; i < 10; i++) {
            list.append(i * i);
        }
        check_int("QList::size() after 10 append", 10, list.size());
        check_int("QList element access", 49, list.at(7));
        list.removeAt(2);
        check_int("QList::removeAt() shrinks size", 9, list.size());
        check_int("QList::removeAt() shifts later elements", 9, list.at(2));
    }

    /* QFile — real file I/O through the real VFS (write then read back) */
    {
        const QString path = QStringLiteral("/tmp/qtcoretest.txt");
        {
            QFile f(path);
            bool opened = f.open(QIODevice::WriteOnly | QIODevice::Text);
            check_true("QFile::open() for write succeeds", opened);
            if (opened) {
                QTextStream out(&f);
                out << "PureUnix Qt Core test\n";
            }
        }
        {
            QFile f(path);
            bool opened = f.open(QIODevice::ReadOnly | QIODevice::Text);
            check_true("QFile::open() for read succeeds", opened);
            if (opened) {
                QTextStream in(&f);
                QString line = in.readLine();
                check_str("QFile round-trip content", QStringLiteral("PureUnix Qt Core test"), line);
            }
        }
        QFile::remove(path);
    }

    /* QElapsedTimer — proves the real CLOCK_MONOTONIC path (docs/qt-port.md
     * Phase 3 item 11) is wired up and returns sane elapsed values. */
    {
        QElapsedTimer timer;
        timer.start();
        check_true("QElapsedTimer::isValid() after start()", timer.isValid());
        qint64 elapsed = timer.elapsed();
        check_true("QElapsedTimer::elapsed() is non-negative", elapsed >= 0);
    }

    /* Real signal/slot connection through moc-generated meta-object code */
    Counter counter;
    Receiver receiver;
    QObject::connect(&counter, &Counter::valueChanged, &receiver, &Receiver::onValueChanged);
    counter.increment();
    counter.increment();
    check_int("signal/slot: receiver saw the right final value", 2, receiver.lastSeen);
    check_int("signal/slot: receiver was called the right number of times", 2, receiver.callCount);

    /* Real event-loop execution: QTimer::singleShot schedules work that
     * can only run once QCoreApplication::exec() actually pumps the event
     * loop - proves the event loop (backed by the real poll()-with-timeout
     * fix in user/newlib_syscalls.c, see docs/qt-port.md section 4) works,
     * not just that Qt Core links. Three staggered timers plus a final
     * quit() prove ordering and repeated dispatch, not just a single
     * callback firing by luck. */
    static QList<int> firedOrder;
    QTimer::singleShot(10, &app, [&]() {
        firedOrder.append(1);
        counter.increment();
    });
    QTimer::singleShot(30, &app, [&]() {
        firedOrder.append(2);
    });
    QTimer::singleShot(60, &app, [&]() {
        firedOrder.append(3);
        std::printf("[%03d] event loop: three staggered QTimers fired in order\n", ++g_num);
        if (firedOrder.size() == 3 && firedOrder.at(0) == 1 && firedOrder.at(1) == 2 && firedOrder.at(2) == 3) {
            std::printf("      PASS\n");
            g_pass++;
        } else {
            std::printf("      FAIL\n");
            g_fail++;
        }

        t_begin("signal/slot: increment() during event loop reached the receiver");
        if (receiver.callCount == 3 && receiver.lastSeen == 3) {
            std::printf("      PASS\n");
            g_pass++;
        } else {
            std::printf("      FAIL: callCount=%d lastSeen=%d\n", receiver.callCount, receiver.lastSeen);
            g_fail++;
        }

        std::printf("=== %d passed, %d failed (of %d) ===\n", g_pass, g_fail, g_num);
        QCoreApplication::quit();
    });

    return app.exec();
}

#include "qtcoretest.moc"
