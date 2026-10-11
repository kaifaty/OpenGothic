#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <string_view>
#include <vector>

class Skeleton;
class Npc;
class World;

// Foot IK: keeps the feet of a humanoid on uneven ground.
// Runs as a post-pass over the world-space bone matrices that Pose builds from animation:
// the whole body drops to the lower foot, each leg is bent by a two-bone solver
// and the feet are tilted to the ground normal.
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
      };

    struct Input final {
      bool     enable = false;
      float    floorY = 0; // height that the animation treats as ground (object origin)
      Ground   ground[2];
      uint64_t dt     = 0;
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
    void           apply(Tempest::Matrix4x4* tr, size_t numBones);

    void           debugLog(std::string_view name, const Input& in);

  private:
    struct Chain final {
      size_t              thigh = size_t(-1);
      size_t              calf  = size_t(-1);
      size_t              foot  = size_t(-1);
      size_t              toe   = size_t(-1);
      std::vector<size_t> thighTree, calfTree, footTree;

      Tempest::Vec3       animAnkle  = {};
      Tempest::Vec3       finalAnkle = {};
      float               offset     = 0;
      Tempest::Vec3       normal     = {0,1,0};
      };

    void solve(Tempest::Matrix4x4* tr, const Chain& c, const Tempest::Vec3& target, const Tempest::Vec3& normal) const;

    Chain    legs[2];
    float    pelvis   = 0;
    float    weight   = 0;
    bool     valid    = false;
    uint64_t logTimer = 0;
  };
