#include "script/LoadGuilt.h"
#include <iostream>
#include <string>
#include <cmath>
#include <limits>
using namespace ht2;

// Machine-readable port endpoint for the original-instruction differential.
// Each input: warmup share containerFlags count; then per item:
// flags hasRelated relatedFlags value40 marker88. One output per input state.
int main(int argc,char** argv) {
    if(argc>1&&std::string(argv[1])=="--self-test") {
        int checks=0,failures=0;
        auto probe=[&](unsigned warmup,double share,unsigned flags,
                       std::vector<LoadGuiltCargo> cargo,std::vector<int> expected) {
            LoadGuiltState state;state.currentPlayerShare=share;
            state.containerFlags=flags;state.cargo=std::move(cargo);
            EngineRandom rng;for(unsigned i=0;i<warmup;++i)rng.bounded(1000000);
            accountForLoadGuilt(state,rng);
            std::vector<int> actual={state.illegalWanted,state.mafiaWanted,
                                     state.suspiciousLoad,(int)rng.bounded(1000000)};
            for(const auto& c:state.cargo)actual.push_back(c.marker88);
            ++checks;failures+=actual!=expected;
            std::cout<<(actual==expected?"PASS":"FAIL")<<" original guilt fixture "<<checks<<'\n';
        };
        // Outputs captured by executing local king.exe, including the next draw.
        probe(0,1,2,{{0,true,2,0,-1}},{0,0,1,482688,-1});
        probe(7,0,0,{{0,true,2,20000,-1},{0,false,0,0,-1}},{1,0,0,773857,-1,-1});
        probe(7,std::nextafter(.15,1.0),0,{{0,true,2,0,1}},{0,1,0,439392,1});
        probe(7,1,0,{{0,true,2,0,-1}},{1,0,0,773857,0});
        probe(0,0,2,{},{0,0,1,58232});
        probe(31,1,2,{{2,false,0,std::numeric_limits<double>::quiet_NaN(),-1}},
              {0,0,1,912689,-1});
        std::cout<<"RESULT load guilt "<<checks<<" checks, "<<failures<<" failures\n";
        return failures?1:0;
    }
    unsigned warmup, count;
    const unsigned rounds=argc>1&&std::string(argv[1])=="--twice"?2:1;
    std::string share;
    while (std::cin >> warmup >> share) {
        LoadGuiltState state;
        state.currentPlayerShare = std::stod(share);
        if (!(std::cin >> state.containerFlags >> count) || count > 1000) return 2;
        state.cargo.resize(count);
        for (auto& item : state.cargo) {
            std::string value;
            if (!(std::cin >> item.flags >> item.hasRelated >> item.relatedFlags >> value >> item.marker88))
                return 2;
            item.value40 = std::stod(value);
        }
        EngineRandom rng;
        for (unsigned i=0;i<warmup;++i) rng.bounded(1000000);
        for(unsigned round=0;round<rounds;++round)accountForLoadGuilt(state,rng);
        std::cout << state.illegalWanted << ' ' << state.mafiaWanted << ' '
                  << state.suspiciousLoad << ' ' << rng.bounded(1000000);
        for (const auto& item : state.cargo) std::cout << ' ' << item.marker88;
        std::cout << '\n';
    }
    return std::cin.eof()?0:2;
}
