#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <string_view>
#include <vector>

class Skeleton;
class Npc;
class World;

// Foot IK: keeps the feet of a humanoid on uneven ground, standing and walking.
// Runs as a post-pass over the world-space bone matrices that Pose builds from animation:
// the whole body drops to the lower planted foot, each leg is bent by a two-bone solver
// and the planted feet are tilted to the ground normal.
class FootIk final {
  public:
    enum Leg : uint8_t {
      Left  = 0,
      Right = 1,
      };

    struct Ground final {
      bool          hasCol = false;
      float         y      = 0;
      Tempest::Vec3 n      = {0,1,0};
      bool          toeCol = false; // ground under the toes: a swinging foot clears a step edge early
      float         toeY   = 0;
      };

    struct Input final {
      bool     enable  = false;
      bool     hardOff = false; // jump, fall, swim: switch off at once instead of fading out
      float    floorY  = 0;     // height that the animation treats as ground (object origin)
      Ground   ground[2];
      uint64_t dt      = 0;
      };

    static bool    isEnabled();
    static void    setEnabled(bool e);

    void           setSkeleton(const Skeleton* sk);
    bool           isValid() const { return valid; }

    // Ground under the animated feet of an npc; disabled input if the npc state does not allow IK
    auto           mkInput(const Npc& npc, World& world, const Tempest::Matrix4x4& obj,
                           bool alignedToGround, uint64_t dt) const -> Input;
    // Moves the smoothed state toward the input; true if the pose has to be rebuilt
    bool           update(const Input& in);
    // Applies IK to world-space bone matrices and records the animated ankle positions
    void           apply(Tempest::Matrix4x4* tr, size_t numBones, float floorY);

    void           debugLog(std::string_view name, const Input& in, bool verbose);

  private:
    struct Chain final {
      size_t              thigh = size_t(-1);
      size_t              calf  = size_t(-1);
      size_t              foot  = size_t(-1);
      size_t              toe   = size_t(-1);
      std::vector<size_t> thighTree, calfTree, footTree;

      Tempest::Vec3       animAnkle  = {};
      Tempest::Vec3       animToe    = {};
      Tempest::Vec3       finalAnkle = {};
      float               groundY    = 0;  // world height of the ground under the foot, smoothed
      float               plant      = 1;  // 1: the foot stands on the ground, 0: it swings
      float               slack      = 0;  // how much lower the animated leg could still reach
      Tempest::Vec3       normal     = {0,1,0};  // the foot is tilted to it, smoothed
      Tempest::Vec3       heading    = {1,0,0};  // horizontal direction of the toes in the animation
      Tempest::Vec3       outDir     = {0,0,0};  // horizontal direction away from the other leg
      float               outward    = 0;        // radians the leg turns outward on a slope, smoothed
      float               kneeDev    = 0;  // debug: degrees between the animated and the solved knee bend
      float               tilt       = 0;  // debug: degrees the foot is tilted to the ground
      float               roll       = 0;  // debug: degrees the foot is rolled sideways
      float               yaw        = 0;  // debug: degrees the toes turned outward (+) from the animation
      };

    float offset(const Chain& c, float floorY) const;
    void  solve(Tempest::Matrix4x4* tr, const Chain& c, const Tempest::Vec3& target,
                const Tempest::Vec3& normal, float outward) const;

    Chain    legs[2];
    float    pelvis    = 0;  // how far the body is lowered, relative to the floor, smoothed
    float    lastFloor = 0;
    float    weight   = 0;
    bool     valid    = false;
    uint64_t logTimer = 0;
    uint64_t logTime  = 0;
  };
