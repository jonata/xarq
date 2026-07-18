#include "spell.h"
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QSettings>
#include <algorithm>

namespace xara {

Speller& Speller::instance() { static Speller s; return s; }

void Speller::loadUserWords(){
    QSettings st;
    const QStringList uw = st.value("spell/userWords").toStringList();
    for(const QString& w: uw){ const QString t=w.trimmed().toLower(); if(!t.isEmpty()) m_userWords.insert(t); }
}
void Speller::addUserWord(const QString& word){
    const QString w=word.trimmed().toLower(); if(w.isEmpty()) return;
    m_userWords.insert(w);
    QSettings st; st.setValue("spell/userWords", QStringList(m_userWords.values()));
}
void Speller::removeUserWord(const QString& word){
    const QString w=word.trimmed().toLower(); if(!m_userWords.remove(w)) return;
    QSettings st; st.setValue("spell/userWords", QStringList(m_userWords.values()));
}

Speller::Speller() {
    loadUserWords();
    // Prefer a plain one-word-per-line list; fall back to a hunspell .dic (word/FLAGS lines).
    const QStringList plain = {
        "/usr/share/dict/words", "/usr/share/dict/american-english", "/usr/share/dict/british-english",
        "/usr/share/dict/cracklib-small"
    };
    for (const QString& p : plain) {
        QFile f(p); if (!f.exists() || !f.open(QIODevice::ReadOnly|QIODevice::Text)) continue;
        QTextStream in(&f);
        while (!in.atEnd()) { QString w=in.readLine().trimmed(); if(w.isEmpty()) continue;
            // strip possessive: dict entries like "cat's" -> also store "cat"
            m_words.insert(w.toLower());
            if (w.endsWith("'s")) m_words.insert(w.left(w.size()-2).toLower());
        }
        if (!m_words.isEmpty()) return;
    }
    const QStringList dics = { "/usr/share/hunspell/en_US.dic", "/usr/share/myspell/en_US.dic" };
    for (const QString& p : dics) {
        QFile f(p); if (!f.exists() || !f.open(QIODevice::ReadOnly|QIODevice::Text)) continue;
        QTextStream in(&f); bool first=true;
        while (!in.atEnd()) { QString line=in.readLine().trimmed(); if(line.isEmpty()) continue;
            if (first){ first=false; bool ok=false; line.toInt(&ok); if(ok) continue; }   // header count line
            int slash=line.indexOf('/'); QString w = slash>=0 ? line.left(slash) : line;
            w=w.trimmed(); if(!w.isEmpty()) m_words.insert(w.toLower());
        }
        if (!m_words.isEmpty()) return;
    }
}

bool Speller::known(const QString& word) const {
    if (m_words.isEmpty()) return true;          // no dictionary => treat everything as known
    QString w = word.toLower();
    if (w.isEmpty()) return true;
    if (m_userWords.contains(w)) return true;    // user-accepted words
    if (m_words.contains(w)) return true;
    // light morphology: possessive and simple plural/3rd-person forms
    if (w.endsWith("'s") && m_words.contains(w.left(w.size()-2))) return true;
    if (w.endsWith("s")  && m_words.contains(w.left(w.size()-1))) return true;
    if (w.endsWith("es") && m_words.contains(w.left(w.size()-2))) return true;
    if (w.endsWith("ed") && m_words.contains(w.left(w.size()-2))) return true;
    if (w.endsWith("ing")&& w.size()>4 && m_words.contains(w.left(w.size()-3))) return true;
    return false;
}

QStringList Speller::check(const QString& text, int maxReport) const {
    QStringList out; if (m_words.isEmpty()) return out;
    QSet<QString> seen;
    static const QRegularExpression tok("[A-Za-z][A-Za-z']*");
    auto it = tok.globalMatch(text);
    while (it.hasNext()) {
        QString w = it.next().captured(0);
        if (w.size() < 3) continue;                                  // skip tiny words
        if (w == w.toUpper()) continue;                              // acronyms (ALLCAPS)
        if (w[0].isUpper()) continue;                                // proper nouns / sentence starts
        if (known(w)) continue;
        QString key=w.toLower(); if (seen.contains(key)) continue; seen.insert(key);
        out << w; if (out.size() >= maxReport) break;
    }
    return out;
}

// Norvig-style edit-distance-1 candidate generator, intersected with the dictionary and ranked so
// that same-case-pattern and same-first-letter suggestions come first. Cheap: O(word length) edits.
QStringList Speller::suggest(const QString& word, int maxN) const {
    if (m_words.isEmpty() || word.isEmpty()) return {};
    const QString w = word.toLower();
    if (known(w)) return {};
    static const QString alpha = "abcdefghijklmnopqrstuvwxyz";
    QSet<QString> cand;
    for (int i=0; i<=w.size(); ++i){
        if (i<w.size()) cand.insert(w.left(i)+w.mid(i+1));                 // deletion
        if (i<w.size()-1) cand.insert(w.left(i)+w[i+1]+w[i]+w.mid(i+2));   // transposition
        for (QChar c: alpha){
            if (i<w.size()) cand.insert(w.left(i)+c+w.mid(i+1));           // replacement
            cand.insert(w.left(i)+c+w.mid(i));                            // insertion
        }
    }
    QStringList hits;
    for (const QString& c: cand) if (c!=w && m_words.contains(c)) hits << c;
    // Rank: shared prefix length first, then alphabetical (stable, deterministic).
    std::sort(hits.begin(), hits.end(), [&](const QString& a, const QString& b){
        auto pre=[&](const QString& s){ int n=0; while(n<s.size()&&n<w.size()&&s[n]==w[n]) ++n; return n; };
        int pa=pre(a), pb=pre(b); if(pa!=pb) return pa>pb; return a<b; });
    // Restore the original capitalisation pattern of the input onto each suggestion.
    const bool cap = !word.isEmpty() && word[0].isUpper();
    QStringList out;
    for (const QString& h: hits){ QString s=h; if(cap && !s.isEmpty()) s[0]=s[0].toUpper(); out<<s; if(out.size()>=maxN) break; }
    return out;
}

} // namespace xara
