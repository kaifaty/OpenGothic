#include "npccontroller.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "game/gamescript.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "gothic.h"

#include "jsonread.h"
#include "jsonwriter.h"
#include "textcodec.h"

namespace {

constexpr uint64_t GraceMs          = 100;   // let the npc pick the action from its queue
constexpr uint64_t SnapshotPeriodMs = 500;
constexpr uint64_t PercThrottleMs   = 1000;
constexpr float    NearbyRadius     = 2000;  // cm
constexpr size_t   NearbyCount      = 6;

std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

using JsonRead::str;
using JsonRead::integer;

Npc* findNpc(World& world, std::string_view name) {
  if(name=="PLAYER" || name=="HERO")
    return world.player();
  const size_t id = world.script().findSymbolIndex(upper(name));
  if(id==size_t(-1))
    return nullptr;
  return world.findNpcByInstance(id);
  }

std::string npcId(World& world, Npc& npc) {
  if(npc.isPlayer())
    return "PLAYER";
  if(auto sym = world.script().findSymbol(npc.instanceSymbol()))
    return sym->name();
  return "?";
  }

std::string_view percName(PercType p) {
  static const char* names[PERC_Count] = {
    "NONE",
    "ASSESSPLAYER", "ASSESSENEMY", "ASSESSFIGHTER", "ASSESSBODY", "ASSESSITEM",
    "ASSESSMURDER", "ASSESSDEFEAT", "ASSESSDAMAGE", "ASSESSOTHERSDAMAGE", "ASSESSTHREAT",
    "ASSESSREMOVEWEAPON", "OBSERVEINTRUDER", "ASSESSFIGHTSOUND", "ASSESSQUIETSOUND", "ASSESSWARN",
    "CATCHTHIEF", "ASSESSTHEFT", "ASSESSCALL", "ASSESSTALK", "ASSESSGIVENITEM",
    "ASSESSFAKEGUILD", "MOVEMOB", "MOVENPC", "DRAWWEAPON", "OBSERVESUSPECT",
    "NPCCOMMAND", "ASSESSMAGIC", "ASSESSSTOPMAGIC", "ASSESSCASTER", "ASSESSSURPRISE",
    "ASSESSENTERROOM", "ASSESSUSEMOB",
    };
  if(p<PERC_Count)
    return names[p];
  return "UNKNOWN";
  }

void writePos(JsonWriter& j, std::string_view key, const Tempest::Vec3& p) {
  j.array(key).item(double(p.x)).item(double(p.y)).item(double(p.z)).endArray();
  }

}

NpcController::NpcController(Emit emit):emit(std::move(emit)) {
  }

NpcController::Controlled* NpcController::find(std::string_view symbol) {
  const auto s = upper(symbol);
  for(auto& c:npcs)
    if(c.symbol==s)
      return &c;
  return nullptr;
  }

const NpcController::Controlled* NpcController::find(std::string_view symbol) const {
  const auto s = upper(symbol);
  for(auto& c:npcs)
    if(c.symbol==s)
      return &c;
  return nullptr;
  }

bool NpcController::isAttached(std::string_view symbol) const {
  return find(symbol)!=nullptr;
  }

bool NpcController::attach(World& world, std::string_view symbolName, std::string& err) {
  const auto symbol = upper(symbolName);
  if(find(symbol)!=nullptr)
    return true;

  const size_t id  = world.script().findSymbolIndex(symbol);
  Npc*         npc = (id==size_t(-1)) ? nullptr : world.findNpcByInstance(id);
  if(npc==nullptr) {
    err = "npc not found in current world: " + symbol;
    return false;
    }
  if(npc->isPlayer()) {
    err = "cannot attach the player";
    return false;
    }
  if(npc->isDown()) {
    err = "npc is down";
    return false;
    }
  if(Gothic::inst().isNpcInDialog(*npc)) {
    err = "npc is in dialog";
    return false;
    }

  npc->clearAiQueue();
  npc->clearState(false);
  npc->setExternalControl(true);

  Controlled c;
  c.instance = id;
  c.symbol   = symbol;
  c.npc      = npc;
  npcs.push_back(std::move(c));

  emit(JsonWriter("attached")
         .str("npc",symbol)
         .str("name",TextCodec::toUtf8(npc->displayName()))
         .finish());
  emitSnapshot(world,npcs.back());
  return true;
  }

bool NpcController::detach(World& world, std::string_view symbol, std::string_view reason, std::string& err) {
  auto* c = find(symbol);
  if(c==nullptr) {
    err = "npc is not attached: " + upper(symbol);
    return false;
    }
  if(c->act)
    finishAction(*c,"cancelled","npc detached");
  // pointer may be stale if the npc was removed in between
  if(world.findNpcByInstance(c->instance)==c->npc)
    release(*c,true);
  emitDetached(c->symbol,reason);
  npcs.erase(npcs.begin()+(c-npcs.data()));
  return true;
  }

void NpcController::detachAll(World& world, std::string_view reason) {
  std::string err;
  while(!npcs.empty()) {
    auto symbol = npcs.back().symbol;
    detach(world,symbol,reason,err);
    }
  }

void NpcController::dropAll(std::string_view reason) {
  for(auto& c:npcs) {
    if(c.act)
      finishAction(c,"cancelled","world unloaded");
    emitDetached(c.symbol,reason);
    }
  npcs.clear();
  percThrottle.clear();
  }

void NpcController::release(Controlled& c, bool resumeRoutine) {
  if(c.npc==nullptr)
    return;
  c.npc->setExternalControl(false);
  if(resumeRoutine) {
    c.npc->clearAiQueue();
    c.npc->resumeAiRoutine();
    }
  }

void NpcController::action(World& world, const rapidjson::Value& msg) {
  const auto id     = std::string(str(msg,"id"));
  const auto symbol = upper(str(msg,"npc"));
  const auto type   = std::string(str(msg,"action"));

  auto* c = find(symbol);
  if(c==nullptr) {
    emitActionResult(id,symbol,"rejected","npc is not attached",-1);
    return;
    }

  static const rapidjson::Value emptyArgs(rapidjson::kObjectType);
  auto  argsIt = msg.FindMember("args");
  auto& args   = (argsIt!=msg.MemberEnd() && argsIt->value.IsObject()) ? argsIt->value : emptyArgs;

  if(c->act) {
    c->npc->clearAiQueue();
    finishAction(*c,"cancelled","preempted by " + id);
    }

  if(type=="return_to_routine") {
    emitActionResult(id,symbol,"done","",-1);
    std::string err;
    detach(world,symbol,"routine",err);
    return;
    }

  Action a;
  a.id    = id;
  a.type  = type;
  a.start = world.tickCount();

  std::string err;
  if(!startAction(world,*c,a,args,err)) {
    emitActionResult(id,symbol,"rejected",err,-1);
    return;
    }
  a.deadline = a.start + uint64_t(integer(args,"timeout_ms",int64_t(a.deadline)));
  c->act = std::move(a);
  }

bool NpcController::startAction(World& world, Controlled& c, Action& a, const rapidjson::Value& args, std::string& err) {
  Npc& npc = *c.npc;

  // a.deadline temporarily holds the default timeout, ms
  if(a.type=="go_to_waypoint") {
    auto name = str(args,"waypoint");
    auto wp   = world.findPoint(name);
    if(wp==nullptr) {
      err = "unknown waypoint: " + std::string(name);
      return false;
      }
    a.point    = wp;
    a.radius   = float(integer(args,"radius",300));
    a.deadline = 60000;
    npc.aiPush(AiQueue::aiGoToPoint(*wp));
    return true;
    }

  if(a.type=="go_to_npc" || a.type=="turn_to") {
    auto name   = str(args,"npc");
    Npc* target = findNpc(world,name);
    if(target==nullptr || target==&npc) {
      err = "unknown target npc: " + std::string(name);
      return false;
      }
    a.toPlayer = target->isPlayer();
    a.target   = target->instanceSymbol();
    if(a.type=="go_to_npc") {
      a.radius   = float(integer(args,"radius",400));
      a.deadline = 60000;
      npc.aiPush(AiQueue::aiGoToNpc(target));
      } else {
      a.deadline = 5000;
      npc.aiPush(AiQueue::aiTurnToNpc(target));
      }
    return true;
    }

  if(a.type=="play_ani") {
    auto name = upper(str(args,"name"));
    if(name.empty() || !npc.hasAnim(name)) {
      err = "unknown animation: " + name;
      return false;
      }
    a.deadline = 15000;
    npc.aiPush(AiQueue::aiPlayAnim(name));
    return true;
    }

  if(a.type=="wait") {
    auto ms = integer(args,"ms",1000);
    if(ms<=0 || ms>60000) {
      err = "wait ms must be in (0, 60000]";
      return false;
      }
    a.deadline = uint64_t(ms)+2000;
    npc.aiPush(AiQueue::aiWait(uint64_t(ms)));
    return true;
    }

  if(a.type=="stop") {
    npc.clearAiQueue();
    a.deadline = 1000;
    return true;
    }

  err = "unknown action: " + a.type;
  return false;
  }

void NpcController::updateAction(World& world, Controlled& c, uint64_t now) {
  auto& a = *c.act;
  if(now<a.start+GraceMs)
    return;

  if(c.npc->isAiBusy()) {
    if(now>=a.deadline) {
      c.npc->clearAiQueue();
      finishAction(c,"timeout","");
      }
    return;
    }

  if(a.point!=nullptr) {
    const double d = std::sqrt(double(c.npc->qDistTo(a.point)));
    if(d<=a.radius)
      finishAction(c,"done","",d); else
      finishAction(c,"failed","stopped away from waypoint",d);
    return;
    }

  if(a.type=="go_to_npc") {
    Npc* target = a.toPlayer ? world.player() : world.findNpcByInstance(a.target);
    if(target==nullptr) {
      finishAction(c,"failed","target npc is gone");
      return;
      }
    const double d = std::sqrt(double(c.npc->qDistTo(*target)));
    if(d<=a.radius)
      finishAction(c,"done","",d); else
      finishAction(c,"failed","stopped away from target",d);
    return;
    }

  finishAction(c,"done","");
  }

void NpcController::finishAction(Controlled& c, std::string_view status, std::string_view detail, double distance) {
  if(!c.act)
    return;
  emitActionResult(c.act->id,c.symbol,status,detail,distance);
  c.act.reset();
  c.nextSnapshot = 0;
  }

void NpcController::requestSnapshot(std::string_view symbol) {
  if(auto c = find(symbol))
    c->nextSnapshot = 0;
  }

void NpcController::tick(World& world) {
  const uint64_t now = world.tickCount();
  for(size_t i=0; i<npcs.size();) {
    auto& c   = npcs[i];
    Npc*  npc = world.findNpcByInstance(c.instance);
    if(npc==nullptr || npc!=c.npc) {
      if(c.act)
        finishAction(c,"cancelled","npc is gone");
      emitDetached(c.symbol,"lost");
      npcs.erase(npcs.begin()+ptrdiff_t(i));
      continue;
      }
    if(npc->isDown()) {
      // death/unconscious states are script-driven: hand the npc back without resuming routine
      if(c.act)
        finishAction(c,"failed","npc is down");
      release(c,false);
      emitDetached(c.symbol,"down");
      npcs.erase(npcs.begin()+ptrdiff_t(i));
      continue;
      }

    if(c.act)
      updateAction(world,c,now);
    if(now>=c.nextSnapshot) {
      emitSnapshot(world,c);
      c.nextSnapshot = now + SnapshotPeriodMs;
      }
    ++i;
    }
  }

void NpcController::perception(World& world, Npc& self, Npc& other, Npc* victim, PercType perc) {
  if(&other==&self)
    return; // e.g. MOVEMOB caused by the npc's own interaction
  auto it = std::find_if(npcs.begin(),npcs.end(),[&](const Controlled& c){ return c.npc==&self; });
  if(it==npcs.end())
    return;

  const uint64_t now = world.tickCount();
  const uint64_t key = (uint64_t(it->instance)<<40) ^ (uint64_t(other.instanceSymbol())<<8) ^ uint64_t(perc);
  auto& last = percThrottle[key];
  if(last!=0 && now<last+PercThrottleMs)
    return;
  last = now;

  JsonWriter j("perception");
  j.str("npc",it->symbol)
   .str("perc",percName(perc))
   .str("other",npcId(world,other))
   .num("distance",std::sqrt(double(self.qDistTo(other))));
  if(victim!=nullptr)
    j.str("victim",npcId(world,*victim));
  emit(j.finish());
  }

std::string NpcController::status(World* world) const {
  if(npcs.empty())
    return "no attached npc";
  std::string s;
  for(auto& c:npcs) {
    if(!s.empty())
      s += "; ";
    s += c.symbol;
    if(c.act)
      s += " [" + c.act->type + " " + c.act->id + "]"; else
      s += " [idle]";
    if(world!=nullptr && c.npc!=nullptr && world->findNpcByInstance(c.instance)==c.npc) {
      if(auto wp = c.npc->currentWayPoint())
        s += " at " + wp->name;
      }
    }
  return s;
  }

void NpcController::emitDetached(std::string_view symbol, std::string_view reason) {
  emit(JsonWriter("detached").str("npc",symbol).str("reason",reason).finish());
  }

void NpcController::emitActionResult(std::string_view id, std::string_view npc, std::string_view status,
                                     std::string_view detail, double distance) {
  JsonWriter j("action_result");
  j.str("id",id).str("npc",npc).str("status",status);
  if(!detail.empty())
    j.str("detail",detail);
  if(distance>=0)
    j.num("distance",distance);
  emit(j.finish());
  }

void NpcController::emitSnapshot(World& world, Controlled& c) {
  Npc&       npc  = *c.npc;
  const auto pos  = npc.position();
  const auto time = world.time();

  JsonWriter j("snapshot");
  j.integer("t",int64_t(world.tickCount()));

  j.object("world")
     .str("name",world.name())
     .integer("day",time.day())
     .integer("hour",time.hour())
     .integer("minute",time.minute())
   .end();

  j.object("npc")
     .str("id",c.symbol)
     .str("name",TextCodec::toUtf8(npc.displayName()));
  writePos(j,"pos",pos);
  j.num("rot",double(npc.rotation()))
   .integer("hp",npc.attribute(ATR_HITPOINTS))
   .integer("hp_max",npc.attribute(ATR_HITPOINTSMAX))
   .boolean("busy",npc.isAiBusy());
  if(c.act)
    j.str("action",c.act->type); else
    j.null("action");
  if(auto wp = npc.currentWayPoint())
    j.str("waypoint",wp->name); else
    j.null("waypoint");
  if(auto wp = world.findWayPoint(pos))
    j.str("nearest_waypoint",wp->name); else
    j.null("nearest_waypoint");
  j.end();

  if(auto pl = world.player()) {
    j.object("player")
       .str("name",TextCodec::toUtf8(pl->displayName()));
    writePos(j,"pos",pl->position());
    j.num("distance",std::sqrt(double(npc.qDistTo(*pl))))
     .end();
    }

  std::vector<const WayPoint*> nearby;
  j.array("nearby_waypoints");
  for(size_t i=0; i<NearbyCount; ++i) {
    auto wp = world.findWayPoint(pos,[&](const WayPoint& w) {
      if(std::find(nearby.begin(),nearby.end(),&w)!=nearby.end())
        return false;
      return (w.position()-pos).quadLength() < NearbyRadius*NearbyRadius;
      });
    if(wp==nullptr)
      break;
    nearby.push_back(wp);
    j.object()
       .str("name",wp->name)
       .num("distance",std::sqrt(double((wp->position()-pos).quadLength())))
     .end();
    }
  j.endArray();

  emit(j.finish());
  }
