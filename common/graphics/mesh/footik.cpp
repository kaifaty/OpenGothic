#include "footik.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/constants.h"
#include "physics/dynamicworld.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "skeleton.h"

using namespace Tempest;

namespace {

// Gothic units are centimeters
constexpr float MaxRaise   = 45.f;   // how far a foot may be lifted above the animation floor
constexpr float MaxDrop    = 45.f;   // how far the body may be lowered
constexpr float MaxStep    = 50.f;   // larger height difference between the feet is a ledge, not a slope
constexpr float MaxTilt    = 0.61f;  // radians (~35 degrees), foot tilt limit
constexpr float MaxSpread  = 100.f;  // animated ankle further than this from the origin: pose is not valid yet
constexpr float OffsetRate = 12.f;   // 1/s, smoothing of ground offsets
constexpr float BlendTime  = 0.25f;  // seconds, fade in/out of the whole effect
constexpr float Snap       = 0.01f;  // smoothing stops this close to the target
constexpr float SnapCos    = 1.f - 1e-6f;
constexpr float Eps        = 1e-4f;

const Vec3 Up = {0,1,0};

bool envFlag(const char* name, bool def) {
  const char* v = std::getenv(name);
  if(v==nullptr || v[0]=='\0')
    return def;
  return std::strcmp(v,"0")!=0;
  }

bool enabled    = envFlag("OPENGOTHIC_FOOTIK",     true);
bool logEnabled = envFlag("OPENGOTHIC_FOOTIK_LOG", false);

// row-major: m[row][col]
struct Mat3 final {
  float m[3][3];
  };

float dot(const Vec3& a, const Vec3& b) {
  return a.x*b.x + a.y*b.y + a.z*b.z;
  }

Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
  }

float length(const Vec3& v) {
  return std::sqrt(dot(v,v));
  }

Vec3 normalize(const Vec3& v) {
  const float l = length(v);
  return l>Eps ? v*(1.f/l) : Vec3();
  }

Vec3 position(const Matrix4x4& m) {
  return {m.at(3,0), m.at(3,1), m.at(3,2)};
  }

Mat3 mul(const Mat3& a, const Mat3& b) {
  Mat3 r = {};
  for(int i=0; i<3; ++i)
    for(int j=0; j<3; ++j)
      r.m[i][j] = a.m[i][0]*b.m[0][j] + a.m[i][1]*b.m[1][j] + a.m[i][2]*b.m[2][j];
  return r;
  }

Vec3 mul(const Mat3& a, const Vec3& v) {
  return {a.m[0][0]*v.x + a.m[0][1]*v.y + a.m[0][2]*v.z,
          a.m[1][0]*v.x + a.m[1][1]*v.y + a.m[1][2]*v.z,
          a.m[2][0]*v.x + a.m[2][1]*v.y + a.m[2][2]*v.z};
  }

Mat3 transpose(const Mat3& a) {
  Mat3 r = {};
  for(int i=0; i<3; ++i)
    for(int j=0; j<3; ++j)
      r.m[i][j] = a.m[j][i];
  return r;
  }

// Shortest-arc rotation that takes unit vector u to unit vector v
Mat3 rotationBetween(const Vec3& u, const Vec3& v) {
  const Vec3  k = cross(u,v);
  const float c = dot(u,v);
  if(c<-1.f+Eps) {
    // opposite vectors: half turn around any perpendicular axis
    const Vec3 a = normalize(cross(u, std::abs(u.x)<0.9f ? Vec3(1,0,0) : Vec3(0,1,0)));
    const float x[3] = {a.x, a.y, a.z};
    Mat3 r = {};
    for(int i=0; i<3; ++i)
      for(int j=0; j<3; ++j)
        r.m[i][j] = 2.f*x[i]*x[j] - (i==j ? 1.f : 0.f);
    return r;
    }
  // R = c*I + [k]x + k*k^T/(1+c)
  const float s = 1.f/(1.f+c);
  return Mat3{{
    {c + k.x*k.x*s,   k.x*k.y*s - k.z, k.x*k.z*s + k.y},
    {k.x*k.y*s + k.z, c + k.y*k.y*s,   k.y*k.z*s - k.x},
    {k.x*k.z*s - k.y, k.y*k.z*s + k.x, c + k.z*k.z*s  },
    }};
  }

// Rotates world-space bone matrices around pivot p: tr = W*tr, W = [R | p - R*p]
void rotateAbout(Matrix4x4* tr, const std::vector<size_t>& bones, const Mat3& r, const Vec3& p) {
  const Vec3 rp = mul(r,p);
  Matrix4x4 w;
  for(int row=0; row<3; ++row)
    for(int col=0; col<3; ++col)
      w.set(col,row,r.m[row][col]);
  w.set(0,3,0);
  w.set(1,3,0);
  w.set(2,3,0);
  w.set(3,0,p.x-rp.x);
  w.set(3,1,p.y-rp.y);
  w.set(3,2,p.z-rp.z);
  w.set(3,3,1);
  for(auto i:bones)
    tr[i] = w*tr[i];
  }

// Ground normal turned up and limited to MaxTilt
Vec3 footNormal(Vec3 n) {
  n = normalize(n);
  if(n.y<0)
    n = n*-1.f;
  if(length(n)<Eps)
    return Up;
  const float maxCos = std::cos(MaxTilt);
  if(n.y>=maxCos)
    return n;
  const Vec3  side   = normalize(Vec3(n.x,0,n.z));
  const float maxSin = std::sin(MaxTilt);
  return {side.x*maxSin, maxCos, side.z*maxSin};
  }

void approach(float& v, float target, float k, bool& changed) {
  float nv = v + (target-v)*k;
  if(std::abs(target-nv)<Snap)
    nv = target;
  if(nv!=v)
    changed = true;
  v = nv;
  }

}

bool FootIk::isEnabled() {
  return enabled;
  }

void FootIk::setEnabled(bool e) {
  enabled = e;
  Log::i("foot ik: ", e ? "on" : "off");
  }

void FootIk::setSkeleton(const Skeleton* sk) {
  valid  = false;
  pelvis = 0;
  weight = 0;
  for(auto& c:legs)
    c = Chain();
  if(sk==nullptr)
    return;

  static const std::string_view names[2][4] = {
    {"BIP01 L THIGH", "BIP01 L CALF", "BIP01 L FOOT", "BIP01 L TOE0"},
    {"BIP01 R THIGH", "BIP01 R CALF", "BIP01 R FOOT", "BIP01 R TOE0"},
    };

  auto& nodes = sk->nodes;
  auto  find  = [&](std::string_view name) {
    for(size_t i=0; i<nodes.size(); ++i)
      if(nodes[i].name==name)
        return i;
    return size_t(-1);
    };
  auto  isUnder = [&](size_t id, size_t root) {
    for(size_t i=id; i<nodes.size(); i=nodes[i].parent)
      if(i==root)
        return true;
    return false;
    };

  for(size_t l=0; l<2; ++l) {
    Chain& c = legs[l];
    c.thigh = find(names[l][0]);
    c.calf  = find(names[l][1]);
    c.foot  = find(names[l][2]);
    c.toe   = find(names[l][3]);
    if(c.thigh==size_t(-1) || c.calf==size_t(-1) || c.foot==size_t(-1))
      return;
    if(!isUnder(c.calf,c.thigh) || !isUnder(c.foot,c.calf))
      return;
    if(c.toe!=size_t(-1) && !isUnder(c.toe,c.foot))
      c.toe = size_t(-1);
    for(size_t i=0; i<nodes.size(); ++i) {
      if(isUnder(i,c.thigh))
        c.thighTree.push_back(i);
      if(isUnder(i,c.calf))
        c.calfTree.push_back(i);
      if(isUnder(i,c.foot))
        c.footTree.push_back(i);
      }
    }
  valid = true;
  }

FootIk::Input FootIk::mkInput(const Npc& npc, World& world, const Matrix4x4& obj,
                              bool alignedToGround, uint64_t dt) const {
  Input in;
  in.dt     = dt;
  in.floorY = obj.at(3,1);
  if(!valid || !enabled || alignedToGround)
    return in;
  if(npc.isDead() || npc.isUnconscious() || npc.isInAir() || npc.isSwim() || npc.interactive()!=nullptr)
    return in;
  // standing only for now: locomotion needs foot plant detection
  if((npc.bodyStateMasked() & BS_MAX)!=BS_NONE)
    return in;

  const Vec3 pos = position(obj);
  if(!world.isInPfxRange(pos))
    return in;
  auto physic = world.physic();
  if(physic==nullptr)
    return in;

  for(size_t i=0; i<2; ++i) {
    const Vec3 ankle = legs[i].animAnkle;
    if(std::abs(ankle.x-pos.x)>MaxSpread || std::abs(ankle.z-pos.z)>MaxSpread)
      return in;
    }

  for(size_t i=0; i<2; ++i) {
    const Vec3 ankle = legs[i].animAnkle;
    const Vec3 from  = {ankle.x, in.floorY + MaxRaise, ankle.z};
    const Vec3 to    = {ankle.x, in.floorY - MaxDrop,  ankle.z};
    auto r = physic->ray(from,to);
    in.ground[i].hasCol = r.hasCol;
    in.ground[i].y      = r.v.y;
    in.ground[i].n      = r.n;
    }
  in.enable = true;
  return in;
  }

bool FootIk::update(const Input& in) {
  if(!valid)
    return false;

  const float dt = float(in.dt)/1000.f;
  const float k  = 1.f - std::exp(-OffsetRate*dt);
  bool        on = in.enable && (in.ground[0].hasCol || in.ground[1].hasCol);

  float off[2] = {0, 0};
  Vec3  n  [2] = {Up, Up};
  if(on) {
    for(size_t i=0; i<2; ++i) {
      if(!in.ground[i].hasCol)
        continue;
      off[i] = std::clamp(in.ground[i].y - in.floorY, -MaxDrop, MaxRaise);
      n  [i] = footNormal(in.ground[i].n);
      }
    }
  if(std::abs(off[0]-off[1])>MaxStep) {
    on     = false;
    off[0] = off[1] = 0;
    n[0]   = n[1]   = Up;
    }
  const float pel = on ? std::clamp(std::min(off[0],off[1]), -MaxDrop, 0.f) : 0.f;

  bool changed = false;
  const float w = on ? std::min(1.f, weight + dt/BlendTime) : std::max(0.f, weight - dt/BlendTime);
  if(w!=weight)
    changed = true;
  weight = w;

  if(weight<=0.f) {
    // nothing is visible: start from the animated pose next time
    pelvis = 0;
    for(auto& c:legs) {
      c.offset = 0;
      c.normal = Up;
      }
    return changed;
    }

  approach(pelvis,pel,k,changed);
  for(size_t i=0; i<2; ++i) {
    auto& c = legs[i];
    approach(c.offset,off[i],k,changed);
    Vec3 nn = normalize(c.normal + (n[i]-c.normal)*k);
    if(dot(nn,n[i])>SnapCos)
      nn = n[i];
    if(nn!=c.normal)
      changed = true;
    c.normal = nn;
    }
  return changed;
  }

void FootIk::apply(Matrix4x4* tr, size_t numBones) {
  if(!valid)
    return;
  for(auto& c:legs) {
    if(c.thighTree.empty() || c.thighTree.back()>=numBones)
      return;
    c.animAnkle  = position(tr[c.foot]);
    c.finalAnkle = c.animAnkle;
    }
  if(weight<=0.f)
    return;

  const float dy = pelvis*weight;
  if(dy!=0.f) {
    for(size_t i=0; i<numBones; ++i)
      tr[i].set(3,1,tr[i].at(3,1)+dy);
    }

  for(auto& c:legs) {
    Vec3 target = c.animAnkle;
    target.y += c.offset*weight;
    const Vec3 n = normalize(Up*(1.f-weight) + c.normal*weight);
    solve(tr,c,target,n);
    c.finalAnkle = position(tr[c.foot]);
    }
  }

void FootIk::solve(Matrix4x4* tr, const Chain& c, const Vec3& target, const Vec3& normal) const {
  const Vec3  a   = position(tr[c.thigh]);
  const Vec3  b   = position(tr[c.calf]);
  const Vec3  f   = position(tr[c.foot]);
  const float lab = length(b-a);
  const float lbf = length(f-b);
  const Vec3  at  = target-a;
  float       lat = length(at);
  if(lab<Eps || lbf<Eps || lat<Eps)
    return;

  const Vec3 dir = at*(1.f/lat);
  lat = std::clamp(lat, std::abs(lab-lbf)+Eps, (lab+lbf)*0.999f);

  // The knee keeps its animated direction; a nearly straight leg bends toward the toes
  Vec3 pole = b-a;
  if(c.toe!=size_t(-1)) {
    Vec3 fwd = position(tr[c.toe]) - f;
    fwd.y = 0;
    pole += normalize(fwd)*(0.05f*lab);
    }
  pole = normalize(pole - dir*dot(pole,dir));
  if(length(pole)<Eps)
    return;

  const float cosA = std::clamp((lab*lab + lat*lat - lbf*lbf)/(2.f*lab*lat), -1.f, 1.f);
  const float sinA = std::sqrt(1.f - cosA*cosA);
  const Vec3  knee  = a + dir*(lab*cosA) + pole*(lab*sinA);
  const Vec3  ankle = a + dir*lat;

  const Mat3 r0 = rotationBetween(normalize(b-a), normalize(knee-a));
  rotateAbout(tr,c.thighTree,r0,a);

  const Vec3 b1 = position(tr[c.calf]);
  const Vec3 f1 = position(tr[c.foot]);
  const Mat3 r1 = rotationBetween(normalize(f1-b1), normalize(ankle-b1));
  rotateAbout(tr,c.calfTree,r1,b1);

  // The foot keeps its animated orientation, tilted to the ground
  const Mat3 legRot = mul(r1,r0);
  const Mat3 fix    = mul(rotationBetween(Up,normal), transpose(legRot));
  rotateAbout(tr,c.footTree,fix,position(tr[c.foot]));
  }

void FootIk::debugLog(std::string_view name, const Input& in) {
  if(!logEnabled || !in.enable)
    return;
  logTimer += in.dt;
  if(logTimer<1000)
    return;
  logTimer = 0;

  float off[2] = {};
  float err[2] = {};
  for(size_t i=0; i<2; ++i) {
    if(!in.ground[i].hasCol)
      return;
    off[i] = in.ground[i].y - in.floorY;
    // distance between the ankle and where it should be over the ground; without IK it is -off
    err[i] = legs[i].finalAnkle.y - (legs[i].animAnkle.y + off[i]);
    }
  if(std::abs(off[0])<1.f && std::abs(off[1])<1.f)
    return;

  char buf[256] = {};
  std::snprintf(buf, sizeof(buf), "offL=%.1f offR=%.1f pelvis=%.1f w=%.2f errL=%.1f errR=%.1f",
                off[0], off[1], pelvis, weight, err[0], err[1]);
  Log::i("[footik] ", name, " ", buf);
  }
