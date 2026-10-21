#pragma once
#include <QString>
#include <QStringList>
#include <QList>
#include <QRegularExpression>

// =============================================================================
//  numerals.h — V5.
//
//  Sequel numbering is written two ways and users type whichever they think
//  of. "gta 6" should find "Grand Theft Auto VI"; "Black Ops 2" should find
//  "Call of Duty: Black Ops II". Neither TMDB nor IGDB does that translation,
//  so the app does it on both ends:
//
//   * queryVariants() — expands a typed query into the forms worth asking
//     the API for, so a search for "6" also asks for "VI".
//
//   * numbersIn() / rejectsQueryNumber() — filters what comes back. Fuzzy
//     server-side matching returns neighbours: searching "Black Ops 2"
//     also returns "Black Ops III". A result whose number contradicts the
//     one that was typed is dropped.
//
//  Deliberately conservative about what counts as a roman numeral: only a
//  standalone whitespace-delimited token qualifies. Without that rule
//  "X-Men" would read as 10 and "I Am Legend" as 1, and titles would start
//  being filtered out on numbers nobody typed.
// =============================================================================
namespace Numerals {

inline QString toRoman(int n)
{
    if (n <= 0 || n > 3999) return {};
    static const int    kVal[] = {1000,900,500,400,100,90,50,40,10,9,5,4,1};
    static const char*  kSym[] = {"M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I"};
    QString out;
    for (int i = 0; i < 13; ++i)
        while (n >= kVal[i]) { out += kSym[i]; n -= kVal[i]; }
    return out;
}

// Strict: the whole token must be a well-formed roman numeral. Returns 0 if
// it isn't one, so callers can treat 0 as "not a number".
inline int fromRoman(const QString& tokenIn)
{
    QString t = tokenIn.toUpper();
    if (t.isEmpty()) return 0;
    static const QRegularExpression shape(
        "^M{0,3}(CM|CD|D?C{0,3})(XC|XL|L?X{0,3})(IX|IV|V?I{0,3})$");
    if (!shape.match(t).hasMatch()) return 0;

    auto value = [](QChar c) -> int {
        switch (c.unicode()) {
            case 'I': return 1;    case 'V': return 5;    case 'X': return 10;
            case 'L': return 50;   case 'C': return 100;  case 'D': return 500;
            case 'M': return 1000; default:  return 0;
        }
    };
    int total = 0;
    for (int i = 0; i < t.size(); ++i) {
        int v = value(t[i]);
        int next = (i + 1 < t.size()) ? value(t[i + 1]) : 0;
        total += (v < next) ? -v : v;
    }
    return total;
}

// Every sequel number a piece of text refers to, in either notation.
// Punctuation is trimmed from token edges ("Ops," / "II:") but tokens are
// never split on it, which is what keeps "X-Men" from reading as 10.
inline QList<int> numbersIn(const QString& text)
{
    QList<int> out;
    const QStringList tokens = text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (const QString& raw : tokens) {
        // An apostrophe before digits is an abbreviated year — "X-Men '97"
        // is not instalment 97. Checked on the RAW token, before the
        // punctuation trim below strips the apostrophe away.
        bool apostropheYear = raw.startsWith('\'') || raw.startsWith(QChar(0x2019));

        QString t = raw;
        while (!t.isEmpty() && !t.at(0).isLetterOrNumber())            t.remove(0, 1);
        while (!t.isEmpty() && !t.at(t.size() - 1).isLetterOrNumber()) t.chop(1);
        if (t.isEmpty()) continue;

        bool isNum = false;
        int arabic = t.toInt(&isNum);
        if (isNum) {
            // Four digits standing alone is a year ("Blade Runner 2049"),
            // not an instalment number.
            if (!apostropheYear && arabic > 0 && arabic < 1000 && !out.contains(arabic))
                out.append(arabic);
            continue;
        }
        int roman = fromRoman(t);
        if (roman > 0 && !out.contains(roman)) out.append(roman);
    }
    return out;
}

// The query forms worth sending upstream. Always includes the original; adds
// the opposite notation when the query names a number, so "gta 6" also asks
// for "gta VI" and "black ops II" also asks for "black ops 2".
inline QStringList queryVariants(const QString& query)
{
    QStringList variants{query};
    const QStringList tokens = query.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);

    for (int i = 0; i < tokens.size(); ++i) {
        QString t = tokens[i];
        bool isNum = false;
        int arabic = t.toInt(&isNum);
        QString swapped;
        if (isNum && arabic > 0 && arabic < 1000) {
            swapped = toRoman(arabic);
        } else {
            int roman = fromRoman(t);
            if (roman > 0) swapped = QString::number(roman);
        }
        if (swapped.isEmpty()) continue;

        QStringList copy = tokens;
        copy[i] = swapped;
        QString variant = copy.join(' ');
        if (!variants.contains(variant, Qt::CaseInsensitive)) variants << variant;
    }
    return variants;
}

// Should this result be discarded for contradicting the number that was
// typed? Only ever true when the query named a number AND the title names a
// different one — a title with no number at all is still a fair match
// ("black ops 2" keeps "Call of Duty: Black Ops", drops "Black Ops III").
inline bool rejectsQueryNumber(const QString& title, const QString& query)
{
    const QList<int> wanted = numbersIn(query);
    if (wanted.isEmpty()) return false;
    const QList<int> got = numbersIn(title);
    if (got.isEmpty()) return false;
    for (int n : wanted)
        if (got.contains(n)) return false;
    return true;
}

}   // namespace Numerals
