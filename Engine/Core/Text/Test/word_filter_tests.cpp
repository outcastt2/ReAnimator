#include "Engine/Core/Text/word_filter.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, std::string_view what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
}

void clean(std::string_view text) { expect(!dingosdk::text::contains_bad_words(text), std::string("clean: ") + std::string(text)); }
void masks(std::string_view text, std::string_view expected) {
    const auto masked = dingosdk::text::mask_bad_words(text);
    expect(masked == expected, std::string("mask: \"") + std::string(text) + "\" -> \"" + masked + "\", wanted \"" +
                                   std::string(expected) + "\"");
}
} // namespace

int main() {
    // The built-in word list ships empty: Engine/Core/Text/bad_words.txt is
    // emptied so the game never masks a name or a chat line of its own.
    // Everything a built-in list used to flag now passes untouched -- but the
    // filter itself is still here, because a server's own lists (set_word_lists
    // below) are pushed at runtime and must keep working.
    clean("fuck");
    clean("FUCK this server");
    clean("Shithead's park");
    clean("sh1t");
    clean("@$$ hats");
    clean("evil ass rape server");
    clean("Hello World");
    clean("Scunthorpe Skaters");
    clean("Physical Therapist Pipe");
    clean("");
    // With no built-in words, masking leaves every line exactly as written.
    masks("what the fuck dude", "what the fuck dude");
    masks("shit happens", "shit happens");
    masks("fuckface", "fuckface");
    masks("Hello friends", "Hello friends");
    // The backend's lists still drive the filter: its words are filtered,
    // the built-in's (empty) are not, and the ones not allowed at all are
    // both filtered and told apart.
    {
        using namespace dingosdk::text;
        const auto expect = [&](bool ok, const char* what) {
            if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
        };
        expect(!contains_forbidden_words("what the fuck"), "a word was forbidden before any list said so");
        const std::vector<std::string> filtered{"Darn", "heck"}, forbidden{"Zorblat", "@$$hat"};
        set_word_lists(filtered, forbidden);
        expect(!contains_bad_words("what the fuck") && contains_bad_words("oh D4RN it") && contains_bad_words("check"),
               "the given filtered words are not the ones filtered");
        expect(contains_forbidden_words("you z0rblat") && contains_forbidden_words("megazorblatter") && contains_forbidden_words("a s s h a t") &&
                   !contains_forbidden_words("darn heck") && !contains_forbidden_words("zorb lat"),
               "the words not allowed at all are not matched as the filter matches");
        expect(mask_bad_words("darn you zorblat") == "**** you *******", "a forbidden word is not masked like a filtered one");
        set_word_lists({}, {});
        expect(!contains_bad_words("darn zorblat fuck") && !contains_forbidden_words("zorblat") && mask_bad_words("darn") == "darn",
               "empty lists still filter");
        reset_word_lists();
        expect(!contains_bad_words("what the fuck") && !contains_bad_words("darn") && !contains_forbidden_words("zorblat"),
               "reset did not return to the (empty) built-in list");
    }
    if (failures == 0) std::printf("word filter: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
