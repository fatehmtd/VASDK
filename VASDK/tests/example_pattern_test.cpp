// Intent examples written as patterns: (a|b), [optional], ${set}, escapes. What each spells, in
// order and once; sets inside sets, several levels deep, with the same syntax; the scopes a
// ${name} is looked up in (the intent's, the file's, the packs', a pack seeing only itself); and
// the mistakes, each with where it is: unbalanced brackets, an unknown set, sets in a loop, a
// pattern that spells too many. Also the even sample an encoder or an LLM is given.

#include <VASDK/flow/ExamplePattern.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace va::flow;

namespace {

using Sentences = std::vector<std::string>;

std::shared_ptr<PhraseScope> scope(PhraseScope::Kind kind, std::map<std::string, std::vector<std::string>> sets,
                                   std::string name = {}) {
    auto made = std::make_shared<PhraseScope>();
    made->kind = kind;
    made->name = std::move(name);
    made->where = kind == PhraseScope::Kind::Pack ? "pack \"" + made->name + "\"" : "agent.txtpb";
    made->sets = std::move(sets);
    return made;
}

Sentences spelled(const std::string& pattern) {
    std::string error;
    const auto sentences = expandPattern(pattern, &error);
    EXPECT_TRUE(sentences.has_value()) << pattern << ": " << error;
    return sentences.value_or(Sentences{});
}

std::string mistake(const std::string& pattern) {
    std::string error;
    EXPECT_FALSE(expandPattern(pattern, &error).has_value()) << pattern << " should be a mistake";
    return error;
}

TEST(ExamplePattern, PlainTextIsItself) {
    EXPECT_EQ(spelled("turn on the light"), Sentences{"turn on the light"});
    EXPECT_EQ(spelled("  turn   on  "), Sentences{"turn on"}) << "spaces collapsed";
}

TEST(ExamplePattern, AlternativesAndOptionalParts) {
    EXPECT_EQ(spelled("(turn|switch) on"), (Sentences{"turn on", "switch on"}));
    EXPECT_EQ(spelled("turn on [the] light"), (Sentences{"turn on the light", "turn on light"}));
    EXPECT_EQ(spelled("(the|) light"), (Sentences{"the light", "light"})) << "an empty alternative";
    EXPECT_EQ(spelled("light[s]"), (Sentences{"lights", "light"})) << "next to a word, part of it";
    EXPECT_EQ(spelled("[a|the] light"), (Sentences{"a light", "the light", "light"})) << "optional one of";
}

TEST(ExamplePattern, NestingSpellsEveryCombinationInOrder) {
    EXPECT_EQ(spelled("(turn (on|off)|switch [it] off)"),
              (Sentences{"turn on", "turn off", "switch it off", "switch off"}));
    EXPECT_EQ(spelled("(a|b) (c|d)"), (Sentences{"a c", "a d", "b c", "b d"}));
}

TEST(ExamplePattern, EachSentenceOnce) {
    EXPECT_EQ(spelled("(light|light) [on|on]"), (Sentences{"light on", "light"}));
}

TEST(ExamplePattern, EscapesAreTheCharacterItself) {
    EXPECT_EQ(spelled("\\(a\\|b\\) \\[c\\] \\$\\{d\\}"), Sentences{"(a|b) [c] ${d}"});
    EXPECT_EQ(spelled("price in $ please"), Sentences{"price in $ please"}) << "a $ not before { is itself";
}

TEST(ExamplePattern, EntitiesStayAsWritten) {
    EXPECT_EQ(spelled("(call|phone) @contact [now]"),
              (Sentences{"call @contact now", "call @contact", "phone @contact now", "phone @contact"}));
}

TEST(ExamplePattern, WildcardsArePassedOnAsWords) {
    EXPECT_EQ(spelled("turn _ (light|lamp) on"), (Sentences{"turn _ light on", "turn _ lamp on"}));
    EXPECT_EQ(spelled("call + back [*]"), (Sentences{"call + back *", "call + back"}));
    EXPECT_TRUE(isWildcard("_"));
    EXPECT_FALSE(isWildcard("fan_level")) << "inside a word it's the word's";
}

TEST(ExamplePattern, MistakesSayWhere) {
    EXPECT_NE(mistake("(turn on").find("column 1: a '(' with no ')'"), std::string::npos);
    EXPECT_NE(mistake("turn on)").find("column 8: unmatched ')'"), std::string::npos);
    EXPECT_NE(mistake("[turn on)").find("a ')' where a ']' was expected"), std::string::npos);
    EXPECT_NE(mistake("turn \\").find("a '\\' with nothing after it"), std::string::npos);
    EXPECT_NE(mistake("${on").find("a '${' with no '}'"), std::string::npos);
    EXPECT_NE(mistake("${on} light").find("no phrase sets here"), std::string::npos) << "a language pack's phrase";
}

TEST(ExamplePattern, TooManySentencesIsAMistake) {
    // 4^5 = 1024 sentences
    const auto error = mistake("(a|b|c|d) (a|b|c|d) (a|b|c|d) (a|b|c|d) (a|b|c|d)");
    EXPECT_NE(error.find("spells more than 256 sentences"), std::string::npos) << error;
    EXPECT_EQ(spelled("(a|b|c|d) (a|b|c|d) (a|b|c|d) (a|b|c|d)").size(), 256u) << "256 is allowed";
}

TEST(ExamplePattern, SetsInsideSetsWithTheFullSyntax) {
    ExampleExpander expander({scope(PhraseScope::Kind::File,
                                    {{"light", {"flashlight", "(light|lights)"}},
                                     {"on", {"(turn|switch) on"}},
                                     {"command", {"${on} [the] ${light}"}}})});
    std::string error;
    const auto sentences = expander.expand("${command} please", nullptr, &error);
    ASSERT_TRUE(sentences.has_value()) << error;
    EXPECT_EQ(sentences->size(), 12u) << "2 verbs x (the|) x 3 nouns";
    EXPECT_EQ(sentences->front(), "turn on the flashlight please");
    EXPECT_EQ(sentences->back(), "switch on lights please");
}

TEST(ExamplePattern, IntentSetsComeFirstThenTheFileThenPacks) {
    auto file = scope(PhraseScope::Kind::File, {{"on", {"turn on"}}});
    auto pack = scope(PhraseScope::Kind::Pack, {{"on", {"power on"}}, {"off", {"power off"}}}, "devices");
    ExampleExpander expander({file, pack});
    auto intent = scope(PhraseScope::Kind::Intent, {{"on", {"light up"}}});
    std::string error;
    EXPECT_EQ(expander.expand("${on}", intent.get(), &error).value_or(Sentences{}), Sentences{"light up"}) << "the intent's";
    EXPECT_EQ(expander.expand("${on}", nullptr, &error).value_or(Sentences{}), Sentences{"turn on"}) << "the file's";
    EXPECT_EQ(expander.expand("${devices.on}", nullptr, &error).value_or(Sentences{}), Sentences{"power on"}) << "the pack's, named";
    EXPECT_EQ(expander.expand("${off}", nullptr, &error).value_or(Sentences{}), Sentences{"power off"}) << "a pack's, unnamed";
    const auto notes = expander.hidingNotes(intent.get());
    EXPECT_EQ(notes.size(), 3u) << "intent hides the file's and the pack's; the file hides the pack's";
}

TEST(ExamplePattern, AFilesSetDoesntSeeTheIntents) {
    auto file = scope(PhraseScope::Kind::File, {{"command", {"${verb} light"}}});
    ExampleExpander expander({file});
    auto intent = scope(PhraseScope::Kind::Intent, {{"verb", {"turn on"}}});
    std::string error;
    EXPECT_FALSE(expander.expand("${command}", intent.get(), &error).has_value());
    EXPECT_NE(error.find("no phrase set \"verb\""), std::string::npos) << error;
}

TEST(ExamplePattern, APacksSetsSeeOnlyThatPack) {
    auto file = scope(PhraseScope::Kind::File, {{"thing", {"light"}}});
    auto pack = scope(PhraseScope::Kind::Pack, {{"on", {"turn on ${thing}"}}}, "devices");
    ExampleExpander expander({file, pack});
    std::string error;
    EXPECT_FALSE(expander.expand("${devices.on}", nullptr, &error).has_value());
    EXPECT_NE(error.find("pack \"devices\" has no set \"thing\""), std::string::npos) << error;
}

TEST(ExamplePattern, AnUnnamedSetInTwoPacksMustBeNamed) {
    auto devices = scope(PhraseScope::Kind::Pack, {{"on", {"power on"}}}, "devices");
    auto car = scope(PhraseScope::Kind::Pack, {{"on", {"start"}}}, "car");
    ExampleExpander expander({scope(PhraseScope::Kind::File, {}), devices, car});
    std::string error;
    EXPECT_FALSE(expander.expand("${on}", nullptr, &error).has_value());
    EXPECT_NE(error.find("${devices.on}, ${car.on}"), std::string::npos) << error;
    EXPECT_EQ(expander.expand("${car.on}", nullptr, &error).value_or(Sentences{}), Sentences{"start"});
    EXPECT_FALSE(expander.expand("${radio.on}", nullptr, &error).has_value());
    EXPECT_NE(error.find("this file uses no pack \"radio\""), std::string::npos) << error;
}

TEST(ExamplePattern, SetsInALoopAreAMistake) {
    ExampleExpander expander({scope(PhraseScope::Kind::File, {{"a", {"x ${b}"}}, {"b", {"y ${c}"}}, {"c", {"z ${a}"}}})});
    std::string error;
    EXPECT_FALSE(expander.expand("${a}", nullptr, &error).has_value());
    EXPECT_NE(error.find("loop: a → b → c → a"), std::string::npos) << error;
}

TEST(ExamplePattern, WhatWasUsedIsKnown) {
    auto file = scope(PhraseScope::Kind::File, {{"on", {"turn on"}}, {"off", {"turn off"}}});
    auto pack = scope(PhraseScope::Kind::Pack, {{"up", {"raise"}}}, "devices");
    ExampleExpander expander({file, pack});
    std::string error;
    ASSERT_TRUE(expander.expand("${on} the light", nullptr, &error).has_value()) << error;
    EXPECT_TRUE(expander.used(*file, "on"));
    EXPECT_FALSE(expander.used(*file, "off"));
    EXPECT_FALSE(expander.usedAny(*pack));
}

TEST(ExamplePattern, ASampleIsSpreadEvenly) {
    Sentences many;
    for (int i = 0; i < 100; ++i) many.push_back(std::to_string(i));
    const auto sample = sampleExamples(many, 4);
    EXPECT_EQ(sample, (Sentences{"0", "25", "50", "75"}));
    EXPECT_EQ(sampleExamples({"a", "b"}, 8), (Sentences{"a", "b"})) << "fewer: all of them";
}

TEST(ExamplePattern, SetNames) {
    EXPECT_TRUE(isPhraseSetName("on"));
    EXPECT_TRUE(isPhraseSetName("light_words2"));
    EXPECT_FALSE(isPhraseSetName("2on"));
    EXPECT_FALSE(isPhraseSetName("light-words"));
    EXPECT_FALSE(isPhraseSetName(""));
}

} // namespace
