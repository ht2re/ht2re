#include "world/GroundMap.h"
#include "game/Collision.h"
#include "game/Vehicle.h"
#include "ui/Map.h"
#include <cstdio>
#include <cmath>
using namespace ht2;
static B3D::SubMesh plane(float span,float h,const char* material) {
    B3D::SubMesh m; m.material=material;
    m.pos={-span,-span,h,span,-span,h,span,span,h,-span,span,h};
    m.idx={0,1,2,0,2,3}; return m;
}
static B3D::SubMesh wall(float x,float lo,float hi) {
    B3D::SubMesh m; m.material="mat_wall";
    m.pos={x,-10,lo,x,10,lo,x,10,hi,x,-10,hi};
    m.idx={0,1,2,0,2,3}; return m;
}
int main() {
    int tests=0,fail=0;
    auto check=[&](const char* name,bool pass) {
        ++tests; fail+=!pass; std::printf("%s %s\n",pass?"PASS":"FAIL",name);
    };
    RoadSurface roads;
    roads.build({plane(1000,20,"mat_road")});
    float h,d; bool visible;
    check("wide asphalt centre is covered",roads.heightAt(0,50,20,12,h,d,visible));
    check("inside triangle distance is zero",d==0 && std::fabs(h-20)<.001f);
    check("wide asphalt interior is not an edge blend",roads.heightAt(700,50,20,12,h,d,visible)&&d==0);
    roads.build({plane(1000,0,"mat_road"),plane(1000,15,"mat_road")});
    check("underpass selects lower tier",roads.heightAt(10,50,0,12,h,d,visible)&&h==0&&!visible);
    check("deck selects upper tier",roads.heightAt(10,50,15,12,h,d,visible)&&h==15&&visible);
    check("spawn reference uses bounded fallback",roads.heightAt(10,50,29,12,h,d,visible)&&h==15);
    check("distant vertical tier is rejected",!roads.heightAt(10,50,100,12,h,d,visible));
    GroundMap ground;
    std::vector<B3D::SubMesh> meshes={plane(2000,-5,"mat_grass"),plane(1000,20,"mat_road"),
                                     plane(20,-5,"mat_water"),plane(1500,-5,"mat_water")};
    ground.build(meshes); ground.buildRoadSurface(meshes);
    ground.setRoadSegments({-900,0,25,0,900,0,25,0});
    int source=0;
    check("actual asphalt overrides different graph height",std::fabs(ground.drivableY(100,0,25,&source)-20)<.001f&&source==2);
    check("2D water flag exists below bridge",ground.waterAt(20,20));
    check("bridge deck is not blocked by water below",!ground.waterAt(20,20,20));
    check("open water still blocks driving",ground.waterAt(1500,1500,-5));
    Vehicle v; v.setGround(&ground); v.setRoads(&ground); v.reset(100,0,90); v.setHeightRef(20);
    const float off=v.surfaceY(1500,0,20);
    bool stable=true;
    for (int i=0;i<50;++i) {
        v.surfaceY(1500+i,20,20);
        stable &= std::fabs(v.surfaceY(1500,0,20)-off)<.001f;
    }
    check("probe queries never mutate height reference",stable);
    Vehicle::Input idle;
    for(int i=0;i<300;++i) v.update(idle,1.f/60);
    Vehicle::Input drive; drive.throttle=1;
    float maxStep=0,prev=v.y(); bool blocked=false;
    for(int i=0;i<600;++i) {
        v.update(drive,1.f/60); maxStep=std::max(maxStep,std::fabs(v.y()-prev));
        prev=v.y(); blocked |= v.blocked();
    }
    check("straight asphalt does not bounce",maxStep<.001f);
    check("straight asphalt has no false climb wall",!blocked&&v.odometer()>20);
    Collision c; c.build({wall(0,12,15),wall(10,-6,-2)});
    check("legacy 2D query sees overhead wall",c.blocked(0,0,.5f));
    check("height query clears overhead geometry",!c.blocked(0,0,.5f,.05f,4));
    check("height query clears geometry below road",!c.blocked(10,0,.5f,.05f,4));
    float ox,oz;
    check("sweep passes under overhead wall",c.resolveMove(-5,0,5,0,.5f,ox,oz,.05f,4)&&ox==5);
    c.build({wall(0,0,5)});
    check("real wall remains solid",c.blocked(0,0,.5f,.05f,4));
    check("height sweep cannot tunnel through wall",!c.resolveMove(-5,0,5,0,.5f,ox,oz,.05f,4)&&ox<0);
    check("resolved circle stays outside wall",!c.blocked(ox,oz,.5f,.05f,4));
    B3D::SubMesh slope; slope.material="mat_wall";
    slope.pos={0,-10,-4,10,-10,16,10,10,16,0,10,-4};
    slope.idx={0,1,2,0,2,3}; c.build({slope});
    check("sloped wall below body does not block its whole footprint",!c.blocked(1,0,.5f,10,14));
    check("sloped wall at body height still blocks",c.blocked(8,0,.5f,10,14));
    for(size_t i=0;i<slope.pos.size();i+=3) { slope.pos[i]+=5500; slope.pos[i+1]-=5500; }
    c.build({slope});
    check("height clipping is stable at island-scale coordinates",c.blocked(5508,-5500,.5f,10,14)&&!c.blocked(5501,-5500,.5f,10,14));
    c.clear(); check("clear also removes height geometry",!c.blocked(0,0,.5f,0,4));
    check("map positive Z is above player",Map::verticalOffset(100,0,1)==-100);
    check("map negative Z is below player",Map::verticalOffset(-100,0,1)==100);
    const float direction[4][2]={{0,-1},{1,0},{0,1},{-1,0}};
    for(int i=0;i<4;++i) {
        const float a=Map::headingRadians(i*90.f);
        check("map arrow matches reflected world direction",
              std::fabs(std::sin(a)-direction[i][0])<.001f &&
              std::fabs(-std::cos(a)-direction[i][1])<.001f);
    }
    std::printf("RESULT road safety %d checks, %d failures, flat maxstep %.6f m\n",tests,fail,maxStep);
    return fail?1:0;
}
