// Runs the fuzz targets without libFuzzer: each on its seeds, then on mutations of them (bytes
// flipped, inserted, removed, pieces of seeds spliced), from a fixed seed so a failure repeats.
// A crash or a sanitizer report fails the ctest; for coverage-guided fuzzing build
// VASDK_BUILD_FUZZERS with clang.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" int fuzzAgent(const uint8_t* data, size_t size);
extern "C" int fuzzExpression(const uint8_t* data, size_t size);
extern "C" int fuzzWords(const uint8_t* data, size_t size);

namespace {

struct Target {
    const char* name;
    int (*run)(const uint8_t*, size_t);
    std::vector<std::string> seeds;
};

class Random {
public:
    explicit Random(uint64_t seed) : _state(seed) {}
    uint64_t next() {
        _state ^= _state << 13;
        _state ^= _state >> 7;
        _state ^= _state << 17;
        return _state;
    }
    size_t below(size_t limit) { return limit == 0 ? 0 : static_cast<size_t>(next() % limit); }

private:
    uint64_t _state;
};

std::string mutate(const std::vector<std::string>& seeds, Random& random) {
    std::string text = seeds[random.below(seeds.size())];
    const size_t edits = 1 + random.below(6);
    for (size_t i = 0; i < edits; ++i) {
        switch (random.below(4)) {
        case 0:   // flip
            if (!text.empty()) text[random.below(text.size())] = static_cast<char>(random.below(256));
            break;
        case 1:   // insert a byte, often one that means something here
            text.insert(random.below(text.size() + 1), 1, "{}\"$@:.'()= \\\n0aZ"[random.below(17)]);
            break;
        case 2:   // remove
            if (!text.empty()) text.erase(random.below(text.size()), 1 + random.below(4));
            break;
        default: {   // splice in a piece of another seed
            const auto& other = seeds[random.below(seeds.size())];
            const size_t from = random.below(other.size() + 1);
            text.insert(random.below(text.size() + 1), other.substr(from, random.below(24)));
        }
        }
    }
    return text;
}

} // namespace

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 3000;
    const std::vector<Target> targets{
        {"agent",
         fuzzAgent,
         {R"(start_flow: "main" intents { name: "book" examples: ["book @service"] }
entity_types { name: "service" list { values { value: "tires" synonyms: ["tyres"] } } }
goals { name: "g" slots { name: "day" type: DATE prompt: "Which day?" } }
flows { name: "main" start_page: "p" pages { name: "p" form { goal: "g" }
  routes { intent: "book" condition: "$form.day != null" steps { say { text: "Booked $form.day." } } target: "END_SESSION" } } })",
          R"(start_flow: "m" triggers { name: "t" source { app_event {} } action { say { text: "Hi $trigger.payload.x" } } }
flows { name: "m" start_page: "a" pages { name: "a" on_enter { ask { say { text: "Yes?" } choices { label: "Yes" } } } } })"}},
        {"expression",
         fuzzExpression,
         {"$session.tier : ('gold', 'platinum') AND NOT $form.confirmed", "len($form.name) > 2 OR $nlu.confidence >= 0.7",
          "Booked for $form.day at ${form.time}, $$5 off.", "exists($page.locals.picked) = true"}},
        {"words",
         fuzzWords,
         {"next tuesday at half past three in the afternoon", "five five five, one two three, double four, six seven",
          "john dot doe at gmail dot com", "an hour and a half", "the 3rd of January 2027", "call 555-123-4567 on 2026-11-10"}},
    };
    Random random(0x5eed);
    const auto started = std::chrono::steady_clock::now();
    for (const auto& target : targets) {
        for (const auto& seed : target.seeds) target.run(reinterpret_cast<const uint8_t*>(seed.data()), seed.size());
        int ran = 0;
        for (; ran < iterations; ++ran) {
            const std::string input = mutate(target.seeds, random);
            target.run(reinterpret_cast<const uint8_t*>(input.data()), input.size());
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(60)) break;   // a time budget, for CI
        }
        std::printf("fuzz %s: %d inputs\n", target.name, ran + static_cast<int>(target.seeds.size()));
    }
    std::printf("fuzz_smoke: all passed\n");
    return 0;
}
