/*
 * user/qtwidgetstest.cpp — Phase 5 of docs/qt-port.md's Phase 6 plan: a
 * real, unmodified Qt Widgets application (QMainWindow + a basic layout
 * of QLabel/QPushButton/QLineEdit/QTextEdit) running against the real
 * "pureunix" QPA plugin (user/qpa_pureunix/), the same plugin
 * user/qtwindowtest.cpp already verified end-to-end inside a real `pude`
 * window. No PureUnix-specific code in this file at all -- every line
 * here is exactly what this same app would look like on any other Qt
 * platform.
 */
#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QtPlugin>
#include <cstdio>

Q_IMPORT_PLUGIN(QPureUnixIntegrationPlugin)

int main(int argc, char *argv[])
{
    static char arg0[] = "qtwidgetstest";
    static char argPlatform[] = "-platform";
    static char argPureUnix[] = "pureunix";
    static char *fakeArgv[] = { arg0, argPlatform, argPureUnix, nullptr };
    int fakeArgc = 3;
    (void)argc;
    (void)argv;

    QApplication app(fakeArgc, fakeArgv);
    std::printf("[001] QApplication constructed with the pureunix QPA plugin\n      PASS\n");

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("PureUnix Qt Widgets Test"));

    auto *central = new QWidget(&window);
    auto *outer = new QVBoxLayout(central);

    auto *label = new QLabel(QStringLiteral("Hello from Qt Widgets on PureUnix"), central);
    outer->addWidget(label);

    auto *lineEdit = new QLineEdit(central);
    lineEdit->setPlaceholderText(QStringLiteral("Type here"));
    outer->addWidget(lineEdit);

    auto *textEdit = new QTextEdit(central);
    textEdit->setPlainText(QStringLiteral("A real QTextEdit.\nMultiple lines work too."));
    outer->addWidget(textEdit);

    auto *buttonRow = new QHBoxLayout();
    auto *button = new QPushButton(QStringLiteral("Click me"), central);
    auto *clickCountLabel = new QLabel(QStringLiteral("Clicks: 0"), central);
    buttonRow->addWidget(button);
    buttonRow->addWidget(clickCountLabel);
    outer->addLayout(buttonRow);

    static int clickCount = 0;
    QObject::connect(button, &QPushButton::clicked, [clickCountLabel]() {
        ++clickCount;
        clickCountLabel->setText(QStringLiteral("Clicks: %1").arg(clickCount));
        std::printf("[003] QPushButton::clicked signal fired (count=%d)\n      PASS\n", clickCount);
    });

    window.setCentralWidget(central);
    window.resize(420, 320);
    window.show();
    std::printf("[002] QMainWindow with QLabel/QLineEdit/QTextEdit/QPushButton shown\n      PASS\n");

    return app.exec();
}
