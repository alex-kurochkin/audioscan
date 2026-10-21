#pragma once
#include <QAbstractTableModel>
#include <QDateTime>
#include <QImage>
#include <QPersistentModelIndex>

// Model of audio files in the currently selected directory.
class FileTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0, ColPreview, ColDuration, ColSampleRate, ColSampleType,
        ColChannels, ColSize, ColComment, ColCreated, ColModified, ColCount
    };

    struct Row {
        QString path, name;
        qint64 size = 0;
        QDateTime created, modified;
        // audio info (invalid if broken file)
        bool broken = true;
        int sampleRate = 0;
        int channels = 0;
        qint64 frames = 0;
        QString sampleTypeName;
        QString comment;
        QImage preview;         // 340x20 at 72ppi
        bool previewLoaded = false;
    };

    explicit FileTableModel(QObject *parent = nullptr);

    void setRows(const QVector<Row> &rows);
    const QVector<Row> &rows() const { return rows_; }
    void updateRow(int row, const Row &r);   // in-place refresh with dataChanged
    void setPreview(int row, const QImage &img);
    QImage previewAt(int row) const;
    void updateVisibleColumns(const QVector<int> &orderedVisibleCols);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    // Sorting on Preview and Comment columns is disabled (spec 2.4).
    void sort(int column, Qt::SortOrder order) override;

    QString columnName(int col) const;
    static QString columnKey(int col);
    static int columnFromKey(const QString &key);

signals:
    void previewRequested(int row);   // lazy loading request

private:
    QVector<Row> rows_;
    QVector<int> visibleCols_;        // model-column order as displayed
    int sortCol_ = ColName;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
};
