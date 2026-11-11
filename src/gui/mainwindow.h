/* mainwindow.h -- BGAPI Monitor main window (layout follows the mockup). */
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QElapsedTimer>
#include <QMainWindow>
#include <QSerialPort>
#include <memory>

#include "bgapi_core.h"

class QAbstractItemModel;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
class QTimer;
class FrameModel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /* used by command-line options */
    void setPorts(const QString &ncp, const QString &host, int baud);
    bool connectPorts();
    void sendHex(const QString &header, const QString &payload);
    bool saveLog(const QString &path);

private slots:
    void refreshPorts();
    void onConnect();
    void onDisconnect();
    void onProxyToggled(bool on);
    void onNcpReadyRead();
    void onHostReadyRead();
    void onPortError(QSerialPort::SerialPortError e);
    void onTick();
    void onCommandPicked(const QString &name);
    void updateSendPreview();
    void onSend();
    void onSelectionChanged(QTableView *view);
    void onClear();
    void onSave();

private:
    QWidget *buildTopBar();
    QWidget *buildSendPanel();
    QTableView *buildTable(QAbstractItemModel *model);
    bool openPort(QSerialPort &port, const QString &name, QString &err);
    int64_t nowUs() const;
    void showDecode(const bgmon::Record *r);
    void setConnectedUi(bool on);
    void updateStatus();
    QByteArray parseHex(const QString &text, bool *ok) const;
    void closeAll(const QString &why);

    /* top bar */
    QComboBox *ncpCombo_ = nullptr;
    QComboBox *hostCombo_ = nullptr;
    QLabel *hostLabel_ = nullptr;
    QComboBox *baudCombo_ = nullptr;
    QCheckBox *rtsCts_ = nullptr;
    QPushButton *refreshBtn_ = nullptr, *connectBtn_ = nullptr, *disconnectBtn_ = nullptr;
    QCheckBox *proxyToggle_ = nullptr;

    /* send panel */
    QComboBox *cmdCombo_ = nullptr;
    QLineEdit *headerEdit_ = nullptr, *headerSize_ = nullptr;
    QLineEdit *payloadEdit_ = nullptr, *payloadSize_ = nullptr;
    QCheckBox *autoLen_ = nullptr;
    QLabel *sendCheck_ = nullptr;
    QPushButton *sendBtn_ = nullptr;

    /* panes */
    QPlainTextEdit *decode_ = nullptr;
    QCheckBox *followRx_ = nullptr;
    QTableView *txView_ = nullptr, *rxView_ = nullptr;
    FrameModel *txModel_ = nullptr, *rxModel_ = nullptr;
    QSortFilterProxyModel *rxFilter_ = nullptr;
    QCheckBox *autoScroll_ = nullptr, *showEvents_ = nullptr;
    QLabel *status_ = nullptr;

    QSerialPort ncpPort_, hostPort_;
    std::unique_ptr<bgmon::Session> session_;
    QElapsedTimer clock_;
    QTimer *tick_ = nullptr;
    QString linkText_;
    bool updatingHeader_ = false;
    bool settingLines_ = false;
};

#endif
