#include "script/ScriptRules.h"
#include <cstdio>
#include <set>
#include <dirent.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
using namespace ht2;
static int checks=0,failures=0;
static void check(bool pass,const std::string& name) {
    ++checks;failures+=!pass;printf("%s %s\n",pass?"PASS":"FAIL",name.c_str());
}
static GameData::NodeEvent event(const char* node,double x,double z) {
    GameData::NodeEvent e;e.id=node;e.wayNode=node;e.dirId=1;e.dirX=x;e.dirZ=z;e.text="message";return e;
}
static ScriptRoadSeg line(const char* a,const char* b) {
    ScriptRoadSeg s;s.nodeA=a;s.nodeB=b;s.width=10;s.xz={0,0,0,100};return s;
}
// Fixtures request load changes until the chosen one/two ambushes are eligible;
// distance itself must not arm them. Probability is verified separately below.
static void selectEligible(ScriptRuntime& rt) {
    for(int i=0;i<200;++i) {
        rt.onLoadStateChanged(true,true);
        if(std::all_of(rt.ambushes().begin(),rt.ambushes().end(),
                       [](const AmbushState& a){return a.armed;}))return;
    }
    throw std::runtime_error("fixture could not select eligible ambushes");
}
int main(int argc,char** argv) {
    const std::string data=argc>1?argv[1]:"original";
    GameData original;if(!original.load(data))return 1;
    for(bool load:{false,true})for(bool player:{false,true}) {
        GameData gd;
        for(const char* klass:{"MAFIA","POLICE","POLICE","MAFIA"}) {
            GameData::Ambush a;a.klass=klass;a.wayNode="point";gd.ambushes.push_back(a);
        }
        ScriptRuntime rt(gd);rt.setNodePositions({{"point",0,0}});EngineRandom oracle;
        ScriptWorldState st;st.px=1000;rt.update(st);
        check(std::none_of(rt.ambushes().begin(),rt.ambushes().end(),[](const AmbushState& a){return a.armed;}),
              "far distance alone does not arm any ambush");
        bool match=true;
        for(int change=0;change<64;++change) {
            rt.onLoadStateChanged(load,player);
            for(const auto& a:rt.ambushes()) {
                const bool allowed=a.def->klass=="POLICE"||(load&&player);
                const bool expected=allowed&&oracle.bounded(1000000)<300000;
                match&=a.armed==expected;
            }
        }
        check(match,"load-change marks use original threshold and conditional PRNG order");
        const auto flags=rt.ambushes();
        for(int frame=0;frame<1000;++frame)rt.update(st);
        bool stable=true;for(size_t i=0;i<flags.size();++i)stable&=rt.ambushes()[i].armed==flags[i].armed;
        check(stable,"ordinary frames preserve load-change eligibility");
    }
    {
        GameData gd;
        for(int kind=1;kind<=4;++kind) {
            GameData::Gate g;g.kindId=kind;g.tile="aa";g.prob=1;gd.gates.push_back(g);
        }
        auto other=gd.gates.front();other.tile="bb";gd.gates.push_back(other);
        ScriptRuntime rt(gd);ScriptWorldState st;st.hour=12;st.weather=0;
        rt.onModuleLoad("aa",st);
        check(rt.gates()[0].rolls==1&&rt.gates()[4].rolls==0,"module load samples only its own groups");
        rt.update(st);const auto visible=rt.coverage().gatesVisible;
        for(int i=0;i<2000;++i)rt.update(st);
        check(rt.coverage().gatesVisible==visible&&rt.gates()[0].rolls==1,"2000 frame ticks do not reroll people");
        st.hour=23;st.weather=.5;rt.update(st);
        check(rt.gates()[1].visible&&rt.gates()[3].visible,"people retain module-load condition snapshot");
        rt.onModuleLoad("aa",st);
        check(rt.gates()[0].visible&&!rt.gates()[1].visible&&!rt.gates()[2].visible&&!rt.gates()[3].visible,
              "night storm reload applies all four gate kinds");
        check(rt.gates()[0].rolls==2&&rt.gates()[4].rolls==1,"reload preserves unrelated module state");
        st.paused=true;rt.onModuleLoad("bb",st);
        check(rt.gates()[4].rolls==1,"paused load does not advance gate RNG");
        gd.gates[0].prob=0;ScriptRuntime zero(gd);st.paused=false;zero.onModuleLoad("aa",st);
        check(!zero.gates()[0].visible,"zero-probability gate is never visible");
    }
    {
        GameData gd;GameData::Ambush a;a.klass="POLICE";a.wayNode="point";gd.ambushes={a};
        ScriptRuntime rt(gd);rt.setNodePositions({{"point",0,0}});selectEligible(rt);
        ScriptWorldState st;st.px=300;rt.update(st);
        for(int change=0;change<200&&rt.ambushes()[0].armed;++change)rt.onLoadStateChanged(false,false);
        check(rt.ambushes()[0].active&&!rt.ambushes()[0].armed,
              "new load mark changes eligibility without dismissing active ambush");
        st.px=700;rt.update(st);
        check(!rt.ambushes()[0].active&&!rt.ambushes()[0].armed,
              "departing deactivates without restoring rejected eligibility");
        st.px=300;rt.update(st);
        check(rt.ambushes()[0].fires==1,"returning cannot reactivate an unmarked ambush");
    }
    for(uint32_t seed=1;seed<=25;++seed) {
        GameData gd;GameData::Obstacle o;o.id="timer";o.debrisA="point";gd.obstacles={o};
        ScriptRuntime rt(gd);rt.setSeed(seed);rt.setNodePositions({{"point",0,0}});
        ScriptWorldState st;st.px=1000;rt.update(st);
        const double due=rt.nextObstacleToggleTime();
        check(due>=600&&due<900,"initial obstacle timer range seed "+std::to_string(seed));
        st.time=due-.001;rt.update(st);
        check(!rt.obstacles()[0].closed,"obstacle cannot close before deadline seed "+std::to_string(seed));
        st.time=due;rt.update(st);
        check(rt.obstacles()[0].closed&&rt.nextObstacleToggleTime()>=due+600&&rt.nextObstacleToggleTime()<due+900,
              "obstacle closes on deadline and reschedules exclusively seed "+std::to_string(seed));
    }
    for(const auto& g:original.gates) {
        GameData fixture;fixture.gates={g};ScriptRuntime rt(fixture);
        ScriptWorldState st;st.hour=12;st.weather=0;rt.onModuleLoad(g.tile,st);
        const bool noon=rt.gates()[0].loaded&&rt.gates()[0].satisfied&&rt.gates()[0].rolls==1;
        st.hour=23;st.weather=.5;rt.onModuleLoad(g.tile,st);
        const bool unconditional=g.kindId==1;
        check(noon&&rt.gates()[0].satisfied==unconditional&&
              rt.gates()[0].rolls==(unconditional?2u:1u),g.tile+"/"+g.key+" authored people load conditions");
    }
    // Every shipped nodeevent must actually execute, not merely be counted.
    for(const auto& e:original.nodeEvents) {
        GameData gd;gd.nodeEvents={e};
        const double dx=e.dirId==0?1:e.dirX,dz=e.dirId==0?0:e.dirZ;
        auto s=line("source",e.wayNode.c_str());s.xz={0,0,dx*100,dz*100};
        ScriptRuntime rt(gd);rt.setRoadSegments({s});
        ScriptWorldState st;st.time=100;rt.update(st);
        st.time=101;st.px=dx*50;st.pz=dz*50;rt.update(st);
        st.time=102;st.px=dx*99;st.pz=dz*99;rt.update(st);
        check(rt.nodeEvents()[0].fires==1,e.id+" executes through road transition");
        if(e.dirId!=0) {
            s.nodeA=e.wayNode;s.nodeB="destination";
            ScriptRuntime atA(gd);atA.setRoadSegments({s});atA.setPreviousPosition(-dx,-dz);
            st.px=st.pz=0;atA.update(st);
            check(atA.nodeEvents()[0].fires==1,e.id+" keeps forward direction at nodeA");
            ScriptRuntime reverse(gd);reverse.setRoadSegments({s});
            st.px=dx*100;st.pz=dz*100;reverse.update(st);
            st.time+=1;st.px=dx*50;st.pz=dz*50;reverse.update(st);
            st.time+=1;st.px=dx;st.pz=dz;reverse.update(st);
            check(reverse.nodeEvents()[0].fires==0,e.id+" rejects reverse travel");
        }
    }
    {
        GameData gd;gd.nodeEvents={event("external",1,0)};
        ScriptRuntime rt(gd);rt.setRoadSegments({line("A","B")});
        ScriptWorldState st;st.time=100;st.px=50;st.pz=50;rt.update(st);
        st.px=60;st.time=101;rt.passNode("external");rt.update(st);
        check(rt.nodeEvents()[0].fires==1,"explicit pass retains movement with network attached");
        st.paused=true;st.time=102;rt.update(st);
        check(rt.pendingMessages().empty()&&rt.coverage().nodeEventsFired==0&&rt.nodeEvents()[0].fires==1,
              "pause clears frame pulses without changing persistent state");
    }
    {
        GameData gd;gd.nodeEvents={event("B",-1,0)};
        auto s=line("A","B");s.xz={0,0,100,0,100,100,0,100};
        ScriptRuntime rt(gd);rt.setRoadSegments({s});ScriptWorldState st;
        st.time=100;st.px=100;st.pz=50;rt.update(st);
        st.time=101;st.pz=100;st.px=50;rt.update(st);
        st.time=102;st.px=1;rt.update(st);
        check(rt.nodeEvents()[0].fires==1,"curved road preserves actual movement, not whole-link chord");
    }
    {
        GameData gd;gd.nodeEvents={event("B",0,1)};
        auto low=line("A","lower"),high=line("upper","B");
        low.heights={0,0};high.heights={15,15};
        ScriptRuntime rt(gd);rt.setRoadSegments({low,high});ScriptWorldState st;
        st.haveHeight=true;st.py=15;st.time=100;rt.update(st);
        st.pz=50;st.time=101;rt.update(st);st.pz=99;st.time=102;rt.update(st);
        check(rt.nodeEvents()[0].fires==1&&rt.currentRoadNode()=="B","overpass selects its own height tier");
    }
    {
        GameData gd;gd.nodeEvents={event("B",0,1)};auto s=line("A","B");s.width=20;s.widthEnd=4;
        ScriptRuntime rt(gd);rt.setRoadSegments({s});ScriptWorldState st;
        st.px=5;st.pz=99;rt.update(st);
        check(rt.currentRoadNode().empty(),"tapered road does not pass nodes outside final width");
    }
    {
        GameData gd;gd.nodeEvents={event("B",0,1)};auto s=line("A","B");s.xz={0,0,0,5};
        ScriptRuntime rt(gd);rt.setRoadSegments({s});rt.setPreviousPosition(0,4);
        ScriptWorldState st;st.pz=5;rt.update(st);
        check(rt.nodeEvents()[0].fires==1,"short links choose nearest endpoint in overlapping pass zones");
    }
    {
        GameData gd;gd.nodeEvents={event("point",0,1)};ScriptRuntime rt(gd);
        rt.setNodePositions({{"point",0,0}});ScriptWorldState st;st.time=100;rt.update(st);
        st.havePosition=false;rt.update(st);st.havePosition=true;st.pz=-1;st.time=101;rt.update(st);
        check(rt.radiusPasses()==2,"losing player position invalidates previous radius node");
    }
    {
        GameData gd;GameData::Obstacle o;o.id="closure";o.debrisA="point";o.msgClose="closed";o.msgOpen="opened";
        gd.obstacles={o};ScriptRuntime rt(gd);ScriptWorldState st;st.time=1000;st.px=1000;
        rt.update(st);check(!rt.obstacles()[0].closed,"unresolved obstacle cannot be scheduled");
        rt.setNodePositions({{"point",0,0}});st.havePosition=false;st.time=2000;rt.update(st);
        check(!rt.obstacles()[0].closed,"obstacles require a player position");
        st.havePosition=true;st.px=500;rt.update(st);
        check(!rt.obstacles()[0].closed,"exactly 500m does not satisfy strict distance guard");
        st.px=501;st.time=4000;rt.update(st);
        check(rt.obstacles()[0].closed&&rt.obstacles()[0].changed,"closure exposes a single transition pulse");
        st.paused=true;rt.update(st);
        check(rt.obstacles()[0].closed&&!rt.obstacles()[0].changed,"pause does not repeat closure message");
        rt.setNodePositions({});check(!rt.obstacles()[0].havePos&&!rt.obstacles()[0].closed,"rebinding discards stale obstacle coordinates");
        ScriptRuntime upper(gd);upper.setNodePositions({{"point",0,0,1000}});
        st.paused=false;st.haveHeight=true;st.py=0;st.px=0;upper.update(st);
        check(upper.obstacles()[0].closed,"obstacle player distance is three-dimensional");
    }
    {
        GameData gd;GameData::Ambush a;a.id="ambush";a.klass="POLICE";a.wayNode="point";gd.ambushes={a};
        ScriptRuntime rt(gd);rt.setNodePositions({{"point",0,0,1000}});ScriptWorldState st;
        st.haveHeight=true;st.py=0;rt.update(st);
        selectEligible(rt);
        check(rt.ambushes()[0].armed&&!rt.ambushes()[0].active,"ambush band uses vertical distance");
        st.py=700;rt.update(st);check(rt.ambushes()[0].fires==1,"3D ambush band executes");
        st.py=400;rt.update(st);
        check(rt.ambushes()[0].active,"exact far boundary 600 keeps active ambush");
        st.py=399.99;rt.update(st);
        check(!rt.ambushes()[0].active&&rt.ambushes()[0].armed,
              "beyond exact far boundary deactivates ambush");
        rt.setNodePositions({});check(!rt.ambushes()[0].havePos&&!rt.ambushes()[0].active&&!rt.ambushes()[0].armed,
                                     "rebinding invalidates old ambush state");
    }
    {
        GameData gd;GameData::Ambush a;a.id="directed";a.klass="POLICE";a.wayNode="target";a.dir="NORTH";
        gd.ambushes={a};
        ScriptRoadSeg road;road.nodeA="start";road.nodeB="target";road.width=10;
        road.xz={0,-1000,0,0,0,1000};road.heights={0,0,0};
        auto executes=[&](double x,double z,double y,std::vector<ScriptRoadSeg> roads) {
            ScriptRuntime rt(gd);rt.setNodePositions({{"target",0,1000,0}});rt.setRoadSegments(roads);
            selectEligible(rt);
            ScriptWorldState st;st.haveHeight=true;st.py=0;st.pz=-1000;rt.update(st);
            st.px=x;st.pz=z;st.py=y;rt.update(st);return rt.ambushes()[0].active;
        };
        check(executes(0,700,0,{road}),"ambush activates on connected authored approach");
        auto extension=road;extension.nodeA="target";extension.nodeB="beyond";
        extension.xz={0,1000,0,2000};extension.heights={0,0};
        check(!executes(0,1300,0,{road,extension}),"ambush rejects connected opposite approach");
        check(!executes(30,700,0,{road}),"ambush rejects grass inside radial band");
        check(!executes(0,700,10,{road}),"ambush rejects unrelated height layer");
        auto isolated=road;isolated.nodeA="isolated";isolated.nodeB="deadend";
        isolated.xz={100,500,100,800};isolated.heights={0,0};
        check(!executes(100,700,0,{road,isolated}),"ambush rejects disconnected road inside radial band");
        auto bridge=isolated;bridge.xz={0,500,0,800};bridge.heights={20,20};
        check(!executes(0,700,20,{road,bridge}),"coincident bridge cannot connect to lower ambush");
        auto aliases=road;aliases.nodeB="canonical";aliases.aliasesB={"target"};
        check(executes(0,700,0,{aliases}),"ambush resolves author RNOD endpoint alias");
        auto reversed=road;std::reverse(reversed.heights.begin(),reversed.heights.end());
        reversed.xz={0,1000,0,0,0,-1000};reversed.nodeA="target";reversed.nodeB="start";
        check(executes(0,700,0,{reversed}),"RSEG storage direction does not invert approach");
        auto curve=road;curve.xz={0,2000,-300,500,0,1000};
        check(executes(-100,1500,0,{curve}),"hairpin approach uses incoming tangent not player-bearing");
        auto transverse=road;transverse.xz={-1000,1000,0,1000};transverse.heights={0,0};
        check(!executes(-300,1000,0,{transverse}),"perpendicular tangent fails strict positive dot");
        auto unnamed=road;unnamed.nodeB="unknown";
        check(executes(0,700,0,{unnamed}),"ambush anchor projection does not require named RSEG endpoint");
    }
    {
        GameData gd;GameData::Ambush a,b;a.id="A";a.wayNode="A";b.id="B";b.wayNode="B";
        a.klass=b.klass="POLICE";
        b.next={"A","missing"};gd.ambushes={a,b};
        ScriptRuntime rt(gd);rt.setNodePositions({{"A",0,0},{"B",500,0}});
        selectEligible(rt);
        ScriptWorldState st;st.px=-1000;rt.update(st);st.px=-300;rt.update(st);
        check(rt.ambushes()[0].active&&!rt.ambushes()[1].active,"next does not auto-activate distant partner");
        st.px=150;rt.update(st);
        check(!rt.ambushes()[0].active&&rt.ambushes()[1].active,
              "next deactivates active partner no longer armable");
        check(rt.ambushes()[0].fires==1&&rt.ambushes()[1].fires==1,
              "next preserves fire history without reactivation pulse");
        ScriptRuntime overlap(gd);overlap.setNodePositions({{"A",0,0},{"B",50,0}});
        selectEligible(overlap);
        st.px=-1000;overlap.update(st);st.px=-300;overlap.update(st);
        check(overlap.ambushes()[0].active&&overlap.ambushes()[1].active,
              "next preserves still-armable active partner");
    }
    // The same adapter App uses must attach every authored event to an RSEG.
    std::vector<Way::Seg> roads;std::vector<ScriptNodePos> nodes;
    DIR* dir=opendir((data+"/ENV").c_str());if(!dir)return 1;
    std::vector<std::string> paths;
    while(auto* e=readdir(dir)) { std::string n=e->d_name;if(n.size()>4&&n.substr(n.size()-4)==".way")paths.push_back(n); }
    closedir(dir);std::sort(paths.begin(),paths.end());
    for(const auto& p:paths) { Way w;if(!w.load(data+"/ENV/"+p))return 1;
        roads.insert(roads.end(),w.segs.begin(),w.segs.end());
        for(const auto& r:w.rooms)for(const auto& n:r.nodes)nodes.push_back({n.name,n.pos.x,n.pos.z,n.pos.y});
    }
    {
        ScriptRuntime rt(original);rt.setNodePositions(nodes);
        rt.setRoadSegments(scriptRoadSegments(roads,nodes));
        check(rt.ambushRoadAnchors()==original.ambushes.size(),
              "all authored ambush anchors resolve on actual RSEG graph ("+
              std::to_string(rt.ambushRoadAnchors())+"/"+std::to_string(original.ambushes.size())+")");
    }
    for(const auto& a:original.ambushes) {
        GameData fixture;fixture.ambushes={a};ScriptRuntime rt(fixture);rt.setNodePositions(nodes);
        selectEligible(rt);
        const auto* p=rt.nodePos(a.wayNode);bool pass=false;
        if(p) {
            ScriptWorldState st;st.haveHeight=true;st.py=p->y;st.pz=p->z;
            st.px=p->x+700;rt.update(st);st.px=p->x+300;rt.update(st);
            const bool first=rt.ambushes()[0].fires==1;
            rt.update(st);const bool stable=rt.ambushes()[0].fires==1;
            st.px=p->x;rt.update(st);
            check(rt.ambushes()[0].active&&rt.ambushes()[0].fires==1,
                  a.id+" active ambush persists inside near bound");
            // Decimal authored coordinates need not represent p.x+600 exactly
            // in binary. Test the inside edge here; exact 600 is tested above.
            st.px=p->x+599.999999;rt.update(st);
            check(rt.ambushes()[0].active,a.id+" inside far boundary remains active");
            st.px=p->x+600.01;rt.update(st);
            check(!rt.ambushes()[0].active&&rt.ambushes()[0].armed,
                  a.id+" beyond 600 deactivates and rearms");
            st.px=p->x+300;rt.update(st);
            pass=first&&stable&&rt.ambushes()[0].fires==2;
        }
        check(pass,a.id+" authored ambush arms, fires once and rearms");
    }
    const auto actualRoads=scriptRoadSegments(roads,nodes);
    for(const auto& a:original.ambushes) {
        GameData fixture;fixture.ambushes={a};ScriptRuntime rt(fixture);
        rt.setNodePositions(nodes);rt.setRoadSegments(actualRoads);
        selectEligible(rt);
        const auto* anchor=rt.nodePos(a.wayNode);bool found=false;
        if(anchor) {
            ScriptWorldState st;st.haveHeight=true;st.px=anchor->x+1000;st.pz=anchor->z;st.py=anchor->y;rt.update(st);
            for(const auto& road:actualRoads) {
                const size_t n=road.xz.size()/2;
                for(size_t i=1;i<n&&!found;++i)for(int sample=0;sample<=10&&!found;++sample) {
                    const double t=sample/10.0;
                    st.px=road.xz[2*i-2]+t*(road.xz[2*i]-road.xz[2*i-2]);
                    st.pz=road.xz[2*i-1]+t*(road.xz[2*i+1]-road.xz[2*i-1]);
                    st.py=road.heights.size()==n?road.heights[i-1]+t*(road.heights[i]-road.heights[i-1]):anchor->y;
                    const double distance=std::sqrt((st.px-anchor->x)*(st.px-anchor->x)+
                        (st.pz-anchor->z)*(st.pz-anchor->z)+(st.py-anchor->y)*(st.py-anchor->y));
                    if(distance<250||distance>600)continue;
                    rt.update(st);found=rt.ambushes()[0].active;
                }
                if(found)break;
            }
        }
        check(found,a.id+" has executable approach on actual authored RSEG network");
        if(!found&&anchor) {
            struct Near {double distance;const ScriptRoadSeg* road;size_t i;};
            std::vector<Near> near;
            for(const auto& road:actualRoads) {
                const size_t n=road.xz.size()/2;
                for(size_t i=1;i<n;++i) {
                    double x=road.xz[2*i-2],z=road.xz[2*i-1],y=road.heights.size()==n?road.heights[i-1]:0;
                    double dx=road.xz[2*i]-x,dz=road.xz[2*i+1]-z,dy=road.heights.size()==n?road.heights[i]-y:0;
                    const double l=dx*dx+dz*dz+dy*dy;if(l<1e-12)continue;
                    const double t=std::clamp(((anchor->x-x)*dx+(anchor->z-z)*dz+(anchor->y-y)*dy)/l,0.0,1.0);
                    near.push_back({std::sqrt(std::pow(anchor->x-x-t*dx,2)+std::pow(anchor->z-z-t*dz,2)+std::pow(anchor->y-y-t*dy,2)),&road,i});
                }
            }
            std::sort(near.begin(),near.end(),[](const Near& x,const Near& y){return x.distance<y.distance;});
            printf("UNARMABLE %s %s anchor %.3f %.3f %.3f\n",a.id.c_str(),a.dir.c_str(),anchor->x,anchor->z,anchor->y);
            for(size_t j=0;j<std::min<size_t>(4,near.size());++j) {
                const auto& p=near[j];const auto& r=*p.road;size_t i=p.i;
                printf("  nearest %.3f: %.3f %.3f -> %.3f %.3f\n",p.distance,r.xz[2*i-2],r.xz[2*i-1],r.xz[2*i],r.xz[2*i+1]);
            }
        }
    }
    for(const auto& o:original.obstacles) {
        GameData fixture;fixture.obstacles={o};ScriptRuntime rt(fixture);rt.setNodePositions(nodes);
        const auto& state=rt.obstacles()[0];ScriptWorldState st;st.time=1000;st.haveHeight=true;
        st.px=state.x+501;st.py=state.y;st.pz=state.z;rt.update(st);
        check(rt.obstacles()[0].havePos&&rt.obstacles()[0].closed&&rt.obstacles()[0].changed,
              o.id+" authored obstacle resolves and closes");
    }
    const auto converted=scriptRoadSegments(roads,nodes);std::set<std::string> bound;
    for(const auto& s:converted) {
        bound.insert(s.nodeA);bound.insert(s.nodeB);
        bound.insert(s.aliasesA.begin(),s.aliasesA.end());
        bound.insert(s.aliasesB.begin(),s.aliasesB.end());
    }
    size_t resolved=0;
    for(const auto& e:original.nodeEvents) {
        const bool found=bound.count(e.wayNode);resolved+=found;
        if(!found)for(const auto& n:nodes)if(n.name==e.wayNode) {
            double best=1e30;std::string nearest;
            for(const auto& s:roads)for(const auto& p:s.pts) {
                const double dx=p.x-n.x,dy=p.y-n.y,dz=p.z-n.z;
                const double d=dx*dx+dy*dy+dz*dz;
                if(d<best) {best=d;nearest=s.name;}
            }
            printf("UNBOUND %s at %.3f %.3f %.3f nearest %s distance %.3f\n",n.name.c_str(),n.x,n.y,n.z,nearest.c_str(),std::sqrt(best));
        }
        check(found,e.id+" bound to actual named RSEG endpoint");
        bool fired=false;
        for(const auto& s:converted) {
            for(int end=0;end<2&&!fired;++end) {
                const auto& names=end?s.aliasesB:s.aliasesA;
                if(std::find(names.begin(),names.end(),e.wayNode)==names.end())continue;
                const size_t n=s.xz.size()/2,i=end?n-1:0,j=end?n-2:1;
                double dx=s.xz[2*i]-s.xz[2*j],dz=s.xz[2*i+1]-s.xz[2*j+1];
                const double len=std::hypot(dx,dz);
                if(len<1e-6)continue;
                // Pass with the authored heading, including transverse
                // loading-yard crossings, just like App's initial pose feed.
                if(e.dirId!=0) {dx=e.dirX*len;dz=e.dirZ*len;}
                GameData fixture;fixture.nodeEvents={e};ScriptRuntime rt(fixture);rt.setRoadSegments({s});
                ScriptWorldState st;st.time=100;st.haveHeight=true;
                st.px=s.xz[2*i];st.pz=s.xz[2*i+1];st.py=s.heights[i];
                rt.setPreviousPosition(st.px-dx/len,st.pz-dz/len);rt.update(st);
                fired=rt.nodeEvents()[0].fires==1;
            }
            if(fired)break;
        }
        check(fired,e.id+" executes on its authored RSEG geometry");
    }
    printf("RESULT script runtime %d checks, %d failures, %zu/%zu authored events attached, %zu road links\n",
           checks,failures,resolved,original.nodeEvents.size(),converted.size());
    return failures?1:0;
}
