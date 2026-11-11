/* main.cpp -- BGAPI Monitor.
 *
 * Optional command line (also handy for scripted bench checks):
 *   --ncp COM7            NCP port
 *   --host COM20          proxy mode: this tool's end of the virtual COM pair
 *   --baud 115200
 *   --connect             connect at start-up
 *   --send "HDR|PAYLOAD"  send after connecting (repeatable), e.g. "20 00 01 00|"
 *   --log file.txt        save the log when the application quits (.csv for CSV)
 *   --screenshot f.png    grab the window just before quitting
 *   --quit-after MS       quit automatically (for unattended runs)
 */
#include <QApplication>
#include <QCommandLineParser>
#include <QPixmap>
#include <QTimer>

#include "mainwindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("BGAPI Monitor");
    QApplication::setApplicationVersion("1.0");

    QCommandLineParser p;
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption ncp("ncp", "NCP COM port.", "port");
    QCommandLineOption host("host", "Proxy port (this tool's end of the virtual pair).", "port");
    QCommandLineOption baud("baud", "Baud rate.", "rate", "115200");
    QCommandLineOption conn("connect", "Connect at start-up.");
    QCommandLineOption send("send", "Frame to send after connecting: \"HEADER|PAYLOAD\" hex.", "frame");
    QCommandLineOption log("log", "Save the log to this file on exit.", "file");
    QCommandLineOption shot("screenshot", "Save a screenshot of the window before quitting.", "file");
    QCommandLineOption quit("quit-after", "Quit after this many milliseconds.", "ms");
    p.addOptions({ncp, host, baud, conn, send, log, shot, quit});
    p.process(app);

    MainWindow w;
    w.setPorts(p.value(ncp), p.value(host), p.value(baud).toInt());
    w.show();

    if (p.isSet(conn)) {
        QTimer::singleShot(100, &w, [&w, &p, send] {
            if (!w.connectPorts()) return;
            int delay = 300;
            for (const QString &f : p.values(send)) {
                const QString hdr = f.section('|', 0, 0), pl = f.section('|', 1);
                QTimer::singleShot(delay, &w, [&w, hdr, pl] { w.sendHex(hdr, pl); });
                delay += 200;
            }
        });
    }
    if (p.isSet(quit)) {
        const int ms = p.value(quit).toInt();
        QTimer::singleShot(ms, &w, [&w, &p, log, shot] {
            if (p.isSet(shot)) w.grab().save(p.value(shot));
            if (p.isSet(log)) w.saveLog(p.value(log));
            QApplication::quit();
        });
    } else if (p.isSet(log)) {
        QObject::connect(&app, &QApplication::aboutToQuit, &w, [&w, &p, log] { w.saveLog(p.value(log)); });
    }
    return app.exec();
}
