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
constexpr float MaxRaise       = 45.f;   // how far a foot may be lifted above the animation floor
constexpr float MaxDrop        = 45.f;   // how far the body may be lowered
constexpr float MaxStep        = 50.f;   // larger height difference between the feet is a ledge, not a slope
constexpr float MaxTilt        = 0.61f;  // radians (~35 degrees), foot tilt limit
constexpr float MaxPitch       = 0.52f;  // radians (~30 degrees), toes up/down
constexpr float MaxRoll        = 0.17f;  // radians (~10 degrees), sideways: an ankle hardly rolls more
constexpr float OutwardFactor  = 0.35f;  // on a slope the legs turn outward by this part of its steepness
constexpr float MaxOutward     = 0.21f;  // radians (~12 degrees)
constexpr float MaxSpread      = 100.f;  // animated ankle further than this from the origin: pose is not valid yet
constexpr float PlantLift      = 12.f;   // an ankle this much above the other one swings
constexpr float SwingAllowance = 30.f;   // a swinging foot does not pull the body down
constexpr float PlantRate      = 200.f;  // 1/s, the ground under a planted foot hardly needs smoothing
constexpr float SwingRate      = 25.f;   // 1/s, smoothing of the ground under a swinging foot
constexpr float NormalRate     = 15.f;   // 1/s, smoothing of the foot tilt
constexpr float PelvisRate     = 8.f;    // 1/s, smoothing of the body height
constexpr float ReachRate      = 30.f;   // 1/s, how fast the body goes down when a planted foot can not reach
constexpr float FloorJump      = 8.f;    // the floor moving more in one frame is a step: the body stays put
constexpr float LegReach       = 0.98f;  // fraction of the full leg length a leg may straighten to
constexpr float BlendTime      = 0.25f;  // seconds, fade in/out of the whole effect
constexpr float Snap           = 0.01f;  // smoothing stops this close to the target
constexpr float SnapCos        = 1.f - 1e-6f;
constexpr float Eps            = 1e-4f;

const Vec3 Up = {0,1,0};

int envInt(const char* name, int def) {
  const char* v = std::getenv(name);
  if(v==nullptr || v[0]=='\0')
    return def;
  return std::atoi(v);
  }

bool enabled  = envInt("OPENGOTHIC_FOOTIK",     1)!=0;
int  logLevel = envInt("OPENGOTHIC_FOOTIK_LOG", 0); // 1: npc's on uneven ground once a second, 2: also the player every frame

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

// Rotation by angle a around unit axis u
Mat3 axisAngle(const Vec3& u, float a) {
  const float c = std::cos(a), s = std::sin(a), t = 1.f-c;
  return Mat3{{
    {t*u.x*u.x + c,     t*u.x*u.y - s*u.z, t*u.x*u.z + s*u.y},
    {t*u.x*u.y + s*u.z, t*u.y*u.y + c,     t*u.y*u.z - s*u.x},
    {t*u.x*u.z - s*u.y, t*u.y*u.z + s*u.x, t*u.z*u.z + c    },
    }};
  }

Mat3 fromColumns(const Vec3& x, const Vec3& y, const Vec3& z) {
  return Mat3{{
    {x.x, y.x, z.x},
    {x.y, y.y, z.y},
    {x.z, y.z, z.z},
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

// debug: knee offset from the hip-ankle line
Vec3 kneeBend(const Matrix4x4* tr, size_t thigh, size_t calf, size_t foot) {
  const Vec3 hip  = position(tr[thigh]);
  const Vec3 leg  = normalize(position(tr[foot])-hip);
  const Vec3 knee = position(tr[calf]) - hip;
  return knee - leg*dot(knee,leg);
  }

// Ground normal limited in the frame of the foot: toes may go up and down more than the foot may roll
Vec3 footTilt(const Vec3& n, const Vec3& heading) {
  const Vec3  side  = cross(Up,heading);
  const float pitch = std::clamp(std::atan2(dot(n,heading),n.y), -MaxPitch, MaxPitch);
  const float roll  = std::clamp(std::atan2(dot(n,side),   n.y), -MaxRoll,  MaxRoll);
  return normalize(heading*std::tan(pitch) + side*std::tan(roll) + Up);
  }

// How much a leg turns outward on ground with normal n, walking along heading
float outwardTarget(const Vec3& n, const Vec3& heading) {
  const float pitch = std::abs(std::atan2(dot(n,heading),n.y));
  return std::min(pitch*OutwardFactor, MaxOutward);
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
  if(npc.isDead() || npc.isUnconscious() || npc.interactive()!=nullptr)
    return in;
  if(npc.isInAir() || npc.isFalling() || npc.isSlide() || npc.isSwim()) {
    in.hardOff = true;
    return in;
    }

  const uint32_t bs = npc.bodyStateMasked() & BS_MAX;
  if(bs==(BS_JUMP & BS_MAX) || bs==(BS_FALL & BS_MAX) || bs==(BS_SWIM & BS_MAX) ||
     bs==(BS_DIVE & BS_MAX) || bs==(BS_CLIMB & BS_MAX)) {
    in.hardOff = true;
    return in;
    }
  // standing and locomotion; other animations (attacks, items, ...) fade out
  if(bs!=BS_NONE && bs!=(BS_WALK & BS_MAX) && bs!=(BS_SNEAK & BS_MAX) && bs!=BS_RUN && bs!=BS_SPRINT)
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
    auto r = physic->ray({ankle.x, in.floorY + MaxRaise, ankle.z}, {ankle.x, in.floorY - MaxDrop, ankle.z});
    in.ground[i].hasCol = r.hasCol;
    in.ground[i].y      = r.v.y;
    in.ground[i].n      = r.n;
    if(legs[i].toe!=size_t(-1)) {
      const Vec3 toe = legs[i].animToe;
      auto t = physic->ray({toe.x, in.floorY + MaxRaise, toe.z}, {toe.x, in.floorY - MaxDrop, toe.z});
      in.ground[i].toeCol = t.hasCol;
      in.ground[i].toeY   = t.v.y;
      }
    }
  in.enable = true;
  return in;
  }

float FootIk::offset(const Chain& c, float floor) const {
  return std::clamp(c.groundY - floor, -MaxDrop, MaxRaise);
  }

bool FootIk::update(const Input& in) {
  if(!valid)
    return false;

  const float dt = float(in.dt)/1000.f;
  bool        on = in.enable;

  // the lower animated ankle stands on the ground, a raised one swings
  const float lowAnkle = std::min(legs[0].animAnkle.y, legs[1].animAnkle.y);
  float       plant[2] = {};
  for(size_t i=0; i<2; ++i)
    plant[i] = std::clamp(1.f - (legs[i].animAnkle.y-lowAnkle)/PlantLift, 0.f, 1.f);

  // ground under each foot; a foot without ground keeps its animated height
  bool  has[2] = {};
  float gy [2] = {in.floorY, in.floorY};
  Vec3  n  [2] = {Up, Up};
  for(size_t i=0; i<2; ++i) {
    if(!on || !in.ground[i].hasCol)
      continue;
    has[i] = true;
    gy [i] = in.ground[i].y;
    n  [i] = footNormal(in.ground[i].n);
    }
  if(has[0] && has[1] && std::abs(gy[0]-gy[1])>MaxStep) {
    // the lower foot is over a ledge
    const size_t low = gy[0]<gy[1] ? 0 : 1;
    has[low] = false;
    gy [low] = in.floorY;
    n  [low] = Up;
    }
  if(!has[0] && !has[1])
    on = false;
  // a swinging foot goes over the higher of the ground under the ankle and under the toes
  for(size_t i=0; i<2; ++i) {
    if(has[i] && in.ground[i].toeCol && in.ground[i].toeY>gy[i] && in.ground[i].toeY-gy[i]<MaxStep)
      gy[i] += (in.ground[i].toeY-gy[i])*(1.f-plant[i]);
    }

  // The body follows the lower foot whichever foot stands on it: following only the planted
  // foot makes the body dip on every step of the lower leg (looks like limping)
  auto pelvisTarget = [&]() {
    const float p = std::min(offset(legs[0],in.floorY), offset(legs[1],in.floorY));
    return std::clamp(p, -MaxDrop, 0.f);
    };
  // A planted foot must reach the ground: the only reason to lower the body fast
  // (not at once: a sudden dip at every step of the lower leg looks like limping)
  auto pelvisLimit = [&]() {
    float p = 0;
    for(size_t i=0; i<2; ++i)
      p = std::min(p, offset(legs[i],in.floorY) + legs[i].slack + (1.f-legs[i].plant)*SwingAllowance);
    return std::max(p, -MaxDrop);
    };
  const float dFloor = in.floorY - lastFloor;
  lastFloor = in.floorY;

  bool        changed = false;
  const float prevW   = weight;
  if(in.hardOff)
    weight = 0;
  else if(on)
    weight = std::min(1.f, weight + dt/BlendTime);
  else
    weight = std::max(0.f, weight - dt/BlendTime);
  if(weight!=prevW)
    changed = true;

  if(prevW<=0.f) {
    if(weight<=0.f)
      return changed;
    // fading in: start from the current ground without smoothing lag
    for(size_t i=0; i<2; ++i) {
      legs[i].groundY = gy[i];
      legs[i].plant   = plant[i];
      legs[i].normal  = normalize(Up*(1.f-plant[i]) + footTilt(n[i],legs[i].heading)*plant[i]);
      legs[i].outward = has[i] ? outwardTarget(n[i],legs[i].heading) : 0.f;
      }
    pelvis = pelvisTarget();
    return true;
    }

  const float kN = 1.f - std::exp(-NormalRate*dt);
  for(size_t i=0; i<2; ++i) {
    auto&       c    = legs[i];
    const float rate = SwingRate + (PlantRate-SwingRate)*plant[i];
    if(std::abs(gy[i]-c.groundY)>MaxStep) {
      // teleport or a ledge: no smoothing across it
      c.groundY = gy[i];
      changed   = true;
      }
    approach(c.groundY,gy[i],1.f - std::exp(-rate*dt),changed);
    if(c.plant!=plant[i])
      changed = true;
    c.plant = plant[i];

    // only a planted foot lies on the ground
    const Vec3 nt = normalize(Up*(1.f-plant[i]) + footTilt(n[i],c.heading)*plant[i]);
    Vec3 nn = normalize(c.normal + (nt-c.normal)*kN);
    if(dot(nn,nt)>SnapCos)
      nn = nt;
    if(nn!=c.normal)
      changed = true;
    c.normal = nn;

    approach(c.outward, has[i] ? outwardTarget(n[i],c.heading) : 0.f, kN, changed);
    }

  const float pt   = pelvisTarget();
  const float prev = pelvis;
  if(std::abs(dFloor)>=MaxStep) {
    pelvis = pt; // teleport
    }
  else {
    if(std::abs(dFloor)>FloorJump)
      pelvis = std::clamp(pelvis-dFloor, -MaxDrop, 0.f); // a step: the body stays where it was
    approach(pelvis,pt,1.f - std::exp(-PelvisRate*dt),changed);
    const float limit = pelvisLimit();
    if(pelvis>limit)
      pelvis += (limit-pelvis)*(1.f - std::exp(-ReachRate*dt));
    }
  if(pelvis!=prev)
    changed = true;
  return changed;
  }

void FootIk::apply(Matrix4x4* tr, size_t numBones, float floor) {
  if(!valid)
    return;
  for(auto& c:legs) {
    if(c.thighTree.empty() || c.thighTree.back()>=numBones)
      return;
    c.animAnkle  = position(tr[c.foot]);
    c.finalAnkle = c.animAnkle;

    // how much lower the ankle could go if the leg straightened (to LegReach)
    const Vec3  hip  = position(tr[c.thigh]);
    const float lmax = (length(position(tr[c.calf])-hip) + length(c.animAnkle-position(tr[c.calf])))*LegReach;
    const float h    = hip.y - c.animAnkle.y;
    const float x2   = (hip.x-c.animAnkle.x)*(hip.x-c.animAnkle.x) + (hip.z-c.animAnkle.z)*(hip.z-c.animAnkle.z);
    c.slack = std::max(0.f, std::sqrt(std::max(0.f, lmax*lmax - x2)) - h);

    if(c.toe!=size_t(-1)) {
      c.animToe = position(tr[c.toe]);
      const Vec3 toe = c.animToe - c.animAnkle;
      const Vec3 hd  = Vec3(toe.x,0,toe.z);
      if(length(hd)>1.f)
        c.heading = normalize(hd);
      }
    }
  {
    const Vec3 hips = position(tr[legs[Left].thigh]) - position(tr[legs[Right].thigh]);
    const Vec3 side = normalize(Vec3(hips.x,0,hips.z));
    legs[Left ].outDir = side;
    legs[Right].outDir = side*-1.f;
  }
  if(weight<=0.f) {
    for(auto& c:legs)
      c.kneeDev = c.tilt = c.roll = c.yaw = 0;
    return;
    }

  const float dy = pelvis*weight;
  if(dy!=0.f) {
    for(size_t i=0; i<numBones; ++i)
      tr[i].set(3,1,tr[i].at(3,1)+dy);
    }

  for(auto& c:legs) {
    Vec3 target = c.animAnkle;
    target.y += offset(c,floor)*weight;
    // the smoothed normal lags behind a turning foot: limit it to the current heading once more
    const Vec3 n = normalize(Up*(1.f-weight) + footTilt(c.normal,c.heading)*weight);
    Vec3      animBend;
    Matrix4x4 animFoot = tr[c.foot];
    if(logLevel>=2)
      animBend = kneeBend(tr,c.thigh,c.calf,c.foot);
    solve(tr,c,target,n,c.outward*weight);
    c.finalAnkle = position(tr[c.foot]);
    if(logLevel>=2) {
      const Vec3 bend = kneeBend(tr,c.thigh,c.calf,c.foot);
      c.kneeDev = length(animBend)>2.f && length(bend)>2.f ?
                    std::acos(std::clamp(dot(normalize(animBend),normalize(bend)),-1.f,1.f))*180.f/float(M_PI) : 0.f;
      c.tilt    = std::acos(std::clamp(n.y,-1.f,1.f))*180.f/float(M_PI);
      c.roll    = std::atan2(dot(n,cross(Up,c.heading)),n.y)*180.f/float(M_PI);
      {
        // where the foot rotation takes the animated heading of the sole
        Vec3 v = {0,0,0};
        for(int k=0; k<3; ++k) {
          const Vec3 ak = normalize(Vec3(animFoot.at(k,0),animFoot.at(k,1),animFoot.at(k,2)));
          const Vec3 bk = normalize(Vec3(tr[c.foot].at(k,0),tr[c.foot].at(k,1),tr[c.foot].at(k,2)));
          v += bk*dot(ak,c.heading);
          }
        c.yaw = std::atan2(dot(v,c.outDir),dot(v,c.heading))*180.f/float(M_PI);
        }
      }
    }
  }

void FootIk::solve(Matrix4x4* tr, const Chain& c, const Vec3& target, const Vec3& normal, float outward) const {
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

  // The knee bends in the plane of the animated leg, turned toward the target.
  // A nearly straight animated leg bends toward the toes (weighted by how level the foot is).
  // Note: the bend must not be taken relative to the target line: at a heel strike uphill
  // the animated knee is behind that line and the knee would flip backwards.
  Vec3 bend = b-a;
  if(c.toe!=size_t(-1)) {
    const Vec3  toe     = position(tr[c.toe]) - f;
    const float footLen = length(toe);
    if(footLen>Eps)
      bend += Vec3(toe.x,0,toe.z)*(0.05f*lab/footLen);
    }
  const Vec3 pole = normalize(cross(cross(f-a,bend),dir));
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

  // On a slope the whole leg turns outward around the hip-ankle line, like people do:
  // the ankle stays, the knee and the toes go away from the other leg
  Mat3 legRot = mul(r1,r0);
  Vec3 out    = {0,0,0};
  if(outward>0.f && length(c.outDir)>0.5f) {
    const Vec3 axis = normalize(ankle-a);
    const Vec3 move = cross(axis,c.heading);
    if(length(axis)>Eps && std::abs(dot(move,c.outDir))>Eps) {
      const Mat3 twist = axisAngle(axis, dot(move,c.outDir)>0 ? outward : -outward);
      rotateAbout(tr,c.thighTree,twist,a);
      legRot = mul(twist,legRot);
      }
    out = c.outDir;
    }

  // The foot keeps its animated orientation relative to a frame that is turned outward and laid
  // on the ground: toes keep their heading (a plain tilt would swing them sideways on a side slope)
  const Vec3 h0  = c.heading;
  const Vec3 hd  = normalize(h0*std::cos(outward) + out*std::sin(outward));
  const Vec3 fd  = normalize(hd - normal*dot(hd,normal));
  const Mat3 to  = fromColumns(fd, normal, cross(fd,normal));
  const Mat3 frm = fromColumns(h0, Up,     cross(h0,Up));
  const Mat3 fix = mul(mul(to,transpose(frm)), transpose(legRot));
  rotateAbout(tr,c.footTree,fix,position(tr[c.foot]));
  }

void FootIk::debugLog(std::string_view name, const Input& in, bool verbose) {
  if(logLevel<=0)
    return;
  logTime += in.dt;

  // contact error: ankle height over the real ground minus its animated height over the floor;
  // for a planted foot it should be ~0, without IK it is -off
  float off[2] = {};
  float err[2] = {};
  bool  hit    = true;
  for(size_t i=0; i<2; ++i) {
    hit = hit && in.ground[i].hasCol;
    off[i] = in.ground[i].hasCol ? in.ground[i].y - in.floorY : 0.f;
    err[i] = (legs[i].finalAnkle.y - (in.floorY + off[i])) - (legs[i].animAnkle.y - in.floorY);
    }

  char buf[320] = {};
  if(verbose && logLevel>=2) {
    std::snprintf(buf, sizeof(buf),
                  "t=%llu on=%d w=%.2f floor=%.1f pelvis=%.1f | L lift=%.1f plant=%.2f off=%.1f err=%.1f knee=%.1f tilt=%.1f roll=%.1f out=%.1f"
                  " | R lift=%.1f plant=%.2f off=%.1f err=%.1f knee=%.1f tilt=%.1f roll=%.1f out=%.1f",
                  (unsigned long long)logTime, int(in.enable), weight, in.floorY, pelvis,
                  legs[0].animAnkle.y-in.floorY, legs[0].plant, off[0], err[0], legs[0].kneeDev, legs[0].tilt, legs[0].roll, legs[0].yaw,
                  legs[1].animAnkle.y-in.floorY, legs[1].plant, off[1], err[1], legs[1].kneeDev, legs[1].tilt, legs[1].roll, legs[1].yaw);
    Log::i("[footik] ", buf);
    return;
    }

  if(!in.enable || !hit)
    return;
  logTimer += in.dt;
  if(logTimer<1000)
    return;
  logTimer = 0;
  if(std::abs(off[0])<1.f && std::abs(off[1])<1.f)
    return;
  std::snprintf(buf, sizeof(buf), "offL=%.1f offR=%.1f pelvis=%.1f w=%.2f errL=%.1f errR=%.1f",
                off[0], off[1], pelvis, weight, err[0], err[1]);
  Log::i("[footik] ", name, " ", buf);
  }
