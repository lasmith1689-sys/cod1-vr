// Rotation-only handling filters. Call once per XR pair in room space;
// never filter head poses, controller positions, or the two eyes separately.
#pragma once
#include <math.h>
#include <string.h>

static inline float VRHandlingClamp(float x, float lo, float hi)
{ return x < lo ? lo : x > hi ? hi : x; }

static inline void VRHandlingQuat(const float m[9], float q[4])
{
    const float trace = m[0] + m[4] + m[8];
    if (trace > 0) {
        const float s = sqrtf(trace + 1.0f) * 2.0f;
        q[3] = s * 0.25f; q[0] = (m[7]-m[5])/s;
        q[1] = (m[2]-m[6])/s; q[2] = (m[3]-m[1])/s;
    } else {
        int i = m[4] > m[0] ? 1 : 0;
        if (m[8] > m[i*3+i]) i = 2;
        const int j = (i+1)%3, k = (i+2)%3;
        const float s = sqrtf(fmaxf(0.0f, 1.0f + m[i*3+i] - m[j*3+j] - m[k*3+k])) * 2.0f;
        q[i] = s * 0.25f; q[j] = (m[j*3+i]+m[i*3+j])/s;
        q[k] = (m[k*3+i]+m[i*3+k])/s; q[3] = (m[k*3+j]-m[j*3+k])/s;
    }
    const float n = sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    for (int i=0; i<4; ++i) q[i] /= n;
}
static inline void VRHandlingRows(const float q[4], float m[9])
{
    const float x=q[0], y=q[1], z=q[2], w=q[3];
    m[0]=1-2*(y*y+z*z); m[1]=2*(x*y-z*w); m[2]=2*(x*z+y*w);
    m[3]=2*(x*y+z*w); m[4]=1-2*(x*x+z*z); m[5]=2*(y*z-x*w);
    m[6]=2*(x*z-y*w); m[7]=2*(y*z+x*w); m[8]=1-2*(x*x+y*y);
}
static inline void VRHandlingSlerp(const float a[4], const float b[4], float t, float out[4])
{
    float d=0; for (int i=0; i<4; ++i) d+=a[i]*b[i];
    const float sign=d<0 ? -1.0f : 1.0f;
    d=VRHandlingClamp(fabsf(d), 0, 1);
    float wa=1-t, wb=t;
    if (d < 0.9995f) {
        const float angle=acosf(d), denom=sinf(angle);
        wa=sinf((1-t)*angle)/denom; wb=sinf(t*angle)/denom;
    }
    float n=0;
    for (int i=0; i<4; ++i) { out[i]=wa*a[i]+wb*sign*b[i]; n+=out[i]*out[i]; }
    n=sqrtf(n); for (int i=0; i<4; ++i) out[i]/=n;
}
static inline void VRHandlingBlendRows(const float a[9], const float b[9], float t, float out[9])
{
    float qa[4], qb[4], q[4]; VRHandlingQuat(a,qa); VRHandlingQuat(b,qb);
    VRHandlingSlerp(qa,qb,VRHandlingClamp(t,0,1),q); VRHandlingRows(q,out);
}
struct VRHandlingFilter { int valid; float q[4]; };
static inline void VRHandlingFilterRows(VRHandlingFilter* f, const float target[9],
                                        float dt, float strength, int active, float out[9])
{
    float q[4]; VRHandlingQuat(target,q);
    float d=0; for (int i=0; i<4; ++i) d+=f->q[i]*q[i];
    const float angle=2.0f*acosf(VRHandlingClamp(fabsf(d),0,1))*57.2957795f;
    if (!active || strength<=0 || !f->valid || dt<=0 || dt>0.1f || angle>60.0f) {
        memcpy(f->q,q,sizeof(q)); memcpy(out,target,9*sizeof(float));
        f->valid=active && strength>0; return;
    }
    // Resist small tremors; catch up more quickly on deliberate large turns.
    const float a=angle/12.0f;
    const float tau=(0.01f+0.09f*VRHandlingClamp(strength,0,1))/(1.0f+4.0f*a*a);
    float next[4]; VRHandlingSlerp(f->q,q,1.0f-expf(-dt/tau),next);
    memcpy(f->q,next,sizeof(next)); VRHandlingRows(next,out);
}
