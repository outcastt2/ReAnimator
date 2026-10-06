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
    const auto save=folder/("inventory-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".sqlite3");
    try {
        {
            profile::Store store(save);
            store.set_bool_option(profile::unlock_cosmetics_option,true);
            store.set_bool_option(profile::unlock_objects_option,true);
            // The catalog's decision is simulated here: gestures and build items
            // go to the seed lists, a reserved item to the revoke list.
            store.seed_cosmetic_inventory({"Own_TestGesture"});
            store.seed_object_inventory({"Own_TestObject"});
            store.seed_cosmetic_inventory({"Own_TestClothing"}); // an earlier build granted it
            store.reconcile_inventory({"Own_TestClothing"},{});
            auto at=store.snapshot();
            require(at.customization.at("inventory").at("Own_TestGesture").get<bool>());
            require(at.extensions.at("object_dropper").at("inventory").at("Own_TestObject").get<bool>());
            require(!at.customization.at("inventory").at("Own_TestClothing").get<bool>());
        }
        {
            profile::Store store(save);
            auto at=store.snapshot();
            // The policy survives a restart for everything that was not revoked.
            require(at.customization.at("inventory").at("Own_TestGesture").get<bool>());
            require(at.extensions.at("object_dropper").at("inventory").at("Own_TestObject").get<bool>());
            require(!at.customization.at("inventory").at("Own_TestClothing").get<bool>());
            // A revoke list is unconditional, and an option-off seed stays off.
            store.set_bool_option(profile::unlock_cosmetics_option,false);
            store.seed_cosmetic_inventory({"Own_TestGestureOff"});
            {
                const auto inventory=store.snapshot().customization.at("inventory");
                const bool off_owned=inventory.contains("Own_TestGestureOff") &&
                    inventory.at("Own_TestGestureOff").is_boolean() &&
                    inventory.at("Own_TestGestureOff").get<bool>();
                require(!off_owned);
            }
            store.reconcile_inventory({"Own_TestGesture"},{});
            require(!store.snapshot().customization.at("inventory").at("Own_TestGesture").get<bool>());
        }
        for(const auto suffix : {"","-wal","-shm"}) std::filesystem::remove(std::filesystem::path(save.string()+suffix));
        std::cout<<"Class-policy inventory: gestures/objects stay owned, revoked classes never do.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
