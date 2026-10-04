#pragma once
// Minimal 4x4 matrix math (column-major, OpenGL layout)
#include <cmath>
#include <cstring>

namespace ht2 {

struct Mat4 {
    float m[16];
    Mat4() { std::memset(m, 0, sizeof(m)); m[0]=m[5]=m[10]=m[15]=1.0f; }
    static Mat4 perspective(float fovYdeg, float aspect, float znear, float zfar) {
        Mat4 r;
        float f = 1.0f / tanf(fovYdeg * 0.5f * 3.14159265f / 180.0f);
        r.m[0]=f/aspect; r.m[5]=f; r.m[10]=(zfar+znear)/(znear-zfar);
        r.m[11]=-1.0f; r.m[14]=(2.0f*zfar*znear)/(znear-zfar); r.m[15]=0.0f;
        return r;
    }
    static Mat4 lookAt(float ex,float ey,float ez,
                       float cx,float cy,float cz,
                       float ux=0,float uy=1,float uz=0) {
        float zx=ex-cx, zy=ey-cy, zz=ez-cz;
        float zl = sqrtf(zx*zx+zy*zy+zz*zz); zx/=zl; zy/=zl; zz/=zl;
        float xx=uy*zz-uz*zy, xy=uz*zx-ux*zz, xz=ux*zy-uy*zx;
        float xl = sqrtf(xx*xx+xy*xy+xz*xz); xx/=xl; xy/=xl; xz/=xl;
        float yx=zy*xz-zz*xy, yy=zz*xx-zx*xz, yz=zx*xy-zy*xx;
        Mat4 r;
        r.m[0]=xx; r.m[1]=yx; r.m[2]=zx; r.m[3]=0;
        r.m[4]=xy; r.m[5]=yy; r.m[6]=zy; r.m[7]=0;
        r.m[8]=xz; r.m[9]=yz; r.m[10]=zz; r.m[11]=0;
        r.m[12]=-(xx*ex+xy*ey+xz*ez);
        r.m[13]=-(yx*ex+yy*ey+yz*ez);
        r.m[14]=-(zx*ex+zy*ey+zz*ez);
        r.m[15]=1;
        return r;
    }
    static Mat4 identity() { return Mat4(); }
    static Mat4 translation(float x, float y, float z) {
        Mat4 r; r.m[12] = x; r.m[13] = y; r.m[14] = z; return r;
    }
    // rotation about the world up axis (+Y), degrees
    static Mat4 rotY(float deg) {
        Mat4 r;
        float a = deg * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
        r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
        return r;
    }
    // rotation about +X (pitch), radians
    static Mat4 rotX(float rad) {
        Mat4 r; float c = cosf(rad), s = sinf(rad);
        r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
        return r;
    }
    // rotation about +Z (roll), radians
    static Mat4 rotZ(float rad) {
        Mat4 r; float c = cosf(rad), s = sinf(rad);
        r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
        return r;
    }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r; r.m[15] = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float v = 0;
                for (int k = 0; k < 4; ++k) v += m[k * 4 + j] * o.m[i * 4 + k];
                r.m[i * 4 + j] = v;
            }
        return r;
    }
};

} // namespace ht2