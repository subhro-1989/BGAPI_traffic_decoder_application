/* framemodel.h -- table model for one pane (TX or RX). */
#ifndef FRAMEMODEL_H
#define FRAMEMODEL_H

#include <QAbstractTableModel>
#include <QDateTime>
#include <QHash>
#include <QVector>

#include "bgapi_core.h"

class FrameModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { ColSeq, ColTime, ColSrc, ColType, ColName, ColLen, ColResult, ColHex, ColAscii, ColInfo, ColCount };
    enum { RecordRole = Qt::UserRole + 1, IsEventRole };

    FrameModel(QDateTime t0, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    void queueAdd(const bgmon::Record &r);       /* batched; call flush() from a timer */
    void update(const bgmon::Record &r);
    void flush();
    void clear();
    void setTimeBase(QDateTime t0) { t0_ = t0; }

    const bgmon::Record *recordAt(int row) const;
    const QVector<bgmon::Record> &records() const { return rows_; }
    QString timeText(int64_t tUs) const;

private:
    QDateTime t0_;
    QVector<bgmon::Record> rows_;
    QVector<bgmon::Record> pending_;
    QHash<quint64, int> rowOfSeq_;
    int maxRows_ = 200000;
};

#endif
