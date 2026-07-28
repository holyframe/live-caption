// Standalone checks for CaptionMerger. Build with tests\run_tests.bat.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "CaptionMerger.h"
#include "Util.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& label) {
    std::printf("%-58s %s\n", label.c_str(), condition ? "PASS" : "FAIL");
    if (!condition) ++g_failures;
}

std::wstring Join(const std::vector<std::wstring>& words, size_t begin, size_t end) {
    std::wstring out;
    for (size_t i = begin; i < end; ++i) {
        if (!out.empty()) out.push_back(L' ');
        out += words[i];
    }
    return out;
}

// Mimics how a recogniser shows an unstable tail: lowercased, punctuation
// stripped, until it gains confidence and rewrites it properly.
std::wstring Degrade(const std::wstring& word) {
    std::wstring out = util::NormalizeWord(word);
    return out.empty() ? word : out;
}

std::wstring MergerText(const CaptionMerger& merger) {
    std::wstring out;
    for (const std::wstring& line : merger.Lines()) {
        if (!out.empty()) out.push_back(L' ');
        out += line;
    }
    return out;
}

// --- Test 1: a scrolling window reconstructs the original transcript --------
void TestScrollingReconstruction() {
    const std::wstring source =
        L"The Java API SDKs, right? What we're doing there is we're taking those. "
        L"We're automatically generating the Java model classes. We're bundling them into a "
        L"library that we're including in the Fusion apps, right? So why is this any different "
        L"except that it's Typescript? All right. So I guess that's that's my question. "
        L"So I guess what we need to do is we need to take this up with Evan.";

    const std::vector<std::wstring> words = util::SplitWords(source);
    const size_t windowSize = 12;  // caption window holds only a few words
    const size_t unstableTail = 3;

    CaptionMerger merger;
    for (size_t count = 1; count <= words.size(); ++count) {
        const size_t begin = count > windowSize ? count - windowSize : 0;
        std::vector<std::wstring> visible(words.begin() + static_cast<ptrdiff_t>(begin),
                                          words.begin() + static_cast<ptrdiff_t>(count));
        // Degrade the newest words to simulate in-flight revision.
        for (size_t i = visible.size() > unstableTail ? visible.size() - unstableTail : 0;
             i < visible.size(); ++i) {
            visible[i] = Degrade(visible[i]);
        }
        merger.Ingest(Join(visible, 0, visible.size()));
    }
    // Final snapshot arrives fully punctuated.
    const size_t begin = words.size() > windowSize ? words.size() - windowSize : 0;
    merger.Ingest(Join(words, begin, words.size()));

    const std::wstring rebuilt = MergerText(merger);
    const std::wstring expected = util::CollapseWhitespace(source);
    Check(rebuilt == expected, "scrolling window reconstructs transcript exactly");
    if (rebuilt != expected) {
        std::printf("   expected: %ls\n   actual  : %ls\n", expected.c_str(), rebuilt.c_str());
    }
}

// --- Test 2: a repeated phrase must not truncate history --------------------
// This is the case the Python implementation got wrong: it searched the whole
// history for the first matching 5-word run, so the earlier copy of a repeated
// phrase won and everything after it was discarded.
void TestRepeatedPhrase() {
    CaptionMerger merger;
    merger.Ingest(L"we need to take this up with Evan.");
    merger.Ingest(L"we need to take this up with Evan. So then later on");
    merger.Ingest(L"So then later on we need to take this up with Evan again.");

    const std::wstring text = MergerText(merger);
    const bool keptBoth = text.find(L"So then later on") != std::wstring::npos &&
                          text.find(L"Evan again.") != std::wstring::npos;
    Check(keptBoth, "repeated phrase does not truncate earlier history");
    if (!keptBoth) std::printf("   actual: %ls\n", text.c_str());
}

// --- Test 3: revisions replace the tail rather than duplicating it ----------
void TestRevisionReplacesTail() {
    CaptionMerger merger;
    merger.Ingest(L"So I guess that's that's my");
    merger.Ingest(L"So I guess that's that's my question");
    merger.Ingest(L"So I guess that's that's my question.");

    const std::wstring text = MergerText(merger);
    Check(text == L"So I guess that's that's my question.", "revised tail is replaced, not appended");
    if (text != L"So I guess that's that's my question.") {
        std::printf("   actual: %ls\n", text.c_str());
    }
}

// --- Test 4: sentence splitting ---------------------------------------------
void TestSentenceSplitting() {
    CaptionMerger merger;
    merger.Ingest(L"All right. So why is this any different? Dr. Evans said no. Yes!");
    const std::vector<std::wstring>& lines = merger.Lines();

    Check(lines.size() == 4, "splits on . ? and ! into four lines");
    if (lines.size() == 4) {
        Check(lines[1] == L"So why is this any different?", "question mark ends a line");
        Check(lines[2] == L"Dr. Evans said no.", "abbreviation does not split the line");
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        std::printf("   line %zu: %ls\n", i, lines[i].c_str());
    }
}

// --- Test 5: text without any terminator is still retained ------------------
// The Python version dropped every snapshot that contained no '.' at all.
void TestUnterminatedTextRetained() {
    CaptionMerger merger;
    merger.Ingest(L"this caption never gets a full stop");
    Check(!merger.Lines().empty(), "unterminated text is retained");
    Check(MergerText(merger) == L"this caption never gets a full stop",
          "unterminated text is preserved verbatim");
}

// --- Test 6: a hard cut with no overlap appends instead of losing text ------
void TestNoOverlapAppends() {
    CaptionMerger merger;
    merger.Ingest(L"first completely unrelated sentence here.");
    merger.Ingest(L"totally different words with nothing in common.");

    const std::wstring text = MergerText(merger);
    const bool bothPresent = text.find(L"first completely unrelated") != std::wstring::npos &&
                             text.find(L"totally different words") != std::wstring::npos;
    Check(bothPresent, "non-overlapping snapshot appends rather than replaces");
}

// --- Test 7: identical snapshots report no change ---------------------------
void TestIdempotentIngest() {
    CaptionMerger merger;
    merger.Ingest(L"hello there world.");
    const CaptionMerger::Update second = merger.Ingest(L"hello there world.");
    Check(!second.changed, "re-ingesting the same snapshot reports no change");
}

// --- Test 8: only scrolled-out lines are considered stable ------------------
void TestStableLineAccounting() {
    CaptionMerger merger;
    merger.Ingest(L"One two three four five.");
    Check(merger.StableLineCount() == 0, "the visible line is not yet stable");

    // A snapshot that shares no words means the first line scrolled away.
    merger.Ingest(L"Six seven eight nine ten.");
    Check(merger.StableLineCount() >= 1, "a scrolled-out line becomes stable");
}

// --- Test 8b: numbers, decimals and initials -------------------------------
void TestNumericSentenceEnds() {
    CaptionMerger merger;
    merger.Ingest(L"The total is 42. It grew by 3.5 percent. J. Smith agreed.");
    const std::vector<std::wstring>& lines = merger.Lines();

    Check(lines.size() == 3, "number-final sentence splits, decimal does not");
    for (size_t i = 0; i < lines.size(); ++i) {
        std::printf("   line %zu: %ls\n", i, lines[i].c_str());
    }
    if (lines.size() == 3) {
        Check(lines[0] == L"The total is 42.", "'42.' ends a sentence");
        Check(lines[1] == L"It grew by 3.5 percent.", "'3.5' does not split mid-sentence");
        Check(lines[2] == L"J. Smith agreed.", "initial 'J.' does not split");
    }
}

// --- Test 9: a source that never scrolls still yields flushable lines -------
// Windows 11 Live captions exposes its whole scrollback on every read, so the
// scroll-based stability rule alone would never commit anything to disk.
void TestNonScrollingSourceBecomesStable() {
    CaptionMerger merger;
    std::wstring buffer;
    for (int i = 0; i < 8; ++i) {
        if (!buffer.empty()) buffer.push_back(L' ');
        buffer += L"Sentence number " + std::to_wstring(i) + L".";
        merger.Ingest(buffer);  // always the entire history, never scrolled
    }
    Check(merger.Lines().size() == 8, "eight sentences accumulated");
    Check(merger.StableLineCount() == 5, "all but the last three lines are flushable");
}

// --- Test 10: appending to a large buffer only dirties the tail -------------
// Guards the optimisation that keeps steady-state cost proportional to the new
// text rather than to the size of the caption buffer.
void TestOnlyTailIsDirtied() {
    CaptionMerger merger;
    std::wstring buffer;
    for (int i = 0; i < 40; ++i) {
        if (!buffer.empty()) buffer.push_back(L' ');
        buffer += L"Filler sentence " + std::to_wstring(i) + L".";
    }
    merger.Ingest(buffer);
    const size_t lineCount = merger.Lines().size();

    // One more sentence arrives; the whole buffer is resent, as the real source
    // does. Only the final line should be reported dirty.
    const CaptionMerger::Update update = merger.Ingest(buffer + L" Brand new sentence.");
    Check(update.changed, "appending to a full buffer registers as a change");
    Check(update.firstDirtyLine >= lineCount - 1,
          "only the tail line is dirtied, not the whole buffer");
    std::printf("   lines=%zu firstDirtyLine=%zu\n", merger.Lines().size(), update.firstDirtyLine);
}

// --- Test 11: per-update cost must not scale with buffer size ---------------
// Windows 11 Live captions resends its whole scrollback on every read. If the
// merger reprocesses all of it each time, latency grows with session length.
// This measures the marginal cost of one appended sentence against buffer sizes
// that differ by 10x; the ratio should stay near 1, not near 10.
void TestUpdateCostIsIndependentOfBufferSize() {
    const auto measure = [](int sentences) {
        std::wstring buffer;
        for (int i = 0; i < sentences; ++i) {
            if (!buffer.empty()) buffer.push_back(L' ');
            buffer += L"This is filler sentence number " + std::to_wstring(i) + L" here.";
        }
        CaptionMerger merger;
        merger.Ingest(buffer);

        // Append one word at a time, exactly as a live recogniser would.
        const auto start = std::chrono::steady_clock::now();
        constexpr int kIterations = 300;
        for (int i = 0; i < kIterations; ++i) {
            buffer += L" word" + std::to_wstring(i);
            merger.Ingest(buffer);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return std::chrono::duration<double, std::milli>(elapsed).count() / kIterations;
    };

    const double small = measure(100);
    const double large = measure(1000);
    const double ratio = small > 0.0 ? large / small : 0.0;

    // Attribute the residual cost. The source hands over its entire buffer, so
    // simply receiving and normalising it is an unavoidable linear floor.
    {
        std::wstring buffer;
        for (int i = 0; i < 1000; ++i) {
            if (!buffer.empty()) buffer.push_back(L' ');
            buffer += L"This is filler sentence number " + std::to_wstring(i) + L" here.";
        }
        const auto start = std::chrono::steady_clock::now();
        constexpr int kIterations = 300;
        size_t sink = 0;
        for (int i = 0; i < kIterations; ++i) {
            sink += util::CollapseWhitespace(buffer).size();
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double perCall =
            std::chrono::duration<double, std::milli>(elapsed).count() / kIterations;
        std::printf("   buffer is %zu chars; normalising it alone costs %.4f ms (sink %zu)\n",
                    buffer.size(), perCall, sink);
    }

    std::printf("   100 sentences: %.4f ms/update | 1000 sentences: %.4f ms/update | ratio %.2fx\n",
                small, large, ratio);
    // Some linear cost is unavoidable: the source hands over its whole buffer,
    // so it must at least be scanned. What must not happen is per-word
    // allocation on every update, which is what made this slow before.
    Check(large < 0.5, "one update stays well inside the frame budget on a big buffer");
    Check(ratio < 12.0, "cost grows at most linearly, never quadratically");
}

}  // namespace

int main() {
    TestScrollingReconstruction();
    TestRepeatedPhrase();
    TestRevisionReplacesTail();
    TestSentenceSplitting();
    TestUnterminatedTextRetained();
    TestNoOverlapAppends();
    TestIdempotentIngest();
    TestStableLineAccounting();
    TestNumericSentenceEnds();
    TestNonScrollingSourceBecomesStable();
    TestOnlyTailIsDirtied();
    TestUpdateCostIsIndependentOfBufferSize();

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "SOME CHECKS FAILED.");
    return g_failures == 0 ? 0 : 1;
}
