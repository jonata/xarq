#include "sqlitemini.h"
#include <QRegularExpression>
#include <cstring>

namespace xara {
namespace {
quint64 varint(const uchar*& p, const uchar* end){
    quint64 v = 0;
    for(int i = 0; i < 9 && p < end; ++i){
        const uchar b = *p++;
        if(i == 8){ v = (v << 8) | b; break; }
        v = (v << 7) | (b & 0x7f);
        if(!(b & 0x80)) break;
    }
    return v;
}
inline int be16(const uchar* p){ return p[0] << 8 | p[1]; }
inline quint32 be32(const uchar* p){ return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | p[3]; }

// "CREATE TABLE x(a INTEGER PRIMARY KEY, b TEXT, c ...)": the column names, and which one aliases the rowid.
QStringList columnsOf(const QString& sql, int* alias){
    QStringList cols; *alias = -1;
    const int open = sql.indexOf(QLatin1Char('('));
    if(open < 0) return cols;
    int depth = 0, start = open + 1; QChar quote;
    auto flush = [&](int end){
        const QString def = sql.mid(start, end - start).trimmed();
        if(def.isEmpty()) return;
        const QString up = def.toUpper();
        if(up.startsWith("PRIMARY ") || up.startsWith("UNIQUE") || up.startsWith("CHECK") || up.startsWith("FOREIGN ") || up.startsWith("CONSTRAINT ")) return;
        QString name = def.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
        name.remove(QLatin1Char('"')); name.remove(QLatin1Char('`')); name.remove(QLatin1Char('[')); name.remove(QLatin1Char(']'));
        if(up.contains("INTEGER PRIMARY KEY")) *alias = cols.size();
        cols << name;
    };
    for(int i = open + 1; i < sql.size(); ++i){
        const QChar c = sql[i];
        if(!quote.isNull()){ if(c == quote) quote = QChar(); continue; }
        if(c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')){ quote = c; continue; }
        if(c == QLatin1Char('(')) ++depth;
        else if(c == QLatin1Char(')')){ if(depth == 0){ flush(i); break; } --depth; }
        else if(c == QLatin1Char(',') && depth == 0){ flush(i); start = i + 1; }
    }
    return cols;
}
} // namespace

// A cell's full payload: what's on the page plus any overflow chain.
QByteArray SqliteMini::payload(const uchar* cell, const uchar* pageEnd, qint64* rowid) const {
    const uchar* p = cell;
    const quint64 size = varint(p, pageEnd);
    if(rowid) *rowid = qint64(varint(p, pageEnd));
    if(size > quint64(m_file.size())) return QByteArray();
    const qint64 U = m_usable, X = U - 35;
    qint64 local = qint64(size);
    if(qint64(size) > X){
        const qint64 M = ((U - 12) * 32 / 255) - 23;
        const qint64 K = M + ((qint64(size) - M) % (U - 4));
        local = K <= X ? K : M;
    }
    if(p + local > pageEnd) return QByteArray();
    QByteArray out(reinterpret_cast<const char*>(p), int(local));
    if(local == qint64(size)) return out;
    if(p + local + 4 > pageEnd) return QByteArray();
    quint32 next = be32(p + local);
    const uchar* base = reinterpret_cast<const uchar*>(m_file.constData());
    int guard = m_pages + 1;
    while(next != 0 && out.size() < qint64(size)){
        if(next > quint32(m_pages) || --guard < 0) return QByteArray();
        const uchar* page = base + qint64(next - 1) * m_pageSize;
        const qint64 take = qMin<qint64>(U - 4, qint64(size) - out.size());
        out.append(reinterpret_cast<const char*>(page + 4), int(take));
        next = be32(page);
    }
    return out.size() == qint64(size) ? out : QByteArray();
}

bool SqliteMini::walk(int page, QVector<QVector<QVariant>>& out, int depth, QVector<bool>& seen, qint64& budget) const {
    if(page < 1 || page > m_pages || depth > 64 || seen[page]) return false;
    seen[page] = true;
    const uchar* base = reinterpret_cast<const uchar*>(m_file.constData());
    const uchar* pg = base + qint64(page - 1) * m_pageSize;
    const uchar* end = pg + m_usable;
    const uchar* h = pg + (page == 1 ? 100 : 0);
    const int type = h[0];
    const int cells = be16(h + 3);
    if(type == 5){              // interior: left children, then the rightmost
        const uchar* ptrs = h + 12;
        if(ptrs + cells * 2 > end) return false;
        for(int i = 0; i < cells; ++i){
            const int off = be16(ptrs + i*2);
            if(off + 4 > m_usable) return false;
            if(!walk(int(be32(pg + off)), out, depth + 1, seen, budget)) return false;
        }
        return walk(int(be32(h + 8)), out, depth + 1, seen, budget);
    }
    if(type != 13) return false;
    const uchar* ptrs = h + 8;
    if(ptrs + cells * 2 > end) return false;
    for(int i = 0; i < cells; ++i){
        const int off = be16(ptrs + i*2);
        if(off >= m_usable) return false;
        qint64 rowid = 0;
        const QByteArray pl = payload(pg + off, end, &rowid);
        budget -= pl.size();
        if(budget < 0) return false;
        const uchar* p = reinterpret_cast<const uchar*>(pl.constData());
        const uchar* pe = p + pl.size();
        const uchar* q = p;
        const quint64 hdrLen = varint(q, pe);
        if(hdrLen > quint64(pl.size())) continue;
        const uchar* hdrEnd = p + hdrLen;
        const uchar* body = hdrEnd;
        QVector<QVariant> row;
        row.push_back(QVariant::fromValue<qint64>(rowid));     // column 0 of the result is always the rowid
        while(q < hdrEnd){
            const quint64 st = varint(q, hdrEnd);
            QVariant v;
            if(st == 0) {}
            else if(st >= 1 && st <= 6){
                static const int sizes[] = { 0, 1, 2, 3, 4, 6, 8 };
                const int n = sizes[st];
                if(body + n > pe) break;
                qint64 x = (body[0] & 0x80) ? -1 : 0;
                for(int b = 0; b < n; ++b) x = (x << 8) | body[b];
                body += n;
                v = QVariant::fromValue<qint64>(x);
            } else if(st == 7){
                if(body + 8 > pe) break;
                quint64 x = 0; for(int b = 0; b < 8; ++b) x = (x << 8) | body[b];
                double d; std::memcpy(&d, &x, 8);
                body += 8; v = d;
            } else if(st == 8) v = QVariant::fromValue<qint64>(0);
            else if(st == 9) v = QVariant::fromValue<qint64>(1);
            else if(st >= 12){
                const quint64 n = (st - 12 - (st & 1)) / 2;
                if(n > quint64(pe - body)) break;
                if(st & 1){
                    if(m_utf16le) v = QString::fromUtf16(reinterpret_cast<const char16_t*>(QByteArray(reinterpret_cast<const char*>(body), int(n)).constData()), int(n / 2));
                    else if(m_utf16be){ QString s; for(quint64 k = 0; k + 1 < n; k += 2) s += QChar(ushort(body[k] << 8 | body[k+1])); v = s; }
                    else v = QString::fromUtf8(reinterpret_cast<const char*>(body), int(n));
                } else v = QByteArray(reinterpret_cast<const char*>(body), int(n));
                body += n;
            }
            row.push_back(v);
        }
        out.push_back(row);
    }
    return true;
}

QVector<QVector<QVariant>> SqliteMini::scan(int root) const {
    QVector<QVector<QVariant>> out;
    QVector<bool> seen(m_pages + 1, false);
    qint64 budget = qint64(m_file.size()) * 4 + (64 << 20);
    if(!walk(root, out, 0, seen, budget)) out.clear();
    return out;
}

bool SqliteMini::open(const QByteArray& file){
    m_tables.clear();
    if(file.size() < 512 || std::memcmp(file.constData(), "SQLite format 3\0", 16) != 0) return false;
    const uchar* h = reinterpret_cast<const uchar*>(file.constData());
    int ps = be16(h + 16); if(ps == 1) ps = 65536;
    if(ps < 512 || (ps & (ps - 1))) return false;
    const int reserved = h[20];
    if(ps - reserved < 480) return false;
    m_file = file; m_pageSize = ps; m_usable = ps - reserved;
    m_pages = int(file.size() / ps);
    const quint32 enc = be32(h + 56);
    m_utf16le = enc == 2; m_utf16be = enc == 3;
    if(m_pages < 1) return false;
    for(const QVector<QVariant>& r : scan(1)){       // sqlite_schema: type, name, tbl_name, rootpage, sql
        if(r.size() < 6 || r[1].toString() != QLatin1String("table")) continue;
        Table t; t.root = int(r[4].toLongLong());
        t.columns = columnsOf(r[5].toString(), &t.rowidAlias);
        if(t.root > 0) m_tables.insert(r[2].toString(), t);
    }
    return !m_tables.isEmpty();
}

QVector<SqliteMini::Row> SqliteMini::rows(const QString& table) const {
    QVector<Row> out;
    const auto it = m_tables.constFind(table);
    if(it == m_tables.constEnd()) return out;
    const Table& t = it.value();
    for(const QVector<QVariant>& r : scan(t.root)){
        Row row;
        for(int c = 0; c < t.columns.size() && c + 1 < r.size(); ++c)
            if(r[c + 1].isValid()) row.insert(t.columns[c], r[c + 1]);
        if(t.rowidAlias >= 0 && t.rowidAlias < t.columns.size() && !r.isEmpty()) row.insert(t.columns[t.rowidAlias], r[0]);
        out.push_back(row);
    }
    return out;
}
} // namespace xara
