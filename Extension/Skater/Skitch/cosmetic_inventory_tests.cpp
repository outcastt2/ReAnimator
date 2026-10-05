#include "Extension/Profile/local_profile.h"
#include <Windows.h>
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
            require(at.customization.at("inventory").at("Own_TestReserved").get<bool>());
            require(at.extensions.at("object_dropper").at("inventory").at("Own_TestObjectReserved").get<bool>());
        }
        {
            profile::Store store(save);
            store.reconcile_inventory({"Own_TestReserved"},{"Own_TestObjectReserved"});
            require(store.snapshot().customization.at("inventory").at("Own_TestReserved").get<bool>());
            store.set_bool_option(profile::unlock_cosmetics_option,false);
            store.reconcile_inventory({"Own_TestReserved"},{"Own_TestObjectReserved"});
            auto at=store.snapshot();
            require(!at.customization.at("inventory").at("Own_TestReserved").get<bool>());
            require(at.extensions.at("object_dropper").at("inventory").at("Own_TestObjectReserved").get<bool>());
        }
        for(const auto suffix : {"","-wal","-shm"}) std::filesystem::remove(std::filesystem::path(save.string()+suffix));
        std::cout<<"Reserved cosmetics/objects stay owned across refresh and restart; explicit unlock disable still works.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
