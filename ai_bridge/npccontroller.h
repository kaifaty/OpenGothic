#pragma once

#include <rapidjson/document.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/constants.h"

class Npc;
class World;
class WayPoint;

// NPCs taken off script control (routines, perceptions) and driven by the external AI server.
// One action in flight per NPC; a new action preempts the current one.
class NpcController final {
  public:
    using Emit = std::function<void(std::string&&)>;

    explicit NpcController(Emit emit);

    bool attach(World& world, std::string_view symbol, std::string& err);
    bool detach(World& world, std::string_view symbol, std::string_view reason, std::string& err);
    void detachAll(World& world, std::string_view reason);
    // world is about to be destroyed: forget npc's without touching them
    void dropAll(std::string_view reason);

    void action(World& world, const rapidjson::Value& msg);
    void requestSnapshot(std::string_view symbol);
    void tick(World& world);
    void perception(World& world, Npc& self, Npc& other, Npc* victim, PercType perc);

    bool        isAttached(std::string_view symbol) const;
    std::string status(World* world) const;

  private:
    struct Action final {
      std::string     id;
      std::string     type;
      uint64_t        start    = 0;
      uint64_t        deadline = 0;
      const WayPoint* point    = nullptr;
      size_t          target   = 0;     // instance of target npc
      bool            toPlayer = false;
      float           radius   = 0;     // success radius for movement, cm
      };

    struct Controlled final {
      size_t                instance = 0;
      std::string           symbol;
      Npc*                  npc      = nullptr;
      std::optional<Action> act;
      uint64_t              nextSnapshot = 0;
      };

    Controlled* find(std::string_view symbol);
    const Controlled* find(std::string_view symbol) const;

    bool startAction(World& world, Controlled& c, Action& a, const rapidjson::Value& args, std::string& err);
    void updateAction(World& world, Controlled& c, uint64_t now);
    void finishAction(Controlled& c, std::string_view status, std::string_view detail, double distance = -1);
    void release(Controlled& c, bool resumeRoutine);

    void emitDetached(std::string_view symbol, std::string_view reason);
    void emitActionResult(std::string_view id, std::string_view npc, std::string_view status,
                          std::string_view detail, double distance);
    void emitSnapshot(World& world, Controlled& c);

    Emit                                   emit;
    std::vector<Controlled>                npcs;
    std::unordered_map<uint64_t,uint64_t>  percThrottle;
  };
