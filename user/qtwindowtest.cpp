/*
 * user/qtwindowtest.cpp — native PureUnix regression test for the real
 * "pureunix" QPA platform plugin (docs/qt-port.md, Phase 6). Grows in
 * place across that phase's own sub-phases (1: QGuiApplication init only;
 * 2: a QWindow inside `pude`; 3: real QPainter output via a raster
 * window; 4: keyboard/mouse input) rather than one throwaway test per
 * sub-phase, mirroring how qtcoretest.cpp/qtguitest.cpp are each a single
 * growing regression file, not one file per Qt module milestone.
 */
#include <QGuiApplication>
#include <QRasterWindow>
#include <QPainter>
#include <QtPlugin>
#include <cstdio>

Q_IMPORT_PLUGIN(QPureUnixIntegrationPlugin)

class TestWindow : public QRasterWindow
{
public:
    TestWindow()
    {
        setTitle(QStringLiteral("PureUnix Qt Window Test"));
        resize(320, 240);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QRect r(QPoint(0, 0), size());
        QPainter p(this);
        p.fillRect(r, QColor(40, 40, 120));
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, QStringLiteral("Hello from Qt on PureUnix"));
    }
};

int main(int argc, char *argv[])
{
    static char arg0[] = "qtwindowtest";
    static char argPlatform[] = "-platform";
    static char argPureUnix[] = "pureunix";
    static char *fakeArgv[] = { arg0, argPlatform, argPureUnix, nullptr };
    int fakeArgc = 3;
    (void)argc;
    (void)argv;

    QGuiApplication app(fakeArgc, fakeArgv);
    std::printf("[001] QGuiApplication constructed with the pureunix QPA plugin\n      PASS\n");

    TestWindow win;
    win.show();
    std::printf("[002] TestWindow (QRasterWindow) shown\n      PASS\n");

    return app.exec();
}
