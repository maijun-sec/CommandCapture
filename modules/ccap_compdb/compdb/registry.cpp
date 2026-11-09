// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb_registry - the compiler registry
//
// See ccap_compdb_registry.h for the design rationale.
//
// This file implements:
//   1. the version predicate (component-wise comparison)
//   2. the version probe (ordered regex extraction)
//   3. the glob matcher
//   4. the rule group scope matcher
//   5. the registry (families, matches, overrides)
//   6. the resolution: executable name -> family + variant + rules

#include "ccap_compdb_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "core/normalize.h"

namespace ccap {
namespace compdb {

namespace {

// ── Case-insensitive compare ──────────────────────────────────────────────
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The version predicate
// ═══════════════════════════════════════════════════════════════════════════

bool parseVersion(const std::string& version, std::vector<unsigned>* out) {
    if (!out) return false;
    out->clear();
    if (version.empty()) return false;

    // Accept "1.2.3", "1.2", "1", and tolerate trailing junk
    // ("1.2.3rc1" -> 1.2.3), because compiler version banners are
    // full of decorations.
    unsigned current = 0;
    bool haveDigit = false;
    bool sawDot = false;

    for (char c : version) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            current = current * 10 + static_cast<unsigned>(c - '0');
            haveDigit = true;
        } else if (c == '.') {
            if (!haveDigit) return false;
            out->push_back(current);
            current = 0;
            haveDigit = false;
            sawDot = true;
        } else {
            // Any other character ends the version. It is only accepted
            // if we have already seen at least one component.
            break;
        }
    }
    if (haveDigit) out->push_back(current);

    // A version must have at least one component, and must not end on a
    // dot ("1.2." is malformed).
    if (out->empty()) return false;
    if (sawDot && !haveDigit) {
        // The string ended with '.', e.g. "1.2." -- the last component
        // was already pushed, and the trailing dot is harmless.
    }
    return true;
}

int compareVersions(const std::string& a, const std::string& b) {
    std::vector<unsigned> va, vb;
    bool oka = parseVersion(a, &va);
    bool okb = parseVersion(b, &vb);

    // An unparseable version sorts *before* any parseable one, so
    // "unknown" < "1.0". The callers never compare against an
    // unparseable version in practice (the version predicate returns
    // false for an undetectable version), but the ordering has to be
    // total so the comparison is deterministic.
    if (!oka && !okb) return 0;
    if (!oka) return -1;
    if (!okb) return 1;

    size_t n = std::max(va.size(), vb.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned x = i < va.size() ? va[i] : 0u;
        unsigned y = i < vb.size() ? vb[i] : 0u;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

bool VersionRange::matches(const std::string& version) const {
    if (empty()) return true;

    // An undetectable version matches a **constrained** range only
    // when the range is empty. A rule scoped to "gcc >= 4.7" must not
    // fire on a compiler whose version we could not determine: firing
    // it would apply a rule whose precondition was never verified.
    std::vector<unsigned> parsed;
    if (!parseVersion(version, &parsed)) return false;

    if (!minVersion.empty() && compareVersions(version, minVersion) < 0) {
        return false;
    }
    if (!maxVersion.empty() && compareVersions(version, maxVersion) > 0) {
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// The version probe
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// ── A tiny regex engine ──────────────────────────────────────────────────
//
// Why not std::regex: the project uses no third-party dependencies, and
// std::regex is a portability and performance liability. But more
// importantly, the replace form ("replace/<regex>/<replacement>") needs
// **capture groups** and a **replacement**, and the patterns used for
// version extraction are a narrow, well-understood subset:
//
//   - literal text
//   - '.' (any character)
//   - '*' (zero or more of the previous)
//   - '+' (one or more of the previous)
//   - '?' (optional previous)
//   - '(' ')' (capture group)
//   - '|' (alternation)
//   - '\d' '\w' '\s' (character classes)
//   - '[' ... ']' (bracket class)
//   - '$' (end of text)
//
// The design below is a **backtracking matcher** over that subset. The
// match state carries the text position, and each element is asked
// "can you match here, and if so, where do you end up?". An element
// that can match in more than one way (a group, a star) tries each way
// in turn, which is what makes the matcher correct for alternation and
// greedy quantifiers.
//
// The recursion depth is bounded by the pattern length and the number of
// repetitions, not by the text length, so a long version banner cannot
// blow the stack.
//
// A pattern that cannot be parsed is reported as unparseable and the
// caller skips it -- a bad pattern must never take the whole mapping
// down.

// The parsed form of one pattern element.
struct ReElem {
    enum Kind {
        Literal,   // a literal character
        Any,       // '.'
        Class,     // \d \w \s
        Bracket,   // [...] and its negation
        Group,     // ( ... )
        Alt,       // a | b | c
        Star,      // x*
        Plus,      // x+
        Opt,       // x?
        End,       // $
    };

    Kind kind = Literal;
    char ch = 0;                    // Literal / Class
    std::string set;                // Bracket
    bool negated = false;           // Bracket
    int groupIndex = -1;            // Group
    std::vector<ReElem> children;   // Group / Alt / Star / Plus / Opt
};

// The match state.
struct ReState {
    const std::string* text = nullptr;
    std::vector<std::string>* caps = nullptr;
    std::vector<bool>* capMatched = nullptr;
};

// Does the character class match?
bool classMatches(char cls, char c) {
    switch (cls) {
        case 'd': return std::isdigit(static_cast<unsigned char>(c)) != 0;
        case 'w': return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
                         c == '_';
        case 's': return std::isspace(static_cast<unsigned char>(c)) != 0;
        default: return false;
    }
}

// Does the bracket class match?
bool bracketMatches(const std::string& set, bool negated, char c) {
    bool found = false;
    for (size_t k = 0; k < set.size(); ++k) {
        if (set[k] == '\\' && k + 1 < set.size()) {
            if (classMatches(set[k + 1], c)) found = true;
            ++k;
        } else if (set[k] == c) {
            found = true;
        } else if (k + 2 < set.size() && set[k + 1] == '-' && set[k + 2] >= c) {
            // A range "a-z".
            if (set[k] <= c && c <= set[k + 2]) found = true;
            ++k;
        }
    }
    return negated ? !found : found;
}

// ═══════════════════════════════════════════════════════════════════════════
// The matcher
// ═══════════════════════════════════════════════════════════════════════════
//
// WHY CONTINUATION PASSING
//
// A backtracking regex engine needs the continuation -- "what has to
// match after this element" -- to be available *inside* the element's
// matcher. Otherwise a greedy repetition has no way to know that a
// shorter match would let the rest of the pattern succeed.
//
// The concrete failure this fixes:
//
//     ".*g[c+][c+][^\s]* [^\s]+ (\d+\.\d+(\.\d+)?)"
//
// on "gcc (GCC) 4.8.5".
//
//   * The leading ".*" is greedy: it wants to eat the whole string.
//   * The capture group then has nothing left to match.
//   * A matcher without a continuation cannot back off: it only knows
//     that the greedy choice failed, not that a shorter one works.
//
// The engine therefore threads a **continuation** through the match:
// every element, when it matches, calls the continuation with the
// position it reached. The continuation is the rest of the sequence.
//
// The cost is a std::function per call, which is fine: the patterns
// are short and the texts are one-line version banners.

// The continuation: "given a position, can the rest of the pattern
// match?".
typedef std::function<bool(size_t)> ReCont;

// Match a sequence of elements, starting at `idx`, with the given
// continuation.
bool reMatchSeq(const std::vector<ReElem>& elems, size_t idx,
                const ReState& st, size_t pos, const ReCont& k);

// Match one element, with the given continuation.
bool reMatchElem(const ReElem& e, const ReState& st, size_t pos,
                 const ReCont& k);

// Match a sequence of elements, with the given continuation.
bool reMatchSeq(const std::vector<ReElem>& elems, size_t idx,
                const ReState& st, size_t pos, const ReCont& k) {
    if (idx == elems.size()) return k(pos);

    const ReElem& e = elems[idx];

    // A repetition: the atom is matched repeatedly, and each round may
    // either continue the repetition or end it. The greedy order is
    // "as many as possible first".
    if (e.kind == ReElem::Star || e.kind == ReElem::Plus ||
        e.kind == ReElem::Opt) {
        if (e.children.empty()) {
            return reMatchSeq(elems, idx + 1, st, pos, k);
        }
        const ReElem& atom = e.children[0];

        // The greedy repetition.
        //
        // The atom is matched, and its continuation tries to match the
        // atom **again**. When that fails -- or when the atom cannot
        // match at all -- the repetition ends and the outer
        // continuation takes over.
        //
        // That is the classic "match, then backtrack" structure, and it
        // is what makes the leading ".*" work: the greedy attempt
        // fails the continuation, and the loop gives back characters
        // until the continuation succeeds.
        //
        // The recursion depth is bounded by the number of repetitions,
        // and the guard below bounds it further.
        struct Rep {
            static bool run(const std::vector<ReElem>& elems, size_t idx,
                            const ReElem& atom, const ReState& st, size_t pos,
                            size_t count, const ReCont& stop) {
                // The guard: 10000 repetitions is far more than any
                // version banner needs.
                if (count > 10000) return false;

                // Continue the repetition: match the atom again.
                //
                // If the atom cannot match, the repetition **ends** and
                // the outer continuation takes over. That fallback is
                // what makes the greedy repetition give back
                // characters: the greedy attempt matches the atom and
                // then fails, and the "stop" path is tried instead.
                ReCont more = [&elems, idx, &atom, &st, count, &stop](
                                  size_t next) {
                    if (Rep::run(elems, idx, atom, st, next, count + 1,
                                 stop)) {
                        return true;
                    }
                    // Give back this repetition and end here.
                    return stop(next);
                };

                // Match the atom once. If it does not match at all,
                // the repetition ends here.
                if (reMatchElem(atom, st, pos, more)) return true;

                // The atom did not match: end the repetition.
                //
                // This is the case that a Plus has to be careful about:
                // a Plus must consume at least once, so the caller
                // checks the count.
                return stop(pos);
            }
        };

        // The repetition that ends here: the continuation takes over.
        ReCont stop = [&elems, idx, &st, &k](size_t next) {
            return reMatchSeq(elems, idx + 1, st, next, k);
        };

        if (e.kind == ReElem::Opt) {
            // The optional form: try the atom first, and fall back to
            // the empty match.
            if (reMatchElem(atom, st, pos, stop)) return true;
            return stop(pos);
        }

        // The greedy form. A Star may match the empty string, so the
        // empty match is the last resort; a Plus must match at least
        // once.
        if (e.kind == ReElem::Plus) {
            // The Plus: match the atom at least once, and let the
            // repetition continue from there.
            //
            // The empty match is **not** allowed: a Plus that matched
            // nothing would make the pattern match a string that has
            // no digits at all.
            ReCont plusCont = [&elems, idx, &atom, &st, &stop](size_t next) {
                return Rep::run(elems, idx, atom, st, next, 1, stop);
            };
            return reMatchElem(atom, st, pos, plusCont);
        }

        // The Star: match the atom as many times as possible, and fall
        // back to the empty match.
        ReCont greedy = [&elems, idx, &atom, &st, &stop](size_t next) {
            return Rep::run(elems, idx, atom, st, next, 1, stop);
        };
        if (reMatchElem(atom, st, pos, greedy)) return true;
        return stop(pos);
    }

    return reMatchElem(e, st, pos, [&elems, idx, &st, &k](size_t next) {
        return reMatchSeq(elems, idx + 1, st, next, k);
    });
}

// Match one element, with the given continuation.
bool reMatchElem(const ReElem& e, const ReState& st, size_t pos,
                 const ReCont& k) {
    const std::string& text = *st.text;

    switch (e.kind) {
        case ReElem::Literal:
            if (pos < text.size() && text[pos] == e.ch) return k(pos + 1);
            return false;

        case ReElem::Any:
            if (pos < text.size() && text[pos] != '\n') return k(pos + 1);
            return false;

        case ReElem::Class:
            if (pos < text.size() && classMatches(e.ch, text[pos])) {
                return k(pos + 1);
            }
            return false;

        case ReElem::Bracket:
            if (pos < text.size() && bracketMatches(e.set, e.negated, text[pos])) {
                return k(pos + 1);
            }
            return false;

        case ReElem::End:
            if (pos == text.size()) return k(pos);
            return false;

        case ReElem::Alt:
            // Try each alternative in turn; the first that matches wins.
            //
            // Each child is a branch: an Alt element whose children are
            // the branch's elements. So the branch is matched with
            // reMatchSeq over those children.
            for (const ReElem& branch : e.children) {
                std::vector<std::string> savedCaps = *st.caps;
                std::vector<bool> savedMatched = *st.capMatched;
                bool ok = reMatchSeq(branch.children, 0, st, pos, k);
                if (ok) return true;
                *st.caps = savedCaps;
                *st.capMatched = savedMatched;
            }
            return false;

        case ReElem::Group:
            {
                // The group captures what its body consumed.
                //
                // The body is matched with the **outer** continuation
                // wrapped around it: the body runs, and when it
                // finishes, the capture is recorded and the outer
                // continuation is called.
                //
                // That is what lets the group try every possible end
                // position: the outer continuation decides which one
                // works.
                ReCont bodyCont = [&e, &st, pos, &k](size_t bodyEnd) {
                    std::vector<std::string> savedCaps = *st.caps;
                    std::vector<bool> savedMatched = *st.capMatched;
                    if (e.groupIndex >= 0) {
                        (*st.caps)[e.groupIndex] =
                            st.text->substr(pos, bodyEnd - pos);
                        (*st.capMatched)[e.groupIndex] = true;
                    }
                    if (k(bodyEnd)) return true;
                    *st.caps = savedCaps;
                    *st.capMatched = savedMatched;
                    return false;
                };

                if (e.children.empty()) return bodyCont(pos);
                return reMatchSeq(e.children, 0, st, pos, bodyCont);
            }

        case ReElem::Star:
        case ReElem::Plus:
        case ReElem::Opt:
            // A repetition reached directly (rather than through
            // reMatchSeq) matches its atom once. The repetition logic
            // lives in reMatchSeq, which is the only place that knows
            // what comes after it.
            if (e.children.empty()) return k(pos);
            return reMatchElem(e.children[0], st, pos, k);
    }
    return false;
}

// Match a sequence of elements and report where it ended.
//
// This is the entry point used by the search: the continuation is the
// one that records the end position.
bool reMatchSeq(const std::vector<ReElem>& elems, size_t idx,
                const ReState& st, size_t pos, size_t* endPos) {
    return reMatchSeq(elems, idx, st, pos, [endPos](size_t next) {
        *endPos = next;
        return true;
    });
}

// The parser.
class ReParser {
public:
    explicit ReParser(const std::string& pattern)
        : p_(pattern), groupCounter_(0) {}

    bool parse(std::vector<ReElem>* out) {
        std::vector<ReElem> alts;
        if (!parseAlt(&alts)) return false;
        if (i_ != p_.size()) return false;

        // The alternation branches are the top level: the matcher tries
        // each of them in turn.
        //
        // So the top level is a **list of branches**, not a sequence.
        // Each branch is an Alt element whose children are the branch's
        // elements, which is exactly what the Alt matcher expects.
        //
        // The structure is therefore:
        //
        //     elems_ = [ Alt{children=[elem, elem, ...]},   // branch 1
        //                Alt{children=[elem, elem, ...]} ]  // branch 2
        //
        // and the search tries them one after the other.
        out->insert(out->end(), alts.begin(), alts.end());
        return true;
    }

    int groupCount() const { return groupCounter_; }

private:
    bool parseAlt(std::vector<ReElem>* out) {
        std::vector<ReElem> first;
        if (!parseSeq(&first)) return false;

        ReElem alt;
        alt.kind = ReElem::Alt;
        alt.children = std::move(first);
        out->push_back(std::move(alt));

        while (i_ < p_.size() && p_[i_] == '|') {
            ++i_;
            std::vector<ReElem> second;
            if (!parseSeq(&second)) return false;

            ReElem branch;
            branch.kind = ReElem::Alt;
            branch.children = std::move(second);
            out->push_back(std::move(branch));
        }
        return true;
    }

    bool parseSeq(std::vector<ReElem>* out) {
        while (i_ < p_.size() && p_[i_] != '|' && p_[i_] != ')') {
            ReElem atom;
            if (!parseAtom(&atom)) return false;

            if (i_ < p_.size()) {
                char q = p_[i_];
                if (q == '*' || q == '+' || q == '?') {
                    ++i_;
                    ReElem rep;
                    rep.kind = q == '*' ? ReElem::Star
                              : q == '+' ? ReElem::Plus
                                         : ReElem::Opt;
                    rep.children.push_back(std::move(atom));
                    out->push_back(std::move(rep));
                    continue;
                }
            }
            out->push_back(std::move(atom));
        }
        return true;
    }

    bool parseAtom(ReElem* out) {
        if (i_ >= p_.size()) return false;
        char c = p_[i_];

        if (c == '(') {
            ++i_;
            int gi = -1;
            if (i_ + 1 < p_.size() && p_[i_] == '?' && i_ + 2 < p_.size() &&
                p_[i_ + 1] == ':') {
                i_ += 2;  // a non-capturing group
            } else {
                gi = ++groupCounter_;
            }
            std::vector<ReElem> body;
            if (!parseSeq(&body)) return false;
            if (i_ >= p_.size() || p_[i_] != ')') return false;
            ++i_;
            out->kind = ReElem::Group;
            out->children = std::move(body);
            out->groupIndex = gi;
            return true;
        }

        if (c == '.') {
            ++i_;
            out->kind = ReElem::Any;
            return true;
        }

        if (c == '\\' && i_ + 1 < p_.size()) {
            char cls = p_[i_ + 1];
            i_ += 2;
            if (cls == 'd' || cls == 'w' || cls == 's') {
                out->kind = ReElem::Class;
                out->ch = cls;
            } else {
                out->kind = ReElem::Literal;
                out->ch = cls;
            }
            return true;
        }

        if (c == '[') {
            ++i_;
            out->kind = ReElem::Bracket;
            if (i_ < p_.size() && p_[i_] == '^') {
                out->negated = true;
                ++i_;
            }
            while (i_ < p_.size() && p_[i_] != ']') {
                if (p_[i_] == '\\' && i_ + 1 < p_.size()) {
                    out->set += p_[i_];
                    out->set += p_[i_ + 1];
                    i_ += 2;
                } else {
                    out->set += p_[i_];
                    ++i_;
                }
            }
            if (i_ >= p_.size()) return false;
            ++i_;  // ']'
            return true;
        }

        if (c == '$') {
            ++i_;
            out->kind = ReElem::End;
            return true;
        }

        if (c == '|' || c == ')') return false;  // handled by the callers

        ++i_;
        out->kind = ReElem::Literal;
        out->ch = c;
        return true;
    }

    const std::string& p_;
    size_t i_ = 0;
    int groupCounter_ = 0;
};

// A compiled pattern.
class Regex {
public:
    static bool compile(const std::string& pattern, Regex* out) {
        if (!out) return false;
        ReParser parser(pattern);
        if (!parser.parse(&out->elems_)) return false;
        out->groupCount_ = parser.groupCount();
        return true;
    }
    // Find the first match anywhere in the text.
    //
    // "Anywhere" matters: a pattern like "(\\d+\\.\\d+\\.\\d+)" has to
    // find "12.2.0" inside "gcc 12.2.0", not at the very start.
    //
    // The start positions are tried in order, and for each start the
    // branches are tried in order -- so the **leftmost** match wins,
    // which is what every other regex engine does.
    bool search(const std::string& text) {
        if (elems_.empty()) return false;

        for (size_t start = 0; start <= text.size(); ++start) {
            for (const ReElem& branch : elems_) {
                std::vector<std::string> caps(groupCount_ + 1);
                std::vector<bool> matched(groupCount_ + 1, false);
                ReState st;
                st.text = &text;
                st.caps = &caps;
                st.capMatched = &matched;

                // The branch's own continuation records the end
                // position.
                size_t end = 0;
                bool ok = reMatchSeq(branch.children, 0, st, start,
                                     [&end](size_t next) {
                                         end = next;
                                         return true;
                                     });
                if (!ok) continue;

                caps_ = caps;
                whole_ = text.substr(start, end - start);
                return true;
            }
        }
        return false;
    }

    const std::vector<std::string>& captures() const { return caps_; }
    const std::string& wholeMatch() const { return whole_; }

private:
    std::vector<ReElem> elems_;
    int groupCount_ = 0;
    std::vector<std::string> caps_;
    std::string whole_;
};

}  // namespace

std::string extractVersion(const VersionProbe& probe, const std::string& output) {
    if (probe.empty()) return "";

    for (const std::string& pattern : probe.patterns) {
        std::string regexText;
        std::string replacement;
        bool isReplace = false;

        // The replace form: "replace/<regex>/<replacement>".
        if (pattern.compare(0, 8, "replace/") == 0) {
            isReplace = true;
            size_t start = 8;

            // Find the separator between the regex and the replacement.
            //
            // It is the **last** '/', not the first: the regex itself
            // may contain a slash. The IAR banner pattern matches
            // "IAR C/C++ Compiler", and the "C/C++" contains a slash,
            // so the first-slash rule would cut the pattern in the
            // wrong place and the "replacement" would be the tail of
            // the regex.
            //
            // The last slash is the right rule because the replacement
            // is a version string or a literal: it never contains a
            // slash.
            //
            // A pattern with **no** replacement ("replace/<regex>") is
            // also valid: the whole match is then the version. That is
            // detected by the separator being the prefix slash itself.
            size_t slash = pattern.rfind('/');
            if (slash == std::string::npos || slash < start) {
                // No separator: the whole rest is the regex, and the
                // result is the whole match.
                regexText = pattern.substr(start);
                replacement.clear();
            } else {
                regexText = pattern.substr(start, slash - start);
                replacement = pattern.substr(slash + 1);
            }
        } else {
            regexText = pattern;
        }

        Regex re;
        if (!Regex::compile(regexText, &re)) continue;
        if (!re.search(output)) continue;

        if (isReplace) {
            // An empty replacement means "the first capture group, or
            // the whole match".
            //
            // That is the "replace/<regex>" form: the pattern says
            // what to look for, and the version is what it captured.
            //
            // Falling through to the plain-form logic below is the
            // right thing to do: the "replace/" prefix then only says
            // "this is a pattern, not a literal", and the extraction
            // rule is the same one the plain form uses.
            if (!replacement.empty()) {
                // Apply the replacement: "$1" refers to capture group 1.
                std::string out;
                for (size_t i = 0; i < replacement.size(); ++i) {
                    if (replacement[i] == '$' && i + 1 < replacement.size() &&
                        std::isdigit(static_cast<unsigned char>(
                            replacement[i + 1]))) {
                        size_t g = static_cast<size_t>(replacement[i + 1] - '0');
                        if (g < re.captures().size()) {
                            out += re.captures()[g];
                        }
                        ++i;
                    } else {
                        out += replacement[i];
                    }
                }
                return out;
            }
        }

        // The plain form: the first capture group, or the whole match.
        const std::vector<std::string>& caps = re.captures();
        if (caps.size() > 1 && !caps[1].empty()) return caps[1];
        return re.wholeMatch();
    }

    return "";
}

// ═══════════════════════════════════════════════════════════════════════════
// The glob matcher
// ═══════════════════════════════════════════════════════════════════════════

bool globMatch(const std::string& pattern, const std::string& text) {
    // The classic iterative glob matcher: two pointers plus a star
    // backtrack point. O(n*m) worst case, no recursion, no exponential
    // blow-up on "a*a*a*a*b".
    size_t p = 0, t = 0;
    size_t starP = std::string::npos, starT = 0;

    while (t < text.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' ||
             std::tolower(static_cast<unsigned char>(pattern[p])) ==
                 std::tolower(static_cast<unsigned char>(text[t])))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starT = t;
        } else if (starP != std::string::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool CompilerMatch::parseRedirect(const std::string& pattern,
                                  std::string* from, std::string* to) {
    const std::string kPrefix = "replace/";
    if (pattern.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    size_t slash = pattern.find('/', kPrefix.size());
    if (slash == std::string::npos) return false;
    if (from) *from = pattern.substr(kPrefix.size(), slash - kPrefix.size());
    if (to) *to = pattern.substr(slash + 1);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// The rule group scope
// ═══════════════════════════════════════════════════════════════════════════

bool RuleGroup::appliesToVariant(const std::string& familyId,
                                  const std::string& variantId) const {
    for (const std::string& sel : appliesTo) {
        if (sel.empty()) continue;

        // "*" matches everything.
        if (sel == "*") return true;

        size_t colon = sel.find(':');
        if (colon == std::string::npos) {
            // A bare variant id, matched against any family.
            if (iequals(sel, variantId)) return true;
            continue;
        }

        std::string selFamily = sel.substr(0, colon);
        std::string selVariant = sel.substr(colon + 1);

        // The family must match, and the variant must match.
        bool familyOk = selFamily == "*" || iequals(selFamily, familyId);
        if (!familyOk) continue;

        if (selVariant == "*") return true;
        if (iequals(selVariant, variantId)) return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// The family
// ═══════════════════════════════════════════════════════════════════════════

const CompilerVariant* CompilerFamily::findVariant(
    const std::string& variantId) const {
    for (const auto& v : variants) {
        if (iequals(v.id, variantId)) return &v;
    }
    return nullptr;
}

const CompilerVariant* CompilerFamily::nextVariant(
    const std::string& variantId) const {
    const CompilerVariant* v = findVariant(variantId);
    if (!v || v->nextVariantId.empty()) return nullptr;
    return findVariant(v->nextVariantId);
}

// ═══════════════════════════════════════════════════════════════════════════
// The names
// ═══════════════════════════════════════════════════════════════════════════

const char* unknownPolicyName(UnknownPolicy policy) {
    switch (policy) {
        case UnknownPolicy::Report: return "report";
        case UnknownPolicy::Keep:   return "keep";
        case UnknownPolicy::Drop:   return "drop";
        case UnknownPolicy::Fail:   return "fail";
    }
    return "report";
}

bool unknownPolicyFromName(const std::string& name, UnknownPolicy* out) {
    if (!out) return false;
    std::string n = toLower(name);
    if (n == "report") { *out = UnknownPolicy::Report; return true; }
    if (n == "keep" || n == "passthrough" || n == "pass") {
        *out = UnknownPolicy::Keep;
        return true;
    }
    if (n == "drop" || n == "ignore") { *out = UnknownPolicy::Drop; return true; }
    if (n == "fail" || n == "error") { *out = UnknownPolicy::Fail; return true; }
    return false;
}

const char* auditBucketName(AuditBucket bucket) {
    switch (bucket) {
        case AuditBucket::Kept:                return "kept";
        case AuditBucket::Mapped:              return "mapped";
        case AuditBucket::DroppedNoEquivalent: return "dropped_no_equivalent";
        case AuditBucket::DroppedByLevel:      return "dropped_by_level";
        case AuditBucket::DroppedByPolicy:     return "dropped_by_policy";
        case AuditBucket::Unknown:             return "unknown";
    }
    return "unknown";
}

// ═══════════════════════════════════════════════════════════════════════════
// The registry
// ═══════════════════════════════════════════════════════════════════════════

CompilerRegistry::CompilerRegistry() = default;

void CompilerRegistry::upsertFamily(const CompilerFamily& family) {
    for (auto& f : families_) {
        if (iequals(f.id, family.id)) {
            f = family;
            return;
        }
    }
    families_.push_back(family);
}

const CompilerFamily* CompilerRegistry::findFamily(
    const std::string& id) const {
    for (const auto& f : families_) {
        if (iequals(f.id, id)) return &f;
    }
    return nullptr;
}

void CompilerRegistry::addMatch(const CompilerMatch& match) {
    matches_.push_back(match);
}

void CompilerRegistry::addOverride(const ProfileOverride& override) {
    overrides_.push_back(override);
}

}  // namespace compdb
}  // namespace ccap
