// spell.h — lightweight dictionary spell-checker for the Proofing tool.
// Loads a system word list (/usr/share/dict/words, or a hunspell .dic) once and
// flags lowercase words not found. Gracefully degrades to "not ready" when no
// dictionary is installed (Proofing then falls back to its structural checks).
#pragma once
#include <QString>
#include <QStringList>
#include <QSet>

namespace xara {

class Speller {
public:
    static Speller& instance();
    bool ready() const { return !m_words.isEmpty(); }
    int  size()  const { return m_words.size(); }
    bool known(const QString& word) const;                       // case-insensitive (+ light plural/possessive + user dict)
    QStringList check(const QString& text, int maxReport = 40) const;   // misspelled words (deduped, in order)
    QStringList suggest(const QString& word, int maxN = 7) const; // correction candidates (edit-distance 1, ranked)
    // User dictionary: words the user has explicitly accepted (persisted via QSettings).
    void addUserWord(const QString& word);                       // add + persist; case-insensitive
    void removeUserWord(const QString& word);                    // forget a user-dictionary word + persist
    bool isUserWord(const QString& word) const { return m_userWords.contains(word.toLower()); }
    const QSet<QString>& userWords() const { return m_userWords; }

private:
    Speller();
    void loadUserWords();
    QSet<QString> m_words;
    QSet<QString> m_userWords;
};

} // namespace xara
