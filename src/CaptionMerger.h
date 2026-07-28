#pragma once

#include <string>
#include <string_view>
#include <vector>

// Reassembles a continuous transcript from the overlapping snapshots that a
// live-caption window produces.
//
// The caption window is a scrolling buffer: each poll returns mostly the same
// text as the previous poll, shifted left as old words scroll off and new ones
// arrive at the end. The recogniser also *revises* the unstable tail, changing
// casing and punctuation once it becomes more confident.
//
// The merge therefore looks for the longest suffix of the previous snapshot
// that matches a prefix of the new one, and lets the new snapshot overwrite
// that region. Matching is done on normalised words so revisions still align,
// and the newer wording always wins.
class CaptionMerger {
public:
    struct Update {
        bool   changed = false;
        // Lines at index >= firstDirtyLine were replaced wholesale.
        size_t firstDirtyLine = 0;
    };

    // Feeds one raw snapshot of the caption window.
    Update Ingest(std::wstring_view snapshot);

    const std::vector<std::wstring>& Lines() const { return m_lines; }

    // Number of leading lines that can no longer be revised, because they have
    // already scrolled out of the caption window's visible buffer. Only these
    // are safe to commit to disk.
    size_t StableLineCount() const;

    std::wstring FullText() const;

    void Reset();

private:
    struct Word {
        std::wstring text;
        std::wstring norm;
    };

    // Rebuilds m_lines from the line containing `fromWord` onwards.
    size_t RebuildLinesFrom(size_t fromWord);

    // Handles the common case where the source merely extended or revised the
    // tail of the text it gave us last time. Returns false if the snapshot is
    // not provably aligned with the previous one, leaving the caller to run the
    // full overlap search.
    bool TryFastPath(std::wstring& cleaned, size_t rawPrefix, Update& out);

    // Smallest overlap we trust. A one- or two-word match on filler words like
    // "the" would otherwise truncate good history.
    static constexpr size_t kMinOverlapWords = 3;

    // How many trailing sentences are treated as still-revisable.
    static constexpr size_t kVolatileTailLines = 3;

    // Largest tail the fast path expects a source to rewrite in one step.
    static constexpr size_t kMaxRevisedTailWords = 32;

    std::vector<Word>         m_words;         // every word of the transcript
    std::vector<std::wstring> m_lines;         // m_words grouped into sentences
    std::vector<size_t>       m_lineStart;     // first word index of each line
    size_t                    m_lastSnapshotWords = 0;
    std::wstring              m_lastSnapshotRaw;

    // Memo of "this character offset into m_lastSnapshotRaw holds this many
    // words", so counting the unchanged prefix costs only the newly added text
    // instead of rescanning the whole buffer every update.
    size_t m_prefixHintChars = 0;
    size_t m_prefixHintWords = 0;
};
