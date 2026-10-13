#include "board_gesture.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <cstdint>
using namespace dingosdk;
unsigned checks{};
void route_tests();
void check(bool value,const char* name) { ++checks; if(!value) throw std::runtime_error(name); }
template<class T> void put(std::uintptr_t at,T value) { std::memcpy(reinterpret_cast<void*>(at),&value,sizeof(value)); }
template<std::size_t N> std::uintptr_t address(std::array<unsigned char,N>& a) { return reinterpret_cast<std::uintptr_t>(a.data()); }
int main() {
    void* reservation{};
    try {
        reservation=VirtualAlloc(nullptr,0x71f0000,MEM_RESERVE,PAGE_NOACCESS);
        check(reservation!=nullptr,"fake image reservation");
        const auto base=reinterpret_cast<std::uintptr_t>(reservation);
        check(VirtualAlloc(reinterpret_cast<void*>(base+0x71ee000),4096,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"fake manager pointer page");
        std::array<unsigned char,0xb0> manager{};
        std::array<unsigned char,88> sax_asset{},arms_asset{},third_asset{};
        std::array<unsigned char,40> sax{},arms{},reloaded{},third{};
        std::array<unsigned char,24> sax_node{},arms_node{},third_node{};
        std::array<std::uintptr_t,1> buckets{address(sax_node)};
        const char sax_key[]="own_rctn_gesture_all_airsaxophone",arms_key[]="own_rctn_gesture_all_armscrossed";
        put(base+0x71ee610,address(manager));
        put(address(manager),base+0x6089cc8); put(address(manager)+0x30,reinterpret_cast<std::uintptr_t>(buckets.data()));
        put(address(manager)+0x38,std::uint32_t{1}); put(address(manager)+0x3c,std::uint32_t{3});manager[0xa8]=1;
        const auto item=[&](auto& asset,auto& data,auto& node,const char* key,std::uint32_t hash,std::int32_t value) {
            put(address(asset)+0x28,address(data));put(address(asset)+0x38,reinterpret_cast<std::uintptr_t>(key));put(address(asset)+0x48,hash);
            put(address(node),hash);put(address(node)+8,address(asset));
            put(address(data),base+0x61047b0);put(address(data)+8,base+0x721d2d8);put(address(data)+0x18,base+0xabc);
            put(address(data)+0x20,value);
        };
        item(sax_asset,sax,sax_node,sax_key,4238209789u,47);
        item(arms_asset,arms,arms_node,arms_key,986424460u,79);
        put(address(sax_node)+0x10,address(arms_node));put(address(arms_node)+0x10,address(third_node));arms[0x24]=1;
        const char third_key[]="own_rctn_gesture_all_testdance";
        item(third_asset,third,third_node,third_key,123,50);third[0x25]=1;
        board_gesture::set_enabled(true);board_gesture::tick(base);
        check(sax[0x24]==0,"wrong reference gesture id prevents writes");
        put(address(arms)+0x20,std::int32_t{80});Sleep(1010);board_gesture::tick(base);
        check(sax[0x24]==1,"verified Air Sax flag applied");
        check(third[0x24]==1 && third[0x25]==0,"additional gesture automatically layered and movement allowed");
        check(arms[0x24]==1 && sax[0x25]==0,"reference and stationary flag untouched");
        board_gesture::set_enabled(false);Sleep(1010);board_gesture::tick(base);
        check(sax[0x24]==0,"disable restores own mutation");
        check(third[0x24]==0 && third[0x25]==1,"additional gesture original flags restored");
        sax[0x24]=1;board_gesture::set_enabled(true);Sleep(1010);board_gesture::tick(base);
        board_gesture::set_enabled(false);board_gesture::tick(base);
        check(sax[0x24]==1,"pre-existing override is not owned or restored");
        sax[0x24]=0;board_gesture::set_enabled(true);Sleep(1010);board_gesture::tick(base);
        check(sax[0x24]==1,"override can rearm");
        put(address(manager)+0x30,std::uintptr_t{0}); // iterator unavailable; verified cached item identities remain valid
        board_gesture::tick(base,false);
        check(sax[0x24]==0 && third[0x25]==1,"dismount restores native flags without waiting for scan timer");
        board_gesture::tick(base,true);
        check(sax[0x24]==1 && third[0x25]==0,"remount immediately reapplies board flags");
        check(sax[0x24]==1 && third[0x24]==1,"mount transitions do not require a full menu bucket scan");
        put(address(manager)+0x30,reinterpret_cast<std::uintptr_t>(buckets.data()));
        reloaded=sax;put(address(sax_asset)+0x28,address(reloaded));
        board_gesture::set_enabled(false);Sleep(1010);board_gesture::tick(base);
        check(reloaded[0x24]==1,"reload identity change does not restore a different asset");
        check(sax[0x24]==1,"obsolete borrowed object not touched");
        VirtualFree(reservation,0,MEM_RELEASE);
        reservation=nullptr;
        route_tests();
        std::cout<<checks<<" gesture ownership and restore checks passed; synthetic assets only\n";
        return 0;
    } catch(const std::exception& e) {
        if(reservation) VirtualFree(reservation,0,MEM_RELEASE);
        std::cerr<<e.what()<<'\n';return 1;
    }
}
