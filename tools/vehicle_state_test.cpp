#include "world/B3D.h"
#include "script/GameData.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
using namespace ht2;
static uint64_t meshHash(const std::vector<B3D::SubMesh>& subs,bool wheelsOnly=false) {
    uint64_t hash = 1469598103934665603ULL;
    auto bytes = [&](const void* ptr, size_t size) {
        const auto* p = (const unsigned char*)ptr;
        for (size_t i=0;i<size;++i) { hash ^= p[i]; hash *= 1099511628211ULL; }
    };
    for (const auto& sm:subs) if(!wheelsOnly||sm.tyreState >= 0) {
        bytes(sm.pos.data(),sm.pos.size()*sizeof(float));
        bytes(sm.idx.data(),sm.idx.size()*sizeof(uint32_t));
        bytes(sm.material.data(),sm.material.size());
    }
    return hash;
}
int main(int argc,char** argv) {
    const std::string data=argc>1?argv[1]:"original";
    GameData gd; B3D b;
    if(!gd.load(data)||!b.load(data+"/COMMON/TRUCKS.B3D"))return 1;
    std::set<std::string> models;
    for(const auto& v:gd.vehicles)models.insert(v.model);
    for(const auto& v:gd.parked)models.insert(v.model);
    int checks=0,failed=0;
    auto check=[&](bool ok,const std::string& what) {
        ++checks; failed+=!ok; printf("%s %s\n",ok?"PASS":"FAIL",what.c_str());
    };
    size_t bodyModels=0,wheelModels=0,rawCount=0,intactCount=0,missing=0;
    for(const auto& model:models) {
        if(!b.buildNamedScene(model)) {
            // Two parked animation/prop entries are not TRUCKS.B3D containers.
            // Keep the gap explicit rather than claiming all 59 models work.
            ++missing;
            check(model=="AnmObj0231"||model=="AnmObj0241",model+" is a known missing parked prop, not a new model failure");
            continue;
        }
        const auto raw=b.submeshes().size(); rawCount+=raw;
        check(b.buildVehicleScene(model),model+" intact scene exists");
        bool healthy=true; size_t bodies=0,tyres=0;
        for(const auto& sm:b.submeshes()) {
            healthy &= sm.damageState<=0 && sm.tyreState<=0;
            bodies += sm.damageState==0; tyres += sm.tyreState==0;
            std::string m=sm.material;
            std::transform(m.begin(),m.end(),m.begin(),[](unsigned char c){return (char)std::tolower(c);});
            healthy &= m.find("alld")==std::string::npos && m.find("damage")==std::string::npos &&
                       (m.size()<3 || m.substr(m.size()-3)!="dmg");
        }
        const auto intact=b.submeshes().size(); intactCount+=intact;
        bodyModels += bodies>0; wheelModels += tyres>0;
        const auto wheels=meshHash(b.submeshes(),true),healthyHash=meshHash(b.submeshes());
        check(healthy,model+" has only intact body/tyre alternatives and materials");
        check(intact<=raw,model+" does not multiply alternative/LOD geometry");
        check(b.buildVehicleScene(model,3),model+" damaged scene exists");
        bool selected=true; size_t damaged=0;
        for(const auto& sm:b.submeshes()) {
            selected &= sm.tyreState<=0;
            damaged += sm.damageState>0;
        }
        check(selected && (!bodies || (damaged>0&&healthyHash!=meshHash(b.submeshes()))),model+" body changes without spontaneous tyre damage");
        check(wheels==meshHash(b.submeshes(),true),model+" tyre geometry unchanged by body impact");
        b.buildVehicleScene(model);
        check(intact==b.submeshes().size() && healthyHash==meshHash(b.submeshes()),model+" repair restores intact state");
    }
    check(bodyModels>20&&wheelModels>20,"real damage and tyre switches covered across catalogue");
    printf("RESULT vehicle states %d checks, %d failures; %zu tested / %zu listed, %zu missing parked props; %zu body / %zu wheel; submeshes %zu -> %zu\n",
           checks,failed,models.size()-missing,models.size(),missing,bodyModels,wheelModels,rawCount,intactCount);
    return failed?1:0;
}
