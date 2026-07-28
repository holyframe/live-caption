#include "CaptionMerger.h"

#include <algorithm>
#include <cwctype>

#include "Util.h"

namespace {

std::vector<std::wstring> NormalizeAll(const std::vector<std::wstring>& words) {
    std::vector<std::wstring> out;
    out.reserve(words.size());
    for (const std::wstring& w : words) out.push_back(util::NormalizeWord(w));
    return out;
}

}  // namespace

void CaptionMerger::Reset() {
    m_words.clear();
    m_lines.clear();
    m_lineStart.clear();
    m_lastSnapshotWords = 0;
    m_lastSnapshotRaw.clear();
    m_prefixHintChars = 0;
    m_prefixHintWords = 0;
}

CaptionMerger::Update CaptionMerger::Ingest(std::wstring_view snapshot) {
    std::wstring cleaned = util::CollapseWhitespace(snapshot);
    if (cleaned.empty()) return {};

    // One scan answers both "did anything change?" and "where does the new text
    // diverge?", which the fast path needs anyway.
    const size_t rawPrefix = util::CommonPrefixLength(cleaned, m_lastSnapshotRaw);
    if (rawPrefix == cleaned.size() && rawPrefix == m_lastSnapshotRaw.size()) return {};

    Update fast;
    if (TryFastPath(cleaned, rawPrefix, fast)) return fast;

    // Slow path: the buffer scrolled, so fall back to a full overlap search.
    m_prefixHintChars = 0;
    m_prefixHintWords = 0;

    const std::vector<std::wstring> rawWords = util::SplitWords(cleaned);
    if (rawWords.empty()) return {};
    const std::vector<std::wstring> normWords = NormalizeAll(rawWords);

    m_lastSnapshotRaw = cleaned;

    // First snapshot of the session: take it verbatim.
    if (m_words.empty()) {
        m_words.reserve(rawWords.size());
        for (size_t i = 0; i < rawWords.size(); ++i) {
            m_words.push_back(Word{rawWords[i], normWords[i]});
        }
        m_lastSnapshotWords = rawWords.size();
        const size_t dirty = RebuildLinesFrom(0);
        return Update{true, dirty};
    }

    // Only the previous snapshot's words can still be revised, so the overlap
    // search is bounded by it rather than by the whole transcript. That keeps
    // the cost proportional to the caption window, not to session length.
    const size_t tail = std::min(m_lastSnapshotWords, m_words.size());
    const size_t maxOverlap = std::min(tail, rawWords.size());
    // The floor guards against a one- or two-word match on filler words, but it
    // must never exceed what is actually available: early in a session, or with
    // a very short caption buffer, matching the whole tail is the strong signal.
    const size_t minOverlap = std::max<size_t>(std::min(kMinOverlapWords, maxOverlap), 1);

    size_t overlap = 0;
    for (size_t k = maxOverlap; k >= minOverlap && k > 0; --k) {
        bool match = true;
        const size_t base = m_words.size() - k;
        for (size_t i = 0; i < k; ++i) {
            if (m_words[base + i].norm != normWords[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            overlap = k;
            break;
        }
    }

    // No overlap means the buffer scrolled further than one snapshot's worth
    // (or the source was swapped); append rather than risk dropping text.
    const size_t truncateAt = m_words.size() - overlap;

    // Most of the overlapping region is usually byte-for-byte identical: the
    // recogniser only rewrites its unstable tail. Skipping the unchanged prefix
    // keeps the steady-state cost proportional to the *new* text rather than to
    // the whole caption buffer, which matters a lot for Windows 11 Live
    // captions because it exposes its entire scrollback on every read.
    size_t common = 0;
    const size_t comparable = std::min(rawWords.size(), m_words.size() - truncateAt);
    while (common < comparable && m_words[truncateAt + common].text == rawWords[common]) {
        ++common;
    }

    const size_t changeAt = truncateAt + common;
    if (common == comparable && rawWords.size() == comparable) {
        // Identical after all; only the snapshot bookkeeping needs updating.
        m_lastSnapshotWords = rawWords.size();
        return {};
    }

    m_words.resize(changeAt);
    m_words.reserve(changeAt + (rawWords.size() - common));
    for (size_t i = common; i < rawWords.size(); ++i) {
        m_words.push_back(Word{rawWords[i], normWords[i]});
    }
    m_lastSnapshotWords = rawWords.size();

    const size_t dirty = RebuildLinesFrom(changeAt);
    return Update{true, dirty};
}

bool CaptionMerger::TryFastPath(std::wstring& cleaned, size_t rawPrefix, Update& out) {
    if (m_words.empty() || m_lastSnapshotWords == 0 || m_lastSnapshotWords > m_words.size()) {
        return false;
    }

    // Never trust a partially matched word; retreat to the preceding boundary.
    size_t prefix = rawPrefix;
    while (prefix > 0 && !util::IsSpace(cleaned[prefix - 1])) --prefix;
    if (prefix == 0) return false;

    // Count the words in the unchanged prefix, resuming from the previous
    // update's memo so the scan covers only newly arrived text. The memo stays
    // valid because those characters are, by definition, unchanged.
    size_t unchanged = 0;
    if (m_prefixHintChars > 0 && prefix >= m_prefixHintChars) {
        unchanged = m_prefixHintWords +
                    util::CountWords(std::wstring_view(cleaned).substr(
                        m_prefixHintChars, prefix - m_prefixHintChars));
    } else {
        unchanged = util::CountWords(std::wstring_view(cleaned).substr(0, prefix));
    }
    if (unchanged == 0 || unchanged > m_lastSnapshotWords) return false;

    // Alignment guard. The fast path pins the new snapshot to where the previous
    // one started, which only holds if the source did not scroll. A short
    // coincidental prefix match — say a repeated word such as "that's that's" —
    // must not be mistaken for alignment, or a word gets silently dropped, so
    // require the match to cover nearly all of the previous snapshot.
    const bool coversMost = unchanged * 5 >= m_lastSnapshotWords * 4;
    const bool divergesNearEnd = unchanged + kMaxRevisedTailWords >= m_lastSnapshotWords;
    if (!coversMost || !divergesNearEnd) return false;

    const std::vector<std::wstring> tailRaw =
        util::SplitWords(std::wstring_view(cleaned).substr(prefix));

    const size_t snapshotStart = m_words.size() - m_lastSnapshotWords;
    const size_t changeAt = snapshotStart + unchanged;

    m_words.resize(changeAt);
    m_words.reserve(changeAt + tailRaw.size());
    for (const std::wstring& word : tailRaw) {
        m_words.push_back(Word{word, util::NormalizeWord(word)});
    }

    m_lastSnapshotWords = unchanged + tailRaw.size();
    m_prefixHintChars = prefix;
    m_prefixHintWords = unchanged;
    m_lastSnapshotRaw = std::move(cleaned);

    out = Update{true, RebuildLinesFrom(changeAt)};
    return true;
}

size_t CaptionMerger::RebuildLinesFrom(size_t fromWord) {
    // Back up to the start of the line that owns `fromWord`; that whole line
    // has to be re-split.
    size_t line = 0;
    if (!m_lineStart.empty()) {
        const auto it = std::upper_bound(m_lineStart.begin(), m_lineStart.end(), fromWord);
        line = static_cast<size_t>(it - m_lineStart.begin());
        if (line > 0) --line;
    }

    const size_t startWord = m_lineStart.empty() ? 0 : m_lineStart[line];
    m_lines.resize(line);
    m_lineStart.resize(line);

    std::wstring current;
    size_t currentStart = startWord;
    for (size_t i = startWord; i < m_words.size(); ++i) {
        if (!current.empty()) current.push_back(L' ');
        current += m_words[i].text;
        if (util::IsSentenceEnd(m_words[i].text)) {
            m_lines.push_back(current);
            m_lineStart.push_back(currentStart);
            current.clear();
            currentStart = i + 1;
        }
    }
    if (!current.empty()) {
        m_lines.push_back(current);
        m_lineStart.push_back(currentStart);
    }
    return line;
}

size_t CaptionMerger::StableLineCount() const {
    if (m_lines.empty()) return 0;

    // Rule 1: a line that has scrolled out of the caption window cannot be
    // revised, because the source can no longer see it.
    const size_t volatileStart =
        m_words.size() > m_lastSnapshotWords ? m_words.size() - m_lastSnapshotWords : 0;

    size_t stableByScroll = 0;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        const size_t lineEnd = (i + 1 < m_lineStart.size()) ? m_lineStart[i + 1] : m_words.size();
        if (lineEnd <= volatileStart) {
            ++stableByScroll;
        } else {
            break;
        }
    }

    // Rule 2: recognisers only rewrite the sentence they are still working on
    // (occasionally the one before it), so anything older than the last few
    // sentences has settled even if the source keeps displaying it.
    //
    // Without this, Windows 11 Live captions — which exposes its whole
    // scrollback on every read — would never mark anything stable, and the
    // transcript would only reach disk on a clean exit.
    const size_t stableByAge =
        m_lines.size() > kVolatileTailLines ? m_lines.size() - kVolatileTailLines : 0;

    return std::max(stableByScroll, stableByAge);
}

std::wstring CaptionMerger::FullText() const {
    std::wstring out;
    size_t total = 0;
    for (const std::wstring& line : m_lines) total += line.size() + 2;
    out.reserve(total);
    for (const std::wstring& line : m_lines) {
        out += line;
        out += L"\r\n";
    }
    return out;
}
