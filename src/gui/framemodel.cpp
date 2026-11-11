#include "framemodel.h"

#include <QBrush>
#include <QColor>
#include <QFont>

using namespace bgmon;

FrameModel::FrameModel(QDateTime t0, QObject *parent) : QAbstractTableModel(parent), t0_(t0) {}

int FrameModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : rows_.size(); }
int FrameModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : ColCount; }

QString FrameModel::timeText(int64_t tUs) const {
    QDateTime t = t0_.addMSecs(tUs / 1000);
    return t.toString("HH:mm:ss.zzz") + QString::number((tUs / 100) % 10);
}

static QString typeText(const Record &r) {
    if (r.type == ItemType::Unsynced) return "RAW";
    if (r.type == ItemType::Truncated) return "TRUNC";
    switch (r.kind) {
    case BGAPI_KIND_CMD: return "CMD";
    case BGAPI_KIND_RSP: return "RSP";
    default: return "EVT";
    }
}

static QString infoText(const Record &r) {
    QStringList parts;
    if (!r.issues.empty()) parts << QString::fromStdString(r.issues.front());
    if (!r.state.empty()) parts << QString::fromStdString(r.state);
    if (r.matchSeq) {
        if (r.kind == BGAPI_KIND_RSP) parts << QString("answers #%1 (%2 ms)").arg(r.matchSeq).arg(r.rttUs / 1000.0, 0, 'f', 1);
    }
    if (!r.route.empty()) parts << QString::fromStdString(r.route);
    return parts.join("  |  ");
}

QVariant FrameModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return QVariant();
    const Record &r = rows_[index.row()];

    if (role == RecordRole) return QVariant::fromValue<qulonglong>(r.seq);
    if (role == IsEventRole) return r.type == ItemType::Frame && r.kind == BGAPI_KIND_EVT;

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColSeq: return QVariant::fromValue<qulonglong>(r.seq);
        case ColTime: return timeText(r.tUs);
        case ColSrc: return r.src == Source::Host ? "HOST" : r.src == Source::Tool ? "TOOL" : "NCP";
        case ColType: return typeText(r);
        case ColName: return QString::fromStdString(r.name);
        case ColLen: return r.type == ItemType::Frame ? QVariant(int(r.payloadLen)) : QVariant(int(r.bytes.size()));
        case ColResult:
            if (!r.hasResult) return QVariant();
            return QString("0x%1").arg(r.result, 4, 16, QChar('0'));
        case ColHex: return QString::fromStdString(hexBytes(r.bytes));
        case ColAscii: return QString::fromStdString(asciiBytes(r.bytes));
        case ColInfo: return infoText(r);
        }
    }
    if (role == Qt::ToolTipRole) {
        if (index.column() == ColResult && r.hasResult) {
            const char *n = statusName(r.result);
            return n ? QString(n) : QString("unknown status");
        }
        return QString::fromStdString(decodeText(r));
    }
    if (role == Qt::FontRole && (index.column() == ColHex || index.column() == ColAscii)) {
        QFont f("Consolas");
        f.setStyleHint(QFont::Monospace);
        return f;
    }
    if (role == Qt::BackgroundRole) {
        if (r.sev == Severity::Error) return QBrush(QColor(110, 24, 24));
        if (r.sev == Severity::Warn) return QBrush(QColor(92, 72, 10));
        if (r.src == Source::Tool) return QBrush(QColor(18, 46, 82));
    }
    if (role == Qt::ForegroundRole) {
        if (index.column() == ColResult && r.hasResult && r.result != 0) return QBrush(QColor(255, 166, 77));
        if (r.type == ItemType::Frame && r.kind == BGAPI_KIND_EVT) return QBrush(QColor(127, 196, 255));
        return QBrush(QColor(225, 225, 225));
    }
    return QVariant();
}

QVariant FrameModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return QVariant();
    static const char *names[ColCount] = {"#", "Time", "Src", "Type", "Message", "Len", "Result", "Hex", "ASCII", "Status / route"};
    return names[section];
}

void FrameModel::queueAdd(const Record &r) { pending_.push_back(r); }

void FrameModel::update(const Record &r) {
    auto it = rowOfSeq_.find(r.seq);
    if (it == rowOfSeq_.end()) {
        for (auto &p : pending_)
            if (p.seq == r.seq) { p = r; return; }
        return;
    }
    rows_[it.value()] = r;
    emit dataChanged(index(it.value(), 0), index(it.value(), ColCount - 1));
}

void FrameModel::flush() {
    if (pending_.isEmpty()) return;
    if (rows_.size() + pending_.size() > maxRows_) {
        const int drop = qMin(rows_.size(), rows_.size() + pending_.size() - maxRows_ + maxRows_ / 10);
        beginRemoveRows(QModelIndex(), 0, drop - 1);
        rows_.remove(0, drop);
        endRemoveRows();
        rowOfSeq_.clear();
        for (int i = 0; i < rows_.size(); ++i) rowOfSeq_[rows_[i].seq] = i;
    }
    const int first = rows_.size();
    beginInsertRows(QModelIndex(), first, first + pending_.size() - 1);
    for (const auto &r : pending_) {
        rowOfSeq_[r.seq] = rows_.size();
        rows_.push_back(r);
    }
    pending_.clear();
    endInsertRows();
}

void FrameModel::clear() {
    beginResetModel();
    rows_.clear();
    pending_.clear();
    rowOfSeq_.clear();
    endResetModel();
}

const Record *FrameModel::recordAt(int row) const {
    return (row >= 0 && row < rows_.size()) ? &rows_[row] : nullptr;
}
