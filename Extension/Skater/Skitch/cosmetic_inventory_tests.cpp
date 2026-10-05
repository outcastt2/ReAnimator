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
            store.seed_cosmetic_inventory({"Own_TestReserved"});
            store.seed_object_inventory({"Own_TestObjectReserved"});
            store.reconcile_inventory({"Own_TestReserved"},{"Own_TestObjectReserved"});
            auto at=store.snapshot();
            const bool cosmetic=at.customization.at("inventory").at("Own_TestReserved").get<bool>();
            const bool object=at.extensions.at("object_dropper").at("inventory").at("Own_TestObjectReserved").get<bool>();
            // With the reserved-item unlocker off, one reconcile refresh revokes a
            // seeded reserved item again even though both unlock options are on.
            require(cosmetic==profile::unlock_reserved_items);
            require(object==profile::unlock_reserved_items);
        }
        {
            profile::Store store(save);
            store.reconcile_inventory({"Own_TestReserved"},{"Own_TestObjectReserved"});
            if (profile::unlock_reserved_items) {
                require(store.snapshot().customization.at("inventory").at("Own_TestReserved").get<bool>());
                store.set_bool_option(profile::unlock_cosmetics_option,false);
                store.reconcile_inventory({"Own_TestReserved"},{"Own_TestObjectReserved"});
                auto at=store.snapshot();
                require(!at.customization.at("inventory").at("Own_TestReserved").get<bool>());
                require(at.extensions.at("object_dropper").at("inventory").at("Own_TestObjectReserved").get<bool>());
            } else {
                auto at=store.snapshot();
                require(!at.customization.at("inventory").at("Own_TestReserved").get<bool>());
                require(!at.extensions.at("object_dropper").at("inventory").at("Own_TestObjectReserved").get<bool>());
            }
        }
        for(const auto suffix : {"","-wal","-shm"}) std::filesystem::remove(std::filesystem::path(save.string()+suffix));
        std::cout<<"Reserved inventory reconcile matches the unlocker switch; explicit option disable still works.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
