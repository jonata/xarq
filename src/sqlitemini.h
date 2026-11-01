// A read-only reader for SQLite database files held in memory: whole tables, by name. Some design formats
// keep their document in an embedded database, and linking all of SQLite to read one is not worth it.
#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

namespace xara {
class SqliteMini {
public:
    using Row = QHash<QString,QVariant>;   // NULL columns are absent
    bool open(const QByteArray& file);     // keeps a shared copy of the bytes
    bool hasTable(const QString& name) const { return m_tables.contains(name); }
    QStringList tables() const { return m_tables.keys(); }
    QVector<Row> rows(const QString& table) const;   // empty if there's no such table or it's damaged
private:
    struct Table { int root = 0; QStringList columns; int rowidAlias = -1; };
    QVector<QVector<QVariant>> scan(int root) const;
    bool walk(int page, QVector<QVector<QVariant>>& out, int depth, QVector<bool>& seen, qint64& budget) const;
    QByteArray payload(const uchar* cell, const uchar* pageEnd, qint64* rowid) const;
    QByteArray m_file;
    int m_pageSize = 0, m_usable = 0, m_pages = 0;
    bool m_utf16le = false, m_utf16be = false;
    QHash<QString,Table> m_tables;
};
}
