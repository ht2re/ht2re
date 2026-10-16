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
    // The initial population is the traffic that surrounds the player: it may
    // (and must) be close, but a car must never be dropped on top of the
    // player's body.  The 90 m streaming distance (kRelocateMin) belongs to
    // relocate()/inView(), not to spawn() - applying it here emptied the roads
    // around the player and hid the traffic (0.009% of the frame instead of
    // ~0.5%).
    Traffic::Body pb;pb.radius=player.radius;pb.length=player.length;
    bool overlap=false,nearby=false;
    for(const auto& c:tr.cars()){
        float nx=0,nz=0;
        if(Collision::bodyOverlap(c.x,c.z,c.yaw,tr.bodyOf(c.model),
                                  player.x,player.z,player.yaw,pb,nx,nz)>0.0f)overlap=true;
        if(std::hypot(c.x-player.x,c.z-player.z)<90.0f)nearby=true;
    }
    check(!tr.cars().empty()&&!overlap&&nearby,
          "initial traffic population surrounds the player without overlapping its body");
    // On a compact fully visible road there is nowhere safe to relocate.
    Way::Seg small;small.pts={{0,0,0},{0,0,100}};
    Traffic compact;compact.build({small},{});compact.setModels({0});compact.spawn(0,50,200,1);
    player.x=0;player.z=50;player.yaw=0;compact.setPlayer(player);
    const auto before=compact.cars().front();compact.update(1.f/60,500,500,10);
    const auto after=compact.cars().front();
    check(std::hypot(after.x-before.x,after.z-before.z)<1&&compact.stats().respawns==0,
          "failed out-of-view relocation keeps source instead of using visible fallback");
    Traffic follower;follower.build({road},{});follower.setModels({0});
    bool fast=false;unsigned fastSeed=0;
    for(unsigned seed=1;seed<5000&&!fast;++seed) {
        follower.setSeed(seed);follower.spawn(0,0,2000,1);
        fast=!follower.cars().empty()&&follower.cars()[0].speed>16;
        if(fast)fastSeed=seed;
    }
    check(fast,"constructed fast approaching traffic fixture");
    if(fast) {
        Traffic crossing=follower, impact=follower, highTruck=follower;
        const auto c=follower.cars().front();const float angle=c.yaw*3.14159265358979f/180;
        const float centreGap=18+0.5f*(follower.bodyOf(c.model).length+player.length);
        player.x=c.x+std::sin(angle)*centreGap;player.z=c.z+std::cos(angle)*centreGap;
        follower.setPlayer(player);follower.update(1.f/60,0,0,0);
        check(follower.cars()[0].speed<c.speed,"AI brakes for player before obsolete 14m trigger");
        for(int frame=0;frame<600;++frame)follower.update(1.f/60,0,0,0);
        check(follower.cars()[0].speed<.1f,"AI comes to rest behind stopped player");
        check(follower.stats().respawns==0,"queued AI does not disappear in player view");

        player.h=c.h+8;
        crossing.setPlayer(player);crossing.update(1.f/60,0,0,0);
        check(crossing.cars()[0].speed>=c.speed,"AI does not brake for player on overpass");
        player.x=c.x;player.z=c.z;crossing.setPlayer(player);
        crossing.update(1.f/60,0,0,0);
        check(!crossing.playerHit().hit&&crossing.stats().playerContacts==0,
              "overlapping XZ bodies on different levels do not push or damage player");

        player.h=c.h;
        player.x=c.x+std::sin(angle)*(0.5f*(impact.bodyOf(c.model).length+player.length)+.3f);
        player.z=c.z+std::cos(angle)*(0.5f*(impact.bodyOf(c.model).length+player.length)+.3f);
        // 20 Hz: first bumper contact, before the two capsule axes overlap.
        impact.setPlayer(player);impact.update(.05f,0,0,0);
        printf("impact fixture: before %.2f %.2f yaw %.1f speed %.2f; after %.2f %.2f speed %.2f; player %.2f %.2f hit %d damage %.3f\n",
               c.x,c.z,c.yaw,c.speed,impact.cars()[0].x,impact.cars()[0].z,impact.cars()[0].speed,
               player.x,player.z,impact.playerHit().hit,impact.playerHit().damage);
        check(impact.playerHit().hit&&impact.playerHit().damage>0,
              "same-level impact uses integrated pose in this tick, not previous frame");

        // Model height, not a fixed road-height threshold, determines contact:
        // a tall AI truck can touch a low elevated body while a sedan cannot.
        highTruck.setBody(c.model,highTruck.bodyOf(c.model).length,highTruck.bodyOf(c.model).radius,5);
        player.x=c.x;player.z=c.z;player.h=c.h+3;
        highTruck.setPlayer(player);highTruck.update(1.f/60,0,0,0);
        check(highTruck.playerHit().hit,"model bbox height preserves vertically overlapping contacts");
    }
    Way::Seg lower;lower.width=8;lower.pts={{0,0,0},{0,0,8}};
    Way::Seg upper=lower;for(auto& p:upper.pts)p.y=8;
    Traffic layers;layers.setModels({0});layers.setLaneOffset(0);
    bool projectedOverlap=false;
    for(unsigned seed=1;seed<100&&!projectedOverlap;++seed) {
        layers.setSeed(seed);layers.build({lower,upper},{});layers.spawn(0,4,100,2);
        if(layers.cars().size()!=2)continue;
        const auto& a=layers.cars()[0];const auto& b=layers.cars()[1];float nx,nz;
        projectedOverlap=std::fabs(a.h-b.h)>7 && Collision::bodyOverlap(
            a.x,a.z,a.yaw,layers.bodyOf(0),b.x,b.z,b.yaw,layers.bodyOf(0),nx,nz)>0;
    }
    check(projectedOverlap,"spawn supports XZ-overlapping traffic on separate road decks");
    if(projectedOverlap) {
        layers.update(1.f/60,0,4,0);
        check(layers.stats().overlapFrames==0&&layers.stats().damageDealt==0,
              "AI pairs on separate decks do not enter capsule contact solver");
    }
    Way::Seg ramp=road;for(auto& p:ramp.pts)p.y=p.z*.25;
    Traffic uphill;uphill.build({ramp},{});uphill.setModels({0});
    uphill.setSeed(fastSeed);uphill.spawn(0,0,2000,1);
    check(!uphill.cars().empty(),"constructed graded road fixture");
    if(!uphill.cars().empty()) {
        const auto c=uphill.cars()[0];const float a=c.yaw*3.14159265358979f/180;
        Traffic::Player leader;leader.valid=true;
        leader.x=c.x+std::sin(a)*24;leader.z=c.z+std::cos(a)*24;leader.h=leader.z*.25;
        uphill.setPlayer(leader);uphill.update(1.f/60,0,0,0);
        check(uphill.cars()[0].speed<c.speed,"AI brakes for same-road leader several metres higher or lower on ramp");
    }
    printf("RESULT traffic safety %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
