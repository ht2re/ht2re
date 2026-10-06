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
    check("lawn below bridge stays on its own level",
          std::fabs(ground.drivableY(100,0,-5,&source)+5)<.001f&&source==3);
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
    // A large lawn quad has no interior vertices. A low unrelated mesh used to
    // pull the coarse grid below its texture, and the nearby graph masked it.
    GroundMap lawn;
    auto grass=plane(500,30,"grass1");
    std::vector<B3D::SubMesh> lawnMeshes={grass,plane(600,0,"mat_roof")};
    lawn.build(lawnMeshes); lawn.buildRoadSurface(lawnMeshes);
    lawn.setRoadSegments({-400,0,12,0,400,0,12,0});
    check("lawn follows its actual triangle, not nearby waypoints",
          std::fabs(lawn.drivableY(100,5,30,&source)-30)<.001f&&source==3);
    check("lawn interior cannot use low smoothed vertex field",
          std::fabs(lawn.drivableY(100,100,30,&source)-30)<.001f&&source==3);
    check("roof and water are not ground materials",
          !RoadSurface::isGroundMaterial("mat_roof")&&!RoadSurface::isGroundMaterial("mat_water"));
    check("poster substring and concrete building parts are not ground",
          !RoadSurface::isGroundMaterial("mat_poster_tile")&&
          !RoadSurface::isGroundMaterial("churchb_beton")&&
          !RoadSurface::isGroundMaterial("mat_side_beton_bridge"));
    for(size_t i=0;i<grass.pos.size();i+=3)grass.pos[i+2]=30+.1f*grass.pos[i];
    lawnMeshes={grass,plane(600,0,"mat_roof")};
    lawn.build(lawnMeshes); lawn.buildRoadSurface(lawnMeshes);
    Vehicle onGrass; onGrass.setGround(&lawn); onGrass.setRoads(&lawn);
    onGrass.reset(0,50,90); onGrass.setHeightRef(30);
    for(int i=0;i<300;++i)onGrass.update(idle,1.f/60);
    bool above=true; float maxRoll=0;
    for(int i=0;i<600;++i) {
        onGrass.update(drive,1.f/60);
        above &= onGrass.y()+.001f >= 30+.1f*onGrass.x()+Vehicle::RIDE_HEIGHT;
        maxRoll=std::max(maxRoll,std::fabs(onGrass.roll()));
    }
    check("uphill chassis never sinks under lawn triangles",above&&onGrass.odometer()>20);
    check("planar lawn does not create spurious sideways roll",maxRoll<.01f);
    auto asphalt=plane(100,20,"mat_road");
    auto verge=plane(100,20,"mat_grass");
    for(size_t i=0;i<asphalt.pos.size();i+=3)asphalt.pos[i]=asphalt.pos[i]*.5f-50;
    for(size_t i=0;i<verge.pos.size();i+=3) {
        verge.pos[i]=verge.pos[i]*.5f+50;
        verge.pos[i+2]=20+.1f*verge.pos[i];
    }
    lawnMeshes={asphalt,verge,plane(150,0,"mat_roof")};
    lawn.build(lawnMeshes); lawn.buildRoadSurface(lawnMeshes);
    lawn.setRoadSegments({-90,0,17,0,90,0,17,0});
    Vehicle exitRoad; exitRoad.setGround(&lawn); exitRoad.setRoads(&lawn);
    exitRoad.reset(-8,50,90); bool exitSafe=true,exitBlocked=false;
    float exitRoll=0;
    for(int i=0;i<600;++i) {
        exitRoad.update(drive,1.f/60);
        exitSafe &= exitRoad.y()+.001f>=20+.1f*std::max(0.f,exitRoad.x())+Vehicle::RIDE_HEIGHT;
        exitBlocked |= exitRoad.blocked(); exitRoll=std::max(exitRoll,std::fabs(exitRoad.roll()));
    }
    check("road-to-lawn transition has no invisible waypoint wall",!exitBlocked&&exitRoad.x()>5);
    check("road-to-lawn transition keeps body above rendered surface",exitSafe);
    check("road-to-lawn transition does not tip cabin sideways",exitRoll<.01f);
    GroundMap cut;
    auto steep=plane(10,30,"mat_grass");
    for(size_t i=0;i<steep.pos.size();i+=3)steep.pos[i+2]=30+2*steep.pos[i];
    cut.build({steep});cut.buildRoadSurface({steep});
    check("natural slopes over 60 degrees remain solid ground",
          std::fabs(cut.drivableY(2,3,34,&source)-34)<.001f&&source==3);
    auto patch=plane(.75f,30,"mat_grass");
    cut.build({patch,plane(50,0,"mat_roof")});cut.buildRoadSurface({patch});
    check("small lawn patches do not disappear from contact index",
          std::fabs(cut.drivableY(.1f,.2f,30,&source)-30)<.001f&&source==3);
    auto edge=plane(100,30,"mat_grass");
    for(size_t i=0;i<edge.pos.size();i+=3)edge.pos[i]=edge.pos[i]*.5f+50;
    cut.build({edge,plane(200,20,"mat_roof")});cut.buildRoadSurface({edge});
    const float nearEdge=cut.drivableY(-.001f,50,30,&source);
    check("coarse fallback meets authored lawn edge continuously",
          std::fabs(nearEdge-30)<.002f&&source==3);
    Vehicle join;join.setGround(&cut);join.setRoads(&cut);join.reset(-8,50,90);
    bool joinBlocked=false,joinAbove=true;
    for(int i=0;i<600;++i) {
        join.update(drive,1.f/60);joinBlocked|=join.blocked();
        if(join.x()>=0)joinAbove&=join.y()+.001f>=31;
    }
    check("coarse-to-lawn seam is not an invisible climb wall",!joinBlocked&&join.x()>5&&joinAbove);
    auto lower=plane(100,30,"mat_grass"),upper=plane(100,45,"mat_grass");
    for(size_t i=0;i<lower.pos.size();i+=3)lower.pos[i]=lower.pos[i]*.5f-49.5f;
    for(size_t i=0;i<upper.pos.size();i+=3)upper.pos[i]=upper.pos[i]*.5f+50.5f;
    cut.build({lower,upper});cut.buildRoadSurface({lower,upper});
    Vehicle tier;tier.setGround(&cut);tier.setRoads(&cut);tier.reset(0,50,0);
    for(int i=0;i<300;++i)tier.update(idle,1.f/60);
    check("unreachable neighbouring tier cannot tip cabin sideways",std::fabs(tier.roll())<.01f);
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
