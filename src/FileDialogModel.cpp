#include "FileDialogModel.h"
#include <QLocale>

static const char *kColumnTitles[] = {
    "Name", "Preview", "Duration", "Sample Rate", "Sample Type",
    "Channels", "Size", "Comment", "Date Created", "Date Modified"};
static const char *kColumnKeys[] = {
    "name","preview","duration","samplerate","sampletype",
    "channels","size","comment","created","modified"};

FileTableModel::FileTableModel(QObject *parent) : QAbstractTableModel(parent) {
    for (int i = 0; i < ColCount; ++i) visibleCols_ << i;
}

QString FileTableModel::columnName(int col) const {
    return QString::fromLatin1(kColumnTitles[std::clamp(col, 0, int(ColCount) - 1)]);
}
QString FileTableModel::columnKey(int col) {
    return QString::fromLatin1(kColumnKeys[std::clamp(col, 0, int(ColCount) - 1)]);
}
int FileTableModel::columnFromKey(const QString &key) {
    for (int i = 0; i < int(ColCount); ++i)
        if (key == QLatin1String(kColumnKeys[i])) return i;
    return -1;
}

void FileTableModel::setRows(const QVector<Row> &rows) {
    beginResetModel();
    rows_ = rows;
    endResetModel();
}

// Refresh a single row in place (emits dataChanged so the view repaints).
void FileTableModel::updateRow(int row, const Row &r) {
    if (row < 0 || row >= rows_.size()) return;
    rows_[row] = r;
    emit dataChanged(index(row, 0), index(row, visibleCols_.size() - 1));
}

void FileTableModel::setPreview(int row, const QImage &img) {
    if (row < 0 || row >= rows_.size()) return;
    rows_[row].preview = img;
    rows_[row].previewLoaded = true;
    int visCol = visibleCols_.indexOf(ColPreview);
    if (visCol >= 0)
        emit dataChanged(index(row, visCol), index(row, visCol));
}

QImage FileTableModel::previewAt(int row) const {
    return (row >= 0 && row < rows_.size()) ? rows_[row].preview : QImage();
}


void FileTableModel::updateVisibleColumns(const QVector<int> &orderedVisibleCols) {
    beginResetModel();
    visibleCols_ = orderedVisibleCols;
    endResetModel();
}

int FileTableModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

int FileTableModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : visibleCols_.size();
}

QVariant FileTableModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    int col = visibleCols_[index.column()];
    const Row &r = rows_[index.row()];

    if (role == Qt::ForegroundRole && r.broken)
        return QColor(220, 40, 40);   // broken files in red (spec 2.4/3.3)
    if (role != Qt::DisplayRole && role != Qt::DecorationRole && role != Qt::ToolTipRole)
        return {};

    if (r.broken) {
        // Broken rows still show basic filesystem info (name, size, dates),
        // only audio params and preview are omitted (spec 3.3).
        if (role != Qt::DisplayRole) return {};
        switch (col) {
        case ColName:     return r.name;
        case ColSize:     return QLocale().formattedDataSize(r.size);
        case ColCreated:  return r.created.isValid() ? r.created.toString(Qt::ISODate) : QVariant();
        case ColModified: return r.modified.isValid() ? r.modified.toString(Qt::ISODate) : QVariant();
        default:          return {};
        }
    }

    switch (col) {
    case ColName:     return role == Qt::DisplayRole ? r.name : QVariant();
    case ColPreview:
        if (role == Qt::DecorationRole) return r.preview;
        if (role == Qt::DisplayRole && !r.previewLoaded)
            Q_EMIT const_cast<FileTableModel *>(this)->previewRequested(index.row());
        return {};
    case ColDuration: {
        double sec = r.sampleRate ? double(r.frames) / r.sampleRate : 0.0;
        int mm = int(sec) / 60, ss = int(sec) % 60;
        return QStringLiteral("%1:%2").arg(mm).arg(ss, 2, 10, QLatin1Char('0'));
    }
    case ColSampleRate: return QLocale().toString(r.sampleRate);
    case ColSampleType: return r.sampleTypeName;
    case ColChannels:   return r.channels;
    case ColSize:       return QLocale().formattedDataSize(r.size);
    case ColComment:    return role == Qt::DisplayRole ? r.comment : QVariant();
    case ColCreated:    return r.created.toString(Qt::ISODate);
    case ColModified:   return r.modified.toString(Qt::ISODate);
    }
    return {};
}

QVariant FileTableModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return columnName(visibleCols_[section]);
}

Qt::ItemFlags FileTableModel::flags(const QModelIndex &index) const {
    auto f = QAbstractTableModel::flags(index);
    if (index.isValid()) {
        int col = visibleCols_[index.column()];
        if (col == ColPreview || col == ColComment)
            f &= ~Qt::ItemIsSelectable;   // no sort indicator either (see view setup)
    }
    return f;
}

void FileTableModel::sort(int column, Qt::SortOrder order) {
    if (column == ColPreview || column == ColComment) return;   // forbidden per spec
    sortCol_ = column; sortOrder_ = order;
    std::stable_sort(rows_.begin(), rows_.end(), [&](const Row &a, const Row &b) {
        auto cmp = [&](auto ka, auto kb) {
            if (order == Qt::AscendingOrder) return ka < kb;
            return ka > kb;
        };
        switch (column) {
        case ColName:      return cmp(a.name.toLower(), b.name.toLower());
        case ColDuration: {
            double da = a.sampleRate ? double(a.frames) / a.sampleRate : -1;
            double db = b.sampleRate ? double(b.frames) / b.sampleRate : -1;
            return cmp(da, db);
        }
        case ColSampleRate: return cmp(a.sampleRate, b.sampleRate);
        case ColSampleType: return cmp(a.sampleTypeName, b.sampleTypeName);
        case ColChannels:   return cmp(a.channels, b.channels);
        case ColSize:       return cmp(a.size, b.size);
        case ColCreated:    return cmp(a.created, b.created);
        case ColModified:   return cmp(a.modified, b.modified);
        default:            return false;
        }
    });
    beginResetModel();
    endResetModel();
}
