#include "script/ScriptRules.h"
#include <cstdio>
#include <set>
#include <dirent.h>
#include <algorithm>
#include <cmath>
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
int main(int argc,char** argv) {
    const std::string data=argc>1?argv[1]:"original";
    GameData original;if(!original.load(data))return 1;
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
        GameData gd;GameData::Ambush a;a.id="ambush";a.wayNode="point";gd.ambushes={a};
        ScriptRuntime rt(gd);rt.setNodePositions({{"point",0,0,1000}});ScriptWorldState st;
        st.haveHeight=true;st.py=0;rt.update(st);
        check(rt.ambushes()[0].armed&&!rt.ambushes()[0].active,"ambush band uses vertical distance");
        st.py=700;rt.update(st);check(rt.ambushes()[0].fires==1,"3D ambush band executes");
        rt.setNodePositions({});check(!rt.ambushes()[0].havePos&&!rt.ambushes()[0].active&&!rt.ambushes()[0].armed,
                                     "rebinding invalidates old ambush state");
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
    for(const auto& a:original.ambushes) {
        GameData fixture;fixture.ambushes={a};ScriptRuntime rt(fixture);rt.setNodePositions(nodes);
        const auto* p=rt.nodePos(a.wayNode);bool pass=false;
        if(p) {
            ScriptWorldState st;st.haveHeight=true;st.py=p->y;st.pz=p->z;
            st.px=p->x+700;rt.update(st);st.px=p->x+300;rt.update(st);
            const bool first=rt.ambushes()[0].fires==1;
            rt.update(st);const bool stable=rt.ambushes()[0].fires==1;
            st.px=p->x;rt.update(st);st.px=p->x+700;rt.update(st);
            st.px=p->x+300;rt.update(st);
            pass=first&&stable&&rt.ambushes()[0].fires==2;
        }
        check(pass,a.id+" authored ambush arms, fires once and rearms");
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
