// hyphen.h — Liang competing-pattern hyphenation, loading libhyphen .dic files.
// Loads the first available /usr/share/hyphen/hyph_*.dic (preferring en, then any),
// so it works for whatever languages are installed and degrades to "not ready" when
// none are present (the text wrapper then just never hyphenates).
#pragma once
#include <QString>
#include <QVector>
#include <QHash>

namespace xara {

class Hyphenator {
public:
    static Hyphenator& instance();
    bool ready() const { return !m_pat.isEmpty(); }
    QString language() const { return m_lang; }
    // 0-based indices i such that a hyphen may be inserted AFTER word[i] (respecting min edges).
    QVector<int> points(const QString& word) const;

private:
    Hyphenator();
    bool load(const QString& path);
    QHash<QString, QVector<int>> m_pat;   // pattern letters -> per-gap weights (len = letters+1)
    int m_lmin = 2, m_rmin = 2, m_maxLen = 2;
    QString m_lang;
};

} // namespace xara
