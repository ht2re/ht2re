#include "world/B3D.h"
#include "world/GroundMap.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <vector>
using namespace ht2;
int main(int argc,char** argv) {
    const std::string dir=(argc>1?argv[1]:"original")+std::string("/ENV");
    DIR* dp=opendir(dir.c_str()); if(!dp)return 1;
    std::vector<std::string> files;
    while(auto* entry=readdir(dp)) {
        const std::string n=entry->d_name;
        if(n.size()>4&&n.substr(n.size()-4)==".b3d")files.push_back(dir+"/"+n);
    }
    closedir(dp); std::sort(files.begin(),files.end());
    std::vector<B3D::SubMesh> meshes;
    for(const auto& path:files) {
        B3D b; if(!b.load(path))return 1; b.buildScene();
        for(auto& sm:b.submeshesMutable())meshes.push_back(std::move(sm));
    }
    GroundMap ground; ground.build(meshes); ground.buildRoadSurface(meshes);
    // Optional column diagnostics for reproducing support-layer transitions.
    if(argc>2) {
        float x=0,z=0,ref=0; if(sscanf(argv[2],"%f,%f,%f",&x,&z,&ref)!=3)return 2;
        for(float dz=-4;dz<=4;dz+=.5f) {
            int src=0; const float y=ground.drivableY(x,z+dz,ref,&src);
            printf("COLUMN %.3f %.3f ref %.3f -> %.3f src %d coarse %.3f\n",x,z+dz,ref,y,src,ground.heightAt(x,z+dz,ref));
            const auto& field=ground.groundSurface();
            for(const auto& t:field.triangles()) {
                const float den=(t.bx-t.ax)*(t.cz-t.az)-(t.bz-t.az)*(t.cx-t.ax);
                if(std::fabs(den)<1e-9f)continue;
                const float a=((t.bx-x)*(t.cz-z-dz)-(t.bz-z-dz)*(t.cx-x))/den;
                const float b=((t.cx-x)*(t.az-z-dz)-(t.cz-z-dz)*(t.ax-x))/den;
                if(a<0||b<0||a+b>1)continue;
                printf("  %s y %.3f\n",field.roadMaterials()[t.mat].c_str(),a*t.ay+b*t.by+(1-a-b)*t.cy);
            }
            for(const auto& sm:meshes) for(size_t j=0;j+2<sm.idx.size();j+=3) {
                const auto ia=sm.idx[j]*3,ib=sm.idx[j+1]*3,ic=sm.idx[j+2]*3;
                if(ia+2>=sm.pos.size()||ib+2>=sm.pos.size()||ic+2>=sm.pos.size())continue;
                const float *a=&sm.pos[ia],*b=&sm.pos[ib],*c=&sm.pos[ic];
                const float den=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
                if(std::fabs(den)<1e-9f)continue;
                const float u=((b[0]-x)*(c[1]-z-dz)-(b[1]-z-dz)*(c[0]-x))/den;
                const float v=((c[0]-x)*(a[1]-z-dz)-(c[1]-z-dz)*(a[0]-x))/den;
                if(u<0||v<0||u+v>1)continue;
                printf("  RAW %s y %.3f\n",sm.material.c_str(),u*a[2]+v*b[2]+(1-u-v)*c[2]);
            }
        }
        return 0;
    }
    const auto& surface=ground.groundSurface();
    std::vector<float> errors; size_t missing=0,wrongLayer=0;
    double worst=0; float wx=0,wz=0;
    for(const auto& t:surface.triangles()) {
        if(t.mat>=surface.roadMaterials().size()||RoadSurface::isRoadMaterial(surface.roadMaterials()[t.mat]))continue;
        // Interior points avoid ambiguous triangle seams; samples use actual
        // authored geometry, not coarse cells or navigation centreline heights.
        for(const auto& weight: {std::vector<float>{.2f,.3f,.5f}, {.5f,.2f,.3f}, {.3f,.5f,.2f}}) {
            const float x=t.ax*weight[0]+t.bx*weight[1]+t.cx*weight[2];
            const float z=t.az*weight[0]+t.bz*weight[1]+t.cz*weight[2];
            const float y=t.ay*weight[0]+t.by*weight[1]+t.cy*weight[2];
            int src=0; const float actual=ground.drivableY(x,z,y,&src);
            if(src==0||src==1)++missing;
            const float error=std::fabs(actual-y); errors.push_back(error);
            if(error>.051f)++wrongLayer;
            if(error>worst) { worst=error; wx=x; wz=z; }
        }
    }
    std::sort(errors.begin(),errors.end());
    const float p95=errors.empty()?1e30f:errors[(size_t)(.95*(errors.size()-1))];
    printf("RESULT ground contact %zu tiles, %zu samples, %zu graph/coarse fallbacks, %zu errors >5.1cm; p95 %.6f m max %.6f m at %.2f,%.2f\n",
           files.size(),errors.size(),missing,wrongLayer,p95,worst,wx,wz);
    return files.size()<30||errors.size()<1000||missing||wrongLayer?1:0;
}
