#include "mainwindow.h"
#include "framemodel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#ifdef BGMON_HAVE_TMS_SCAN
#include "serial_transport.h"   /* TMS Configurator's registry scan (HARDWARE\DEVICEMAP\SERIALCOMM) */
#endif

using namespace bgmon;

namespace {

class EventFilterModel : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    bool showEvents = true;
    void setShowEvents(bool on) {
        showEvents = on;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        invalidateRowsFilter();
#else
        invalidateFilter();
#endif
    }
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override {
        if (showEvents) return true;
        return !sourceModel()->index(row, 0, parent).data(FrameModel::IsEventRole).toBool();
    }
};

const char *kPaneStyle =
    "QTableView, QPlainTextEdit { background: #101214; color: #e1e1e1; gridline-color: #2a2d31;"
    " selection-background-color: #3d6fb4; selection-color: white; }"
    "QHeaderView::section { background: #2a2d31; color: #cfcfcf; border: 0; padding: 3px; }";

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("BGAPI Monitor - TMS NCP");
    clock_.start();
    const QDateTime t0 = QDateTime::currentDateTime();
    txModel_ = new FrameModel(t0, this);
    rxModel_ = new FrameModel(t0, this);
    auto *filter = new EventFilterModel(this);
    filter->setSourceModel(rxModel_);
    rxFilter_ = filter;

    auto *central = new QWidget(this);
    auto *v = new QVBoxLayout(central);
    v->setContentsMargins(6, 6, 6, 6);
    v->addWidget(buildTopBar());

    /* upper area: send panel (left) | RX decoding (right) */
    auto *upper = new QWidget;
    auto *uh = new QHBoxLayout(upper);
    uh->setContentsMargins(0, 0, 0, 0);
    uh->addWidget(buildSendPanel(), 1);
    auto *sep = new QFrame;
    sep->setFrameShape(QFrame::VLine);
    uh->addWidget(sep);
    auto *decW = new QWidget;
    auto *dv = new QVBoxLayout(decW);
    dv->setContentsMargins(0, 0, 0, 0);
    auto *dh = new QHBoxLayout;
    dh->addWidget(new QLabel("<b>RX decoding</b>"));
    dh->addStretch();
    followRx_ = new QCheckBox("follow latest RX");
    followRx_->setChecked(true);
    followRx_->setToolTip("Unticked: shows the frame you click in the TX or RX table");
    dh->addWidget(followRx_);
    dv->addLayout(dh);
    decode_ = new QPlainTextEdit;
    decode_->setReadOnly(true);
    decode_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    decode_->setFont(mono);
    decode_->setStyleSheet(kPaneStyle);
    dv->addWidget(decode_);
    uh->addWidget(decW, 1);

    /* lower area: TX | RX */
    auto *lower = new QSplitter(Qt::Horizontal);
    auto mkPane = [&](const QString &title, QTableView *view) {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        auto *lab = new QLabel("<b>" + title + "</b>");
        lab->setAlignment(Qt::AlignCenter);
        l->addWidget(lab);
        l->addWidget(view);
        return w;
    };
    txView_ = buildTable(txModel_);
    rxView_ = buildTable(rxFilter_);
    lower->addWidget(mkPane("TX  (to NCP)", txView_));
    lower->addWidget(mkPane("RX  (from NCP)", rxView_));

    auto *vs = new QSplitter(Qt::Vertical);
    vs->addWidget(upper);
    vs->addWidget(lower);
    vs->setStretchFactor(0, 0);
    vs->setStretchFactor(1, 1);
    v->addWidget(vs, 1);

    auto *bottom = new QHBoxLayout;
    auto *clearBtn = new QPushButton("Clear");
    auto *saveBtn = new QPushButton("Save log...");
    autoScroll_ = new QCheckBox("Auto-scroll");
    autoScroll_->setChecked(true);
    showEvents_ = new QCheckBox("Show events in RX");
    showEvents_->setChecked(true);
    bottom->addWidget(clearBtn);
    bottom->addWidget(saveBtn);
    bottom->addSpacing(12);
    bottom->addWidget(autoScroll_);
    bottom->addWidget(showEvents_);
    bottom->addStretch();
    v->addLayout(bottom);
    setCentralWidget(central);

    status_ = new QLabel;
    statusBar()->addWidget(status_, 1);

    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onClear);
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSave);
    connect(showEvents_, &QCheckBox::toggled, this, [filter](bool on) { filter->setShowEvents(on); });
    connect(txView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { onSelectionChanged(txView_); });
    connect(rxView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { onSelectionChanged(rxView_); });
    connect(&ncpPort_, &QSerialPort::readyRead, this, &MainWindow::onNcpReadyRead);
    connect(&hostPort_, &QSerialPort::readyRead, this, &MainWindow::onHostReadyRead);
    connect(&ncpPort_, &QSerialPort::errorOccurred, this, &MainWindow::onPortError);
    connect(&hostPort_, &QSerialPort::errorOccurred, this, &MainWindow::onPortError);

    tick_ = new QTimer(this);
    tick_->setInterval(20);
    connect(tick_, &QTimer::timeout, this, &MainWindow::onTick);
    tick_->start();

    refreshPorts();
    onProxyToggled(false);
    setConnectedUi(false);
    updateSendPreview();
    showDecode(nullptr);
    resize(1280, 820);
}

MainWindow::~MainWindow() { closeAll(QString()); }

/* ------------------------------------------------------------------ layout */

QWidget *MainWindow::buildTopBar() {
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(new QLabel("<b>COM</b>"));
    ncpCombo_ = new QComboBox;
    ncpCombo_->setEditable(true);
    ncpCombo_->setMinimumWidth(110);
    ncpCombo_->setToolTip("NCP port (the J-Link VCOM of the WSTK)");
    h->addWidget(ncpCombo_);
    hostLabel_ = new QLabel("<b>Host COM</b>");
    hostCombo_ = new QComboBox;
    hostCombo_->setEditable(true);
    hostCombo_->setMinimumWidth(110);
    hostCombo_->setToolTip("Proxy port: this tool's end of the virtual COM pair.\n"
                           "The TMS Configurator opens the OTHER end of the pair.");
    h->addWidget(hostLabel_);
    h->addWidget(hostCombo_);
    h->addSpacing(8);
    h->addWidget(new QLabel("Baud"));
    baudCombo_ = new QComboBox;
    baudCombo_->setEditable(true);
    for (int b : {115200, 230400, 460800, 921600, 1000000, 57600, 9600}) baudCombo_->addItem(QString::number(b));
    h->addWidget(baudCombo_);
    rtsCts_ = new QCheckBox("RTS/CTS");
    rtsCts_->setChecked(true);
    rtsCts_->setToolTip("Hardware flow control. On by default, as in serial_transport.c and the stock NCP firmware.");
    h->addWidget(rtsCts_);
    h->addSpacing(8);
    refreshBtn_ = new QPushButton("Refresh");
    connectBtn_ = new QPushButton("Connect");
    disconnectBtn_ = new QPushButton("Disconnect");
    h->addWidget(refreshBtn_);
    h->addWidget(connectBtn_);
    h->addWidget(disconnectBtn_);
    h->addStretch();
    proxyToggle_ = new QCheckBox("proxy COM");
    proxyToggle_->setLayoutDirection(Qt::RightToLeft);
    proxyToggle_->setStyleSheet(
        "QCheckBox { font-weight: bold; spacing: 8px; }"
        "QCheckBox::indicator { width: 36px; height: 16px; border-radius: 8px; border: 1px solid #555; background: #9a9a9a; }"
        "QCheckBox::indicator:checked { background: #2e9d48; }");
    proxyToggle_->setToolTip("On: sit between the TMS Configurator and the NCP, forward all traffic and\n"
                             "show it; commands from this tool are slotted in safely.");
    h->addWidget(proxyToggle_);

    connect(refreshBtn_, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(connectBtn_, &QPushButton::clicked, this, &MainWindow::onConnect);
    connect(disconnectBtn_, &QPushButton::clicked, this, &MainWindow::onDisconnect);
    connect(proxyToggle_, &QCheckBox::toggled, this, &MainWindow::onProxyToggled);
    return w;
}

QWidget *MainWindow::buildSendPanel() {
    auto *w = new QWidget;
    auto *g = new QGridLayout(w);
    g->setContentsMargins(0, 0, 0, 0);

    cmdCombo_ = new QComboBox;
    cmdCombo_->setEditable(true);
    cmdCombo_->setInsertPolicy(QComboBox::NoInsert);
    QStringList names;
    for (unsigned i = 0; i < bgapi_msg_def_count; ++i)
        if (bgapi_msg_defs[i].kind == BGAPI_KIND_CMD) names << QString(bgapi_msg_defs[i].name + 4);
    names.sort();
    cmdCombo_->addItem(QString());
    cmdCombo_->addItems(names);
    cmdCombo_->completer()->setFilterMode(Qt::MatchContains);
    cmdCombo_->completer()->setCompletionMode(QCompleter::PopupCompletion);
    cmdCombo_->setToolTip("Optional: pick a command to fill the header (and a zeroed payload of its API size)");

    headerEdit_ = new QLineEdit;
    headerEdit_->setPlaceholderText("20 00 01 00");
    headerSize_ = new QLineEdit;
    headerSize_->setReadOnly(true);
    headerSize_->setMaximumWidth(70);
    headerSize_->setToolTip("Payload length encoded in the header (11-bit length field)");
    payloadEdit_ = new QLineEdit;
    payloadEdit_->setPlaceholderText("hex bytes, e.g. 01 2C 00");
    payloadSize_ = new QLineEdit;
    payloadSize_->setReadOnly(true);
    payloadSize_->setMaximumWidth(70);
    payloadSize_->setToolTip("Number of payload bytes typed");
    autoLen_ = new QCheckBox("auto length");
    autoLen_->setChecked(true);
    autoLen_->setToolTip("Keep the header's length field equal to the payload size");
    sendCheck_ = new QLabel;
    sendCheck_->setWordWrap(true);
    sendCheck_->setTextFormat(Qt::RichText);
    sendBtn_ = new QPushButton("Send");
    sendBtn_->setMinimumWidth(80);

    g->addWidget(new QLabel("command"), 0, 0);
    g->addWidget(cmdCombo_, 0, 1, 1, 3);
    g->addWidget(new QLabel("header"), 1, 0);
    g->addWidget(headerEdit_, 1, 1);
    g->addWidget(new QLabel("size"), 1, 2);
    g->addWidget(headerSize_, 1, 3);
    g->addWidget(new QLabel("payload"), 2, 0);
    g->addWidget(payloadEdit_, 2, 1);
    g->addWidget(new QLabel("size"), 2, 2);
    g->addWidget(payloadSize_, 2, 3);
    g->addWidget(autoLen_, 3, 1);
    g->addWidget(sendBtn_, 3, 3);
    g->addWidget(sendCheck_, 4, 0, 1, 4);
    g->setRowStretch(5, 1);
    g->setColumnStretch(1, 1);

    connect(cmdCombo_, &QComboBox::textActivated, this, &MainWindow::onCommandPicked);
    connect(headerEdit_, &QLineEdit::textChanged, this, &MainWindow::updateSendPreview);
    connect(payloadEdit_, &QLineEdit::textChanged, this, &MainWindow::updateSendPreview);
    connect(autoLen_, &QCheckBox::toggled, this, &MainWindow::updateSendPreview);
    connect(sendBtn_, &QPushButton::clicked, this, &MainWindow::onSend);
    connect(payloadEdit_, &QLineEdit::returnPressed, this, &MainWindow::onSend);
    connect(headerEdit_, &QLineEdit::returnPressed, this, &MainWindow::onSend);
    return w;
}

QTableView *MainWindow::buildTable(QAbstractItemModel *model) {
    auto *t = new QTableView;
    t->setModel(model);
    t->setStyleSheet(kPaneStyle);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setWordWrap(false);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(20);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setColumnWidth(FrameModel::ColSeq, 44);
    t->setColumnWidth(FrameModel::ColTime, 92);
    t->setColumnWidth(FrameModel::ColSrc, 40);
    t->setColumnWidth(FrameModel::ColType, 42);
    t->setColumnWidth(FrameModel::ColName, 190);
    t->setColumnWidth(FrameModel::ColLen, 34);
    t->setColumnWidth(FrameModel::ColResult, 54);
    t->setColumnWidth(FrameModel::ColHex, 300);
    t->setColumnWidth(FrameModel::ColAscii, 110);
    return t;
}

/* ------------------------------------------------------------------- ports */

void MainWindow::refreshPorts() {
    QStringList names;
    QHash<QString, QString> desc;
    for (const QSerialPortInfo &i : QSerialPortInfo::availablePorts()) {
        names << i.portName();
        desc[i.portName()] = i.description() + (i.manufacturer().isEmpty() ? "" : " (" + i.manufacturer() + ")");
    }
#ifdef BGMON_HAVE_TMS_SCAN
    {
        char ports[SERIAL_MAX_SCAN_PORTS][SERIAL_PORT_NAME_LEN];
        int count = 0;
        if (serial_transport_scan_ports(ports, SERIAL_MAX_SCAN_PORTS, &count) == 0)
            for (int i = 0; i < count; ++i) {
                QString n = QString::fromLatin1(ports[i]);
                if (!names.contains(n)) { names << n; desc[n] = "listed in HARDWARE\\DEVICEMAP\\SERIALCOMM"; }
            }
    }
#endif
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        static const QRegularExpression re("(\\d+)$");
        auto ma = re.match(a), mb = re.match(b);
        QString pa = a.left(a.size() - ma.capturedLength()), pb = b.left(b.size() - mb.capturedLength());
        if (pa != pb) return pa < pb;
        return ma.captured(1).toInt() < mb.captured(1).toInt();
    });
    for (QComboBox *c : {ncpCombo_, hostCombo_}) {
        const QString cur = c->currentText();
        c->clear();
        for (const QString &n : names) {
            c->addItem(n);
            c->setItemData(c->count() - 1, desc.value(n), Qt::ToolTipRole);
        }
        if (!cur.isEmpty()) c->setCurrentText(cur);
    }
    if (hostCombo_->currentText() == ncpCombo_->currentText() && hostCombo_->count() > 1)
        hostCombo_->setCurrentIndex(ncpCombo_->currentIndex() == 0 ? 1 : 0);
}

void MainWindow::setPorts(const QString &ncp, const QString &host, int baud) {
    if (!ncp.isEmpty()) ncpCombo_->setCurrentText(ncp);
    if (!host.isEmpty()) {
        proxyToggle_->setChecked(true);
        hostCombo_->setCurrentText(host);
    }
    if (baud > 0) baudCombo_->setCurrentText(QString::number(baud));
}

bool MainWindow::openPort(QSerialPort &port, const QString &name, QString &err) {
    bool ok = false;
    const int baud = baudCombo_->currentText().toInt(&ok);
    if (!ok || baud <= 0) { err = "invalid baud rate"; return false; }
    port.setPortName(name);
    port.setBaudRate(baud);
    port.setDataBits(QSerialPort::Data8);
    port.setParity(QSerialPort::NoParity);
    port.setStopBits(QSerialPort::OneStop);
    port.setFlowControl(rtsCts_->isChecked() ? QSerialPort::HardwareControl : QSerialPort::NoFlowControl);
    port.setReadBufferSize(0);
    if (!port.open(QIODevice::ReadWrite)) {
        err = name + ": " + port.errorString();
        return false;
    }
    /* DTR on, as serial_transport.c (DTR_CONTROL_ENABLE). Ports without modem
     * lines (some virtual ports) refuse this; that is harmless, so it is not
     * reported as an error. */
    settingLines_ = true;
    port.setDataTerminalReady(true);
    if (!rtsCts_->isChecked()) port.setRequestToSend(true);
    settingLines_ = false;
    port.clearError();
    port.clear();
    return true;
}

bool MainWindow::connectPorts() {
    if (session_) return true;
    const bool proxy = proxyToggle_->isChecked();
    const QString ncpName = ncpCombo_->currentText().trimmed();
    const QString hostName = hostCombo_->currentText().trimmed();
    QString err;
    if (ncpName.isEmpty()) { QMessageBox::warning(this, "Connect", "Choose the NCP COM port."); return false; }
    if (proxy && (hostName.isEmpty() || hostName == ncpName)) {
        QMessageBox::warning(this, "Connect", "Proxy mode needs a second port: this tool's end of the virtual COM pair.");
        return false;
    }
    if (!openPort(ncpPort_, ncpName, err)) { QMessageBox::warning(this, "Connect", "Cannot open the NCP port:\n" + err); return false; }
    if (proxy && !openPort(hostPort_, hostName, err)) {
        ncpPort_.close();
        QMessageBox::warning(this, "Connect", "Cannot open the proxy port:\n" + err);
        return false;
    }

    Session::Callbacks cb;
    cb.writeNcp = [this](const uint8_t *d, size_t n) {
        return ncpPort_.isOpen() && ncpPort_.write(reinterpret_cast<const char *>(d), qint64(n)) == qint64(n);
    };
    cb.writeHost = [this](const uint8_t *d, size_t n) {
        return hostPort_.isOpen() && hostPort_.write(reinterpret_cast<const char *>(d), qint64(n)) == qint64(n);
    };
    cb.added = [this](const Record &r) { (r.dir == Dir::HostToNcp ? txModel_ : rxModel_)->queueAdd(r); };
    cb.updated = [this](const Record &r) { (r.dir == Dir::HostToNcp ? txModel_ : rxModel_)->update(r); };
    session_.reset(new Session(proxy ? Session::Mode::Proxy : Session::Mode::Direct, cb));

    linkText_ = QString("NCP %1 @ %2%3").arg(ncpName, baudCombo_->currentText(), rtsCts_->isChecked() ? " RTS/CTS" : "");
    if (proxy) linkText_ += QString("  |  proxy: host app on the other end of %1").arg(hostName);
    setConnectedUi(true);
    updateStatus();
    return true;
}

void MainWindow::onConnect() { connectPorts(); }

void MainWindow::closeAll(const QString &why) {
    if (session_) {
        session_->close(nowUs());
        txModel_->flush();
        rxModel_->flush();
        session_.reset();
    }
    if (ncpPort_.isOpen()) ncpPort_.close();
    if (hostPort_.isOpen()) hostPort_.close();
    if (!why.isEmpty()) linkText_ = why;
    if (connectBtn_) setConnectedUi(false);
}

void MainWindow::onDisconnect() {
    closeAll("Disconnected");
    updateStatus();
}

void MainWindow::onProxyToggled(bool on) {
    hostLabel_->setVisible(on);
    hostCombo_->setVisible(on);
}

void MainWindow::setConnectedUi(bool on) {
    connectBtn_->setEnabled(!on);
    disconnectBtn_->setEnabled(on);
    ncpCombo_->setEnabled(!on);
    hostCombo_->setEnabled(!on);
    baudCombo_->setEnabled(!on);
    rtsCts_->setEnabled(!on);
    proxyToggle_->setEnabled(!on);
    refreshBtn_->setEnabled(!on);
    updateSendPreview();
}

int64_t MainWindow::nowUs() const { return clock_.nsecsElapsed() / 1000; }

void MainWindow::onNcpReadyRead() {
    const QByteArray d = ncpPort_.readAll();
    if (session_ && !d.isEmpty()) session_->fromNcp(reinterpret_cast<const uint8_t *>(d.constData()), size_t(d.size()), nowUs());
}

void MainWindow::onHostReadyRead() {
    const QByteArray d = hostPort_.readAll();
    if (session_ && !d.isEmpty()) session_->fromHost(reinterpret_cast<const uint8_t *>(d.constData()), size_t(d.size()), nowUs());
}

void MainWindow::onPortError(QSerialPort::SerialPortError e) {
    if (e == QSerialPort::NoError || e == QSerialPort::TimeoutError) return;
    if (e == QSerialPort::UnsupportedOperationError && settingLines_) return;
    auto *port = qobject_cast<QSerialPort *>(sender());
    const QString which = port == &hostPort_ ? "proxy port" : "NCP port";
    const QString msg = QString("%1 error: %2").arg(which, port ? port->errorString() : QString::number(int(e)));
    if (e == QSerialPort::ResourceError || e == QSerialPort::PermissionError) {
        closeAll(msg + " -- disconnected");
        updateStatus();
    } else {
        statusBar()->showMessage(msg, 5000);
    }
}

/* -------------------------------------------------------------------- tick */

void MainWindow::onTick() {
    if (session_) session_->tick(nowUs());
    const int txBefore = txModel_->rowCount(), rxBefore = rxModel_->rowCount();
    txModel_->flush();
    rxModel_->flush();
    const bool txNew = txModel_->rowCount() != txBefore, rxNew = rxModel_->rowCount() != rxBefore;
    if (autoScroll_->isChecked()) {
        if (txNew) txView_->scrollToBottom();
        if (rxNew) rxView_->scrollToBottom();
    }
    if (rxNew && followRx_->isChecked()) showDecode(rxModel_->recordAt(rxModel_->rowCount() - 1));
    updateStatus();
}

void MainWindow::updateStatus() {
    QString s = linkText_.isEmpty() ? QString("Not connected") : linkText_;
    if (session_) {
        const auto &st = session_->stats;
        s += QString("   |   frames %1   unsynced bytes %2   errors %3   warnings %4   awaiting response %5   queued %6")
                 .arg(st.frames).arg(st.unsyncedBytes).arg(st.errors).arg(st.warnings)
                 .arg(session_->outstanding()).arg(session_->toolQueued());
    }
    status_->setText(s);
}

/* --------------------------------------------------------------- send side */

QByteArray MainWindow::parseHex(const QString &text, bool *ok) const {
    QString t = text;
    t.replace(QRegularExpression("0[xX]"), "");
    t.remove(QRegularExpression("[\\s,;:\\-]"));
    *ok = !t.contains(QRegularExpression("[^0-9A-Fa-f]")) && (t.size() % 2 == 0);
    return *ok ? QByteArray::fromHex(t.toLatin1()) : QByteArray();
}

void MainWindow::onCommandPicked(const QString &name) {
    const bgapi_msg_def_t *d = findCommandByName(name.trimmed().toStdString());
    if (!d) return;
    QByteArray pl(int(d->min_len), '\0');
    payloadEdit_->setText(QString(pl.toHex(' ')).toUpper());
    const unsigned len = d->min_len;
    const uint32_t hdr = d->id | ((len & 0xffu) << 8) | ((len & 0x700u) >> 8);
    QByteArray h;
    for (int i = 0; i < 4; ++i) h.append(char(hdr >> (8 * i)));
    headerEdit_->setText(QString(h.toHex(' ')).toUpper());
    payloadEdit_->setFocus();
}

void MainWindow::updateSendPreview() {
    if (updatingHeader_) return;
    bool hok = false, pok = false;
    QByteArray h = parseHex(headerEdit_->text(), &hok);
    const QByteArray p = parseHex(payloadEdit_->text(), &pok);
    payloadSize_->setText(pok ? QString::number(p.size()) : "?");

    if (hok && h.size() == 4 && pok && autoLen_->isChecked()) {
        const unsigned len = unsigned(p.size());
        const uint8_t b0 = uint8_t((uint8_t(h[0]) & 0xF8) | ((len >> 8) & 0x07));
        const uint8_t b1 = uint8_t(len & 0xff);
        if (uint8_t(h[0]) != b0 || uint8_t(h[1]) != b1) {
            h[0] = char(b0);
            h[1] = char(b1);
            updatingHeader_ = true;
            const int cur = headerEdit_->cursorPosition();
            headerEdit_->setText(QString(h.toHex(' ')).toUpper());
            headerEdit_->setCursorPosition(cur);
            updatingHeader_ = false;
        }
    }

    QString msg;
    bool sendable = false;
    if (!hok || h.size() != 4) {
        headerSize_->setText("?");
        msg = headerEdit_->text().trimmed().isEmpty() ? QString("Enter a 4-byte header or pick a command.")
                                                       : QString("<span style='color:#c0392b'>Header must be exactly 4 hex bytes.</span>");
    } else if (!pok) {
        headerSize_->setText(QString::number(payloadLenOf(headerFromBytes(reinterpret_cast<const uint8_t *>(h.constData())))));
        msg = "<span style='color:#c0392b'>Payload is not valid hex (an even number of hex digits).</span>";
    } else {
        Record r;
        r.dir = Dir::HostToNcp;
        r.bytes.assign(h.begin(), h.end());
        r.bytes.insert(r.bytes.end(), p.begin(), p.end());
        analyseFrame(r);
        headerSize_->setText(QString::number(r.payloadLen));
        if (r.issues.empty()) {
            msg = QString("<span style='color:#1e8449'>&#10004; <b>%1</b> - header and %2-byte payload match the API</span>")
                      .arg(QString::fromStdString(r.name)).arg(r.payloadLen);
        } else {
            QStringList l;
            for (auto &i : r.issues) l << QString::fromStdString(i).toHtmlEscaped();
            msg = QString("<span style='color:%1'><b>%2</b>: %3</span>")
                      .arg(r.sev == Severity::Error ? "#c0392b" : "#b9770e", QString::fromStdString(r.name), l.join("; "));
        }
        sendable = true;
    }
    sendCheck_->setText(msg);
    sendBtn_->setEnabled(sendable && session_ != nullptr);
}

void MainWindow::onSend() {
    if (!session_ || !sendBtn_->isEnabled()) return;
    bool hok = false, pok = false;
    const QByteArray h = parseHex(headerEdit_->text(), &hok);
    const QByteArray p = parseHex(payloadEdit_->text(), &pok);
    if (!hok || !pok || h.size() != 4) return;
    std::vector<uint8_t> f(h.begin(), h.end());
    f.insert(f.end(), p.begin(), p.end());
    Record r;
    r.dir = Dir::HostToNcp;
    r.bytes = f;
    analyseFrame(r);
    if (r.sev == Severity::Error) {
        QStringList l;
        for (auto &i : r.issues) l << QString::fromStdString(i);
        if (QMessageBox::question(this, "Send", "This frame does not match the BGAPI definition:\n\n- " + l.join("\n- ") +
                                  "\n\nThe NCP may lose frame sync. Send anyway?") != QMessageBox::Yes)
            return;
    }
    session_->sendFromTool(f, nowUs());
}

void MainWindow::sendHex(const QString &header, const QString &payload) {
    headerEdit_->setText(header);
    payloadEdit_->setText(payload);
    updateSendPreview();
    onSend();
}

/* --------------------------------------------------------- decode / tables */

void MainWindow::showDecode(const Record *r) {
    if (!r) {
        decode_->setPlainText("Click a row in TX or RX to decode it.\n"
                              "With 'follow latest RX' ticked, the newest RX frame is shown here.");
        return;
    }
    const FrameModel *m = r->dir == Dir::HostToNcp ? txModel_ : rxModel_;
    decode_->setPlainText(m->timeText(r->tUs) + "\n" + QString::fromStdString(decodeText(*r)));
}

void MainWindow::onSelectionChanged(QTableView *view) {
    QModelIndex idx = view->selectionModel()->currentIndex();
    if (!idx.isValid()) return;
    const Record *r = nullptr;
    if (view == rxView_) r = rxModel_->recordAt(rxFilter_->mapToSource(idx).row());
    else r = txModel_->recordAt(idx.row());
    if (!r) return;
    followRx_->setChecked(false);
    showDecode(r);
}

void MainWindow::onClear() {
    txModel_->clear();
    rxModel_->clear();
    showDecode(nullptr);
}

bool MainWindow::saveLog(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream out(&f);
    std::vector<const Record *> all;
    for (const auto &r : txModel_->records()) all.push_back(&r);
    for (const auto &r : rxModel_->records()) all.push_back(&r);
    std::sort(all.begin(), all.end(), [](const Record *a, const Record *b) { return a->seq < b->seq; });
    const bool csv = path.endsWith(".csv", Qt::CaseInsensitive);
    auto q = [](QString s) { return "\"" + s.replace("\"", "\"\"") + "\""; };
    if (csv) out << "seq,time,dir,src,name,payload_len,result,status,hex,ascii,issues,state,route,match_seq,rtt_ms\n";
    for (const Record *r : all) {
        const FrameModel *m = r->dir == Dir::HostToNcp ? txModel_ : rxModel_;
        const QString dir = r->dir == Dir::HostToNcp ? "TX" : "RX";
        const QString src = r->src == Source::Host ? "HOST" : r->src == Source::Tool ? "TOOL" : "NCP";
        const QString res = r->hasResult ? QString("0x%1").arg(r->result, 4, 16, QChar('0')) : QString();
        const char *sn = r->hasResult ? statusName(r->result) : nullptr;
        QStringList issues;
        for (auto &i : r->issues) issues << QString::fromStdString(i);
        if (csv) {
            out << r->seq << ',' << m->timeText(r->tUs) << ',' << dir << ',' << src << ','
                << q(QString::fromStdString(r->name)) << ',' << r->payloadLen << ',' << res << ',' << (sn ? sn : "") << ','
                << q(QString::fromStdString(hexBytes(r->bytes))) << ',' << q(QString::fromStdString(asciiBytes(r->bytes))) << ','
                << q(issues.join("; ")) << ',' << q(QString::fromStdString(r->state)) << ',' << q(QString::fromStdString(r->route)) << ','
                << (r->matchSeq ? QString::number(r->matchSeq) : QString()) << ','
                << (r->rttUs >= 0 ? QString::number(r->rttUs / 1000.0, 'f', 3) : QString()) << '\n';
        } else {
            out << QString("%1 %2 %3 %4 %5").arg(r->seq, 6).arg(m->timeText(r->tUs), dir, src.leftJustified(4), QString::fromStdString(r->name));
            if (r->hasResult) out << "  result=" << res << (sn ? QString(" ") + sn : QString());
            if (r->matchSeq) out << QString("  [%1 #%2, %3 ms]").arg(r->kind == BGAPI_KIND_RSP ? "answers" : "answered by").arg(r->matchSeq).arg(r->rttUs / 1000.0, 0, 'f', 3);
            out << "\n        " << QString::fromStdString(hexBytes(r->bytes)) << "   |" << QString::fromStdString(asciiBytes(r->bytes)) << "|\n";
            if (!r->state.empty()) out << "        state: " << QString::fromStdString(r->state) << "\n";
            if (!r->route.empty()) out << "        route: " << QString::fromStdString(r->route) << "\n";
            for (auto &i : issues) out << "        ! " << i << "\n";
        }
    }
    return true;
}

void MainWindow::onSave() {
    const QString path = QFileDialog::getSaveFileName(this, "Save log", "bgapi_log.txt", "Text log (*.txt);;CSV (*.csv)");
    if (path.isEmpty()) return;
    if (!saveLog(path)) QMessageBox::warning(this, "Save log", "Could not write " + path);
}
