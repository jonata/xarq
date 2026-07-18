#include "hyphen.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStringConverter>

namespace xara {

Hyphenator& Hyphenator::instance() { static Hyphenator h; return h; }

Hyphenator::Hyphenator() {
    // Prefer English, then Portuguese, then any installed dictionary.
    const QStringList prefs = {
        "/usr/share/hyphen/hyph_en_US.dic", "/usr/share/hyphen/hyph_en_GB.dic", "/usr/share/hyphen/hyph_en.dic",
        "/usr/share/hyphen/hyph_pt_PT.dic", "/usr/share/hyphen/hyph_pt_BR.dic",
        "/usr/share/myspell/hyph_en_US.dic",
    };
    for (const QString& p : prefs) if (load(p)) return;
    // fallback: scan the hyphen directory for any hyph_*.dic
    QFileInfo dir("/usr/share/hyphen");
    if (dir.isDir()) { for (const QString& name : QDir("/usr/share/hyphen").entryList({"hyph_*.dic"}, QDir::Files))
        if (load("/usr/share/hyphen/"+name)) return; }
}

bool Hyphenator::load(const QString& path) {
    QFile f(path); if (!f.exists() || !f.open(QIODevice::ReadOnly)) return false;
    QByteArray raw = f.readAll(); if (raw.isEmpty()) return false;
    const int nl = raw.indexOf('\n');
    QString charset = QString::fromLatin1(nl>=0 ? raw.left(nl) : raw).trimmed();
    // Decode the body with the declared charset (libhyphen header line names it).
    QStringDecoder dec(charset.toLatin1().constData());
    if (!dec.isValid()) dec = QStringDecoder(QStringConverter::Latin1);
    const QString text = dec(QByteArray(raw).mid(nl>=0 ? nl+1 : 0));
    for (QString line : text.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('%')) continue;
        if (line.startsWith("LEFTHYPHENMIN"))  { m_lmin = line.section(' ',1,1).toInt(); continue; }
        if (line.startsWith("RIGHTHYPHENMIN")) { m_rmin = line.section(' ',1,1).toInt(); continue; }
        if (line.startsWith("NEXTLEVEL") || line.startsWith("COMPOUND") || line.contains('/')) continue;  // skip directives / exception syntax we don't support
        // A pattern is letters/dots interleaved with digit weights.
        QString letters; QVector<int> w; w.append(0);
        bool any=false;
        for (const QChar c : line) {
            if (c.isDigit()) { w.last() = c.digitValue(); any=true; }
            else { letters.append(c); w.append(0); }
        }
        if (letters.isEmpty()) continue;
        if (!any) continue;                       // a line with no weights carries no Liang info
        m_pat.insert(letters.toLower(), w);
        m_maxLen = qMax(m_maxLen, int(letters.size()));
    }
    if (m_pat.isEmpty()) return false;
    if (m_lmin < 1) m_lmin = 1; if (m_rmin < 1) m_rmin = 1;
    m_lang = QFileInfo(path).completeBaseName().section('_',1);
    return true;
}

QVector<int> Hyphenator::points(const QString& word) const {
    QVector<int> out;
    if (m_pat.isEmpty() || word.size() < m_lmin + m_rmin) return out;
    const QString w = "." + word.toLower() + ".";
    const int n = w.size();
    QVector<int> val(n + 1, 0);
    for (int i = 0; i < n - 1; ++i) {
        const int jmax = qMin(i + m_maxLen, n);
        for (int j = i + 1; j <= jmax; ++j) {
            auto it = m_pat.constFind(w.mid(i, j - i));
            if (it == m_pat.constEnd()) continue;
            const QVector<int>& p = *it;
            for (int k = 0; k < p.size() && i + k <= n; ++k) val[i+k] = qMax(val[i+k], p[k]);
        }
    }
    // a hyphen may go after word[i] when val[i+2] is odd, honouring left/right minimums
    for (int i = m_lmin - 1; i <= word.size() - m_rmin - 1; ++i)
        if (i + 2 < val.size() && (val[i+2] & 1)) out.append(i);
    return out;
}

} // namespace xara
