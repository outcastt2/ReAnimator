// A config written by an older server gains the settings added since, keeping its own values.
#include "Server/server_config.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
int failures{};
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
std::string text(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
} // namespace

int run() {
    using namespace dingosdk::server;
    const auto folder = std::filesystem::temp_directory_path() / "reskate_server_config_tests";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto file = folder / "ReSkateServer.json";
    // No enforce_tuning, and a votes object without "seconds".
    std::ofstream(file, std::ios::binary) << R"({"name": "Old Server", "tps": 60, "boosts": false,
        "votes": {"map": {"enabled": true, "percent": 60}}})";

    std::vector<std::string> added;
    const auto config = load_config(file, &added);
    const auto has = [&](std::string_view name) { return std::ranges::find(added, name) != added.end(); };
    check(has("enforce_tuning") && has("votes.seconds") && has("votes.kick") && has("score_check") && has("score_allow") &&
              has("global_bans"),
          "New settings not reported");
    check(!has("name") && !has("tps") && !has("votes.map"), "Settings the file had reported as new");
    const auto written = text(file);
    check(written.find("\"enforce_tuning\"") != std::string::npos && written.find("\"seconds\"") != std::string::npos,
          "New settings not written into the file");
    check(config.name == "Old Server" && config.tps == 60 && !config.boosts && config.votes.map.enabled &&
              config.votes.map.percent == 60 && config.enforce_tuning && config.global_bans,
          "The file's own values or the new defaults were lost");
    const auto reloaded = load_config(file);
    check(reloaded.name == "Old Server" && !reloaded.boosts && reloaded.votes.map.percent == 60, "Values lost on rewrite");

    // An up-to-date file is left alone.
    const auto before = std::filesystem::last_write_time(file);
    added.clear();
    static_cast<void>(load_config(file, &added));
    check(added.empty() && std::filesystem::last_write_time(file) == before, "A complete config was rewritten");

    // The scoring check: its mode and the accepted fingerprints survive a save, and bad entries are dropped.
    check(reloaded.score_check == "warn" && reloaded.score_allow.empty(), "Scoring check defaults wrong");
    auto scoring = reloaded;
    scoring.score_check = "kick";
    scoring.score_allow = {0x00c0ffee12345678ULL, 0xffffffffffffffffULL};
    save_config(scoring);
    const auto saved = load_config(file);
    check(saved.score_check == "kick" && saved.score_allow == scoring.score_allow, "Scoring check settings lost");
    check(scoring_text(0x00c0ffee12345678ULL) == "00c0ffee12345678", "Fingerprint not written as 16 hex digits");
    check(parse_scoring("00C0FFEE12345678") == 0x00c0ffee12345678ULL && !parse_scoring("0") && !parse_scoring("xyz") &&
              !parse_scoring("123456789abcdef01"),
          "Fingerprint text not read back, or a bad one accepted");
    std::ofstream(file, std::ios::binary) << R"({"score_check": "ban", "score_allow": ["nothex", "0", "abc"]})";
    const auto odd = load_config(file);
    check(odd.score_check == "warn" && odd.score_allow == std::vector<std::uint64_t>{0xabc}, "Bad scoring settings not cleaned");

    // A port outside 1-65535 is refused, not wrapped to another port, and the file keeps the typo.
    const auto refused = [&](const char *json) {
        std::ofstream(file, std::ios::binary) << json;
        try {
            static_cast<void>(load_config(file));
        } catch (const std::exception &) {
            return text(file) == json;
        }
        return false;
    };
    check(refused(R"({"query_port": 70000})") && refused(R"({"port": 65536})") && refused(R"({"port": 0})"),
          "An out-of-range port was accepted or the file rewritten");
    std::ofstream(file, std::ios::binary) << R"({"port": 65535, "query_port": 1})";
    const auto edges = load_config(file);
    check(edges.port == 65535 && edges.query_port == 1, "Ports at the ends of the range refused");

    std::filesystem::remove_all(folder);
    if (failures) return 1;
    std::cout << "server config: ok\n";
    return 0;
}
int main() {
    try {
        return run();
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
