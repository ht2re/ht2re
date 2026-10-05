#include "game/Cockpit.h"
#include "game/Vehicle.h"
#include "gfx/Renderer.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace ht2;
int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : "/tmp/ht2-steering";
    Renderer r;
    if (!r.init(1280,720,true)) return 1;
    Cockpit ck;
    if (!ck.load(r,"original","ZilR") || !ck.hasWheel()) return 2;
    const Mat4 zero = ck.wheelTransform(0);
    for (float direction : {-1.f, 1.f}) {
        const Mat4 turn = ck.wheelTransform(direction);
        // Column Y is the wheel-plane normal: turning must never tilt it.
        for (int axis=0;axis<3;++axis)
            if (std::fabs(turn.m[4+axis]-zero.m[4+axis])>.0001f) return 6;
        // Authored pivot must remain fixed.
        for (int axis=12;axis<15;++axis)
            if (std::fabs(turn.m[axis]-zero.m[axis])>.0001f) return 7;
    }
    Vehicle v;
    v.useVehicle("ZilR");
    v.reset(0,0,0);
    ck.setVisible(true);
    float x,y,z,pitch;
    ck.eyePose(x,y,z,pitch);
    y += v.y();
    auto render = [&](const char* name) {
        r.draw(x,y,z,180,pitch);
        r.clearDepth();
        r.setNearPlane(.4f);
        ck.draw(r,v.modelMatrix(),v,x,y,z,180,pitch);
        r.screenshot(out+"_"+name+".bmp");
    };
    render("neutral");
    for (float direction : {-1.f, 1.f}) {
        Vehicle::Input in; in.steer=direction;
        for (int i=0;i<90;++i) v.update(in,1.f/60);
        render(direction < 0 ? "left" : "right");
        if (direction*ck.wheelAngleDeg()<440) return 3;
    }
    Vehicle::Input idle;
    for (int i=0;i<240;++i) v.update(idle,1.f/60);
    render("returned");
    if (std::fabs(ck.wheelAngleDeg())>.01f) return 4;
    ck.setVisible(false);
    const float before=ck.wheelAngleDeg();
    ck.draw(r,v.modelMatrix(),v,x,y,z,180,pitch);
    if (ck.wheelAngleDeg()!=before) return 5;
    r.shutdown();
    std::puts("RESULT cockpit steering left/right/return/hidden PASS");
}
