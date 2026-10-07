#include "game/Traffic.h"
#include <cmath>
#include <cstdio>
using namespace ht2;
int main() {
    int failures=0,checks=0;
    auto check=[&](bool ok,const char* name){++checks;failures+=!ok;printf("%s %s\n",ok?"PASS":"FAIL",name);};
    Way::Seg road;road.width=8;road.pts={{0,0,-2000},{0,0,2000}};
    Traffic tr;tr.build({road},{});tr.setModels({0});
    Traffic::Player player;player.valid=true;tr.setPlayer(player);
    tr.spawn(0,0,600,40);
    bool clear=true;
    for(const auto& c:tr.cars())clear&=std::hypot(c.x,c.z)>=90;
    check(!tr.cars().empty()&&clear,"initial traffic population never spawns inside player exclusion zone");
    // On a compact fully visible road there is nowhere safe to relocate.
    Way::Seg small;small.pts={{0,0,0},{0,0,100}};
    Traffic compact;compact.build({small},{});compact.setModels({0});compact.spawn(0,50,200,1);
    player.x=0;player.z=50;player.yaw=0;compact.setPlayer(player);
    const auto before=compact.cars().front();compact.update(1.f/60,500,500,10);
    const auto after=compact.cars().front();
    check(std::hypot(after.x-before.x,after.z-before.z)<1&&compact.stats().respawns==0,
          "failed out-of-view relocation keeps source instead of using visible fallback");
    Traffic follower;follower.build({road},{});follower.setModels({0});
    bool fast=false;
    for(unsigned seed=1;seed<5000&&!fast;++seed) {
        follower.setSeed(seed);follower.spawn(0,0,2000,1);
        fast=!follower.cars().empty()&&follower.cars()[0].speed>16;
    }
    check(fast,"constructed fast approaching traffic fixture");
    if(fast) {
        const auto c=follower.cars().front();const float angle=c.yaw*3.14159265358979f/180;
        const float centreGap=18+0.5f*(follower.bodyOf(c.model).length+player.length);
        player.x=c.x+std::sin(angle)*centreGap;player.z=c.z+std::cos(angle)*centreGap;
        follower.setPlayer(player);follower.update(1.f/60,0,0,0);
        check(follower.cars()[0].speed<c.speed,"AI brakes for player before obsolete 14m trigger");
        for(int frame=0;frame<600;++frame)follower.update(1.f/60,0,0,0);
        check(follower.cars()[0].speed<.1f,"AI comes to rest behind stopped player");
        check(follower.stats().respawns==0,"queued AI does not disappear in player view");
    }
    printf("RESULT traffic safety %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
