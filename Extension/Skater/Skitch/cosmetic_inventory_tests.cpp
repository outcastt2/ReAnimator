#include "Extension/Profile/local_profile.h"
#include <Windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace dingosdk;
void require(bool ok) { if(!ok) throw std::runtime_error("Cosmetic unlock regression"); }
int main() {
    const auto folder=std::filesystem::current_path()/"skitch-test-saves";
    std::filesystem::create_directories(folder);
    const auto save=folder/("policy-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".sqlite3");
    try {
        // The unlocker is a policy over classes now, not a list of items seeded
        // into the profile: whichever class an item is in decides it, and every
        // other kind of cosmetic is handed back to upstream's rule.
        {
            profile::Store store(save);
            store.set_bool_option(profile::unlock_cosmetics_option,true);
            store.set_bool_option(profile::unlock_objects_option,true);
            const auto at=store.snapshot();
            // Both switches on: the two classes the unlocker owns are granted.
            // A reserved gesture is granted too, which is the point of the class
            // switch -- tier rewards are what a reservation mostly is.
            require(profile::unlocker_decision(at,false,true,false)==profile::Unlock::grant);
            require(profile::unlocker_decision(at,false,true,true)==profile::Unlock::grant);
            require(profile::unlocker_decision(at,true,false,true)==profile::Unlock::grant);
            // A reserved garment is denied: the reserved switch stays off, and
            // it is reached before the open-catalogue switch is even asked.
            require(profile::unlocker_decision(at,false,false,true)==profile::Unlock::deny);
            // An open garment defers to upstream, which grants it on the switch.
            require(profile::unlocker_decision(at,false,false,false)==profile::Unlock::defer);
        }
        {
            profile::Store store(save);
            store.set_bool_option(profile::unlock_cosmetics_option,false);
            store.set_bool_option(profile::unlock_objects_option,false);
            const auto at=store.snapshot();
            // With both switches off the classes do not open, but a garment is
            // still deferred rather than denied, so a profile that saved its own
            // rows keeps what it saved.
            require(profile::unlocker_decision(at,false,true,false)==profile::Unlock::deny);
            require(profile::unlocker_decision(at,true,false,false)==profile::Unlock::deny);
            require(profile::unlocker_decision(at,false,false,false)==profile::Unlock::defer);
            require(profile::unlocker_decision(at,false,false,true)==profile::Unlock::deny);
        }
        for(const auto suffix : {"","-wal","-shm"}) std::filesystem::remove(std::filesystem::path(save.string()+suffix));
        std::cout<<"Class-policy unlocker: gestures/objects granted, reserved denied, the rest deferred to upstream.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
