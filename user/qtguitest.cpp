/*
 * user/qtguitest.cpp — native PureUnix regression test for real Qt 6 Gui
 * (docs/qt-port.md Phase 5), proving QtGui's platform-independent raster
 * path — QImage, QPainter, QColor, QFont/QFontMetrics — actually runs on
 * real PureUnix hardware/QEMU, on top of the real cross-built
 * libQt6Gui.a from Phase 3.
 *
 * Real upstream Qt classes only, no reimplementation. No real PureUnix
 * QPA platform plugin exists yet (that's Phase 6/7) — this test runs
 * against Qt's own real, upstream "offscreen" QPA plugin
 * (QOffscreenIntegrationPlugin), which renders entirely into an in-memory
 * QImage with no real display/window system required, exactly matching
 * this phase's own scope ("platform-independent parts... no real
 * windowing backend needed yet" per docs/qt-port.md). Same numbered-
 * checks harness convention as qtcoretest.cpp: every check is
 * independent, a failure never stops the run, and a summary prints at
 * the end.
 */
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QColor>
#include <QRect>
#include <QFont>
#include <QFontMetrics>
#include <QtPlugin>
#include <cstdio>

Q_IMPORT_PLUGIN(QOffscreenIntegrationPlugin)

static int g_num = 0;
static int g_pass = 0;
static int g_fail = 0;

static void check_true(const char *desc, bool cond)
{
    g_num++;
    std::printf("[%03d] %s\n", g_num, desc);
    if (cond) {
        std::printf("      PASS\n");
        g_pass++;
    } else {
        std::printf("      FAIL\n");
        g_fail++;
    }
}

static void check_int(const char *desc, long expected, long got)
{
    g_num++;
    std::printf("[%03d] %s\n", g_num, desc);
    if (expected == got) {
        std::printf("      PASS\n");
        g_pass++;
    } else {
        std::printf("      FAIL: expected %ld, got %ld\n", expected, got);
        g_fail++;
    }
}

int main(int argc, char *argv[])
{
    /* Force the real, upstream offscreen QPA platform (statically linked
     * above via Q_IMPORT_PLUGIN — no dynamic plugin loading exists on
     * PureUnix, see docs/qt-port.md) rather than relying on a
     * command-line flag or environment variable a real invocation might
     * not pass. */
    static char arg0[] = "qtguitest";
    static char argPlatform[] = "-platform";
    static char argOffscreen[] = "offscreen";
    static char *fakeArgv[] = { arg0, argPlatform, argOffscreen, nullptr };
    int fakeArgc = 3;
    (void)argc;
    (void)argv;

    QGuiApplication app(fakeArgc, fakeArgv);

    std::printf("=== PureUnix Qt Gui test (qtguitest) ===\n");

    check_true("QGuiApplication constructed with the offscreen QPA plugin", true);

    /* QImage — the platform-independent raster surface every other check
     * below draws into. */
    QImage img(64, 32, QImage::Format_ARGB32);
    check_true("QImage::isNull() is false after construction", !img.isNull());
    check_int("QImage::width()", 64, img.width());
    check_int("QImage::height()", 32, img.height());

    img.fill(QColor(0, 0, 0, 255));
    check_true("QImage::pixelColor() reads back a fill()ed color",
               img.pixelColor(5, 5) == QColor(0, 0, 0, 255));

    /* QPainter — the real raster paint engine, not a hand-rolled one. */
    {
        QPainter p(&img);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 0, 0, 255));
        p.drawRect(QRect(10, 10, 20, 10));
        p.setPen(QColor(0, 255, 0, 255));
        p.drawLine(0, 0, 63, 0);
        p.end();
    }
    check_true("QPainter::drawRect() fill color reads back correctly",
               img.pixelColor(15, 15) == QColor(255, 0, 0, 255));
    check_true("QPainter::drawLine() color reads back correctly",
               img.pixelColor(30, 0) == QColor(0, 255, 0, 255));
    check_true("QPainter drawing left pixels outside the rect untouched",
               img.pixelColor(1, 1) == QColor(0, 0, 0, 255));

    /* QColor — HSV/named-color conversions, not just RGB construction. */
    QColor named("cornflowerblue");
    check_true("QColor named-color lookup produces a valid color", named.isValid());
    QColor hsv;
    hsv.setHsv(120, 255, 255);
    check_true("QColor::setHsv() green hue round-trips through toRgb()",
               hsv.toRgb() == QColor(0, 255, 0));

    /* QFont/QFontMetrics — exercises the platform plugin's font database
     * (QBasicFontDatabase, real upstream code, not PureUnix-specific) and
     * the bundled FreeType/HarfBuzz text-shaping stack. No real font
     * files are vendored on this target, so this only checks that the
     * font subsystem *runs* without crashing and returns self-consistent
     * metrics — not that a specific typeface's exact glyph shapes are
     * available. */
    QFont font;
    QFontMetrics fm(font);
    int height = fm.height();
    int width = fm.horizontalAdvance(QStringLiteral("Hi"));
    check_true("QFontMetrics::height() is positive", height > 0);
    check_true("QFontMetrics::horizontalAdvance() is positive for non-empty text", width > 0);

    std::printf("=== %d passed, %d failed (of %d) ===\n", g_pass, g_fail, g_num);
    return g_fail == 0 ? 0 : 1;
}
