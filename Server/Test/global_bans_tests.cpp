#include "Server/global_bans.h"
#include "Extension/Multiplayer/developer_identity.h"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace dingosdk::server;
using dingosdk::multiplayer::reskate_banned;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
constexpr std::uint64_t griefer = 76561198000000009ULL, cheater = 76561198000000010ULL, player = 76561198000000011ULL;

void lists() {
    check(!reskate_banned(griefer), "A player was banned before any list was read");
    // The first list read counts as news even when it bans nobody, so the server can say it has it.
    auto read = use_ban_list(R"({"categories":{"dev":["76561198000000011"]},"banned":[]})");
    check(read.ok && read.changed && read.banned == 0 && read.problem.empty() && !reskate_banned(griefer),
          "A list with nobody on it was not taken as read");
    read = use_ban_list(R"({"categories":{},"banned":["76561198000000010","76561198000000009"]})");
    check(read.ok && read.changed && read.banned == 2 && reskate_banned(griefer) && reskate_banned(cheater) && !reskate_banned(player),
          "The ban list did not ban the players on it");
    // The other lists changing is no news about bans.
    read = use_ban_list(R"({"categories":{"homie":["76561198000000011"]},"banned":["76561198000000009","76561198000000010"]})");
    check(read.ok && !read.changed && read.banned == 2, "The same ban list counted as a change");
    // An answer that is not the lists (the backend down, a proxy's error page) lifts no ban.
    for (const std::string_view wrong : {"", "<html>502 Bad Gateway</html>", R"({"banned":["76561198000000009"]})",
                                         R"({"categories":{},"banned":["everyone"]})"}) {
        read = use_ban_list(wrong);
        check(!read.ok && !read.problem.empty() && reskate_banned(griefer) && reskate_banned(cheater),
              "A bad answer was accepted, or lifted the bans: " + std::string(wrong));
    }
    read = use_ban_list(R"({"categories":{},"banned":["76561198000000010"]})");
    check(read.ok && read.changed && read.banned == 1 && !reskate_banned(griefer) && reskate_banned(cheater), "A lifted ban stayed");
    // A backend from before it had bans bans nobody.
    read = use_ban_list(R"({"categories":{"dev":[]}})");
    check(read.ok && read.changed && read.banned == 0 && !reskate_banned(cheater), "A list without bans kept one");
}

// --live: the deployed backend, read the way the server reads it. Not part of the
// test run, which stays off the network.
int live() {
    const auto read = read_global_bans();
    if (read.ok) std::cout << "the backend's ban list was read: " << read.banned << " banned\n";
    else std::cout << "the backend's ban list could not be read: " << read.problem << '\n';
    return read.ok ? 0 : 1;
}
} // namespace

int main(int count, char **arguments) {
    if (count == 2 && std::string_view(arguments[1]) == "--live") return live();
    lists();
    std::cout << "global ban tests passed\n";
}
