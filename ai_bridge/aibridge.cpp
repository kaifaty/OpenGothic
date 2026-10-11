#include "aibridge.h"

#include <Tempest/Application>
#include <Tempest/Log>
#include <Tempest/Timer>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/gamescript.h"
#include "game/serialize.h"
#include "commandline.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "world/worldsound.h"
#include "gothic.h"

#include "chatoverlay.h"
#include "jsonread.h"
#include "jsonwriter.h"
#include "linelibrary.h"
#include "npccontroller.h"
#include "textcodec.h"
#include "voicebuffer.h"

using namespace Tempest;

namespace {

constexpr int         ProtocolVersion = 4;
constexpr const char* SaveEntry       = "ai_bridge/session";  // own entry in save games
constexpr const char* DefaultUrl      = "ws://127.0.0.1:47817";

std::string lower(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return r;
  }

std::string npcSymbol(World& world, Npc& npc) {
  if(auto sym = world.script().findSymbol(npc.instanceSymbol()))
    return sym->name();
  return {};
  }

// random id of a line of play (see Session below)
std::string newBranchId() {
  static std::mt19937_64 rng{std::random_device{}() ^ uint64_t(std::time(nullptr))};
  static const char*     hex = "0123456789abcdef";
  std::string id;
  for(int i=0; i<2; ++i) {
    uint64_t v = rng();
    for(int k=0; k<16; ++k, v>>=4)
      id += hex[v&0xF];
    }
  return id;
  }

std::vector<uint8_t> fromBase64(std::string_view in) {
  static const auto table = [] {
    std::array<int8_t,256> t{};
    t.fill(-1);
    const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(int i=0; i<64; ++i)
      t[uint8_t(abc[i])] = int8_t(i);
    return t;
    }();
  std::vector<uint8_t> out;
  out.reserve(in.size()*3/4);
  uint32_t acc  = 0;
  int      bits = 0;
  for(char c:in) {
    const int8_t v = table[uint8_t(c)];
    if(v<0)
      continue;  // padding, line breaks
    acc   = (acc<<6) | uint32_t(v);
    bits += 6;
    if(bits>=8) {
      bits -= 8;
      out.push_back(uint8_t(acc>>bits));
      }
    }
  return out;
  }

// any npc of the world by script symbol (attached or not)
Npc* npcBySymbol(World& world, std::string_view symbol) {
  const size_t id = world.script().findSymbolIndex(symbol);
  return id==size_t(-1) ? nullptr : world.findNpcByInstance(id);
  }

std::vector<std::string> split(std::string_view s) {
  std::vector<std::string> ret;
  std::istringstream       in{std::string(s)};
  std::string              tok;
  while(in >> tok)
    ret.push_back(tok);
  return ret;
  }

}

struct Voice final {
  std::string                  line;
  std::string                  npc;
  std::shared_ptr<VoiceBuffer> buf;
  Tempest::SoundEffect         effect;
  uint64_t                     startedAt = 0;  // world ticks
  uint64_t                     drainedAt = 0;  // app ticks
  bool                         sized     = false;  // the line got its real length
  };

struct AiBridge::Impl final {
  struct Inbound final {
    enum Kind : uint8_t { Open, Close, Error, Message };
    Kind        kind = Message;
    std::string text;
    };

  Impl();
  ~Impl();

  // A session is one line of play: a new game or a loaded save starts a new branch. The server
  // keeps npc memories per branch; the memory head (`seq`) it reports goes into every save, and a
  // load branches off what the save remembers. World changes keep the branch.
  struct Session final {
    std::string branch;
    std::string parent;
    int64_t     parentSeq = 0;
    int64_t     seq       = 0;
    std::string reason;
    };

  void onTick(uint64_t dt);
  void onStartLoading();
  void onWorldLoaded();
  void takePendingSession();
  void startBranch(std::string parent, int64_t parentSeq, std::string_view reason);
  void sendSession();
  void updateSaveRecord();
  void onDebug(const rapidjson::Value& msg);
  void runDebug();
  void onPerception(Npc& self, Npc& other, Npc* victim, PercType perc);
  bool onConsoleCommand(std::string_view cmd, std::string& reply);

  void handle(World* world, const std::string& text);
  void onSay(World& world, const rapidjson::Value& msg);
  void onSayAudio(World& world, const rapidjson::Value& msg);
  // streamed voices: playback starts with the line, the line is shortened to the real length
  void startVoice(const LineLibrary::Line& line);
  void tickVoices(World& world);
  void correctBarrier(World& world, struct Voice& v);
  void onChatStatus(const rapidjson::Value& msg);

  // chat with the npc the player looks at
  void ensureChat();
  bool canOpenChat();
  void openChat();
  void submitChat(const std::string& text);
  // notifyServer: player closed it; release: hand the npc back to its routine (unless it follows the player)
  void closeChat(bool notifyServer, bool release);
  void onLineStarted(const LineLibrary::Line& line);
  void onNpcDetached(const std::string& symbol, std::string_view reason);
  void sendPtt(std::string_view type);

  void send(std::string&& text);
  void sendResult(std::string_view req, bool ok, std::string_view err);
  void sendWorld(std::string_view type, World* world);

  std::string          url;
  ix::WebSocket        ws;

  std::mutex           sync;
  std::vector<Inbound> inbound;

  bool                 connected = false;
  std::string          lastError;
  NpcController        ctrl{[this](std::string&& s){ send(std::move(s)); }};

  Session              session;
  bool                 saving = false;  // a save is in progress: the same world comes back
  // shared with the loading thread (save-game hooks)
  std::mutex           saveSync;
  std::string          saveRecord;      // written into the next save
  bool                 pendingSession  = false;
  bool                 pendingFromSave = false;
  std::string          pendingRecord;   // read from the loaded save

  // debug save/load must not run inside the game tick (it clears the world mid-frame):
  // it waits for the application's event loop
  Tempest::Timer       debugTimer;
  std::string          debugReq, debugCmd, debugSlot;

  LineLibrary                  lines;
  std::vector<struct Voice>    voices;
  std::unordered_map<std::string,std::string> lastLine;  // npc -> line it speaks (or spoke) last
  std::unique_ptr<ChatOverlay> chat;
  std::string                  chatNpc;      // symbol of the npc in chat, empty if closed
  std::string                  chatNpcName;  // game codepage
  };

AiBridge::Impl::Impl() {
  const char* envUrl = std::getenv("GOTHIC_AI_URL");
  url = (envUrl!=nullptr && envUrl[0]!='\0') ? envUrl : DefaultUrl;

  auto& gothic = Gothic::inst();
  gothic.onTick              .bind(this,&Impl::onTick);
  gothic.onStartLoading      .bind(this,&Impl::onStartLoading);
  gothic.onWorldLoaded       .bind(this,&Impl::onWorldLoaded);
  gothic.onExternalPerception.bind(this,&Impl::onPerception);
  gothic.consoleCommandFn = [this](std::string_view cmd, std::string& reply) {
    return onConsoleCommand(cmd,reply);
    };
  gothic.externalMessageFn = [this](std::string_view id, std::string_view& text, uint32_t& time) {
    return lines.lookup(id,text,time);
    };
  gothic.externalSoundFn = [this](std::string_view file, std::vector<uint8_t>& wav) {
    return lines.sound(file,wav);
    };
  // both run on the loading thread
  gothic.saveExtFn = [this](Serialize& fout) {
    std::lock_guard<std::mutex> guard(saveSync);
    fout.setEntry(SaveEntry);
    fout.write(saveRecord);
    };
  gothic.sessionStartFn = [this](Serialize* fin) {
    std::string rec;
    if(fin!=nullptr && fin->setEntry(SaveEntry))
      fin->read(rec);
    std::lock_guard<std::mutex> guard(saveSync);
    pendingSession  = true;
    pendingFromSave = fin!=nullptr;
    pendingRecord   = std::move(rec);
    };
  debugTimer.timeout.bind(this,&Impl::runDebug);
  lines.onStart   = [this](const LineLibrary::Line& l) { onLineStarted(l); };
  ctrl.onDetached = [this](const std::string& npc, std::string_view reason) { onNpcDetached(npc,reason); };
  ctrl.onFollowEnded = [this](const std::string& npc, std::string_view) {
    // following on its own ended: nobody talks to the npc, back to its routine
    std::string err;
    if(npc!=chatNpc)
      if(auto world = Gothic::inst().world())
        ctrl.detach(*world,npc,"follow_ended",err);
    };

  ix::initNetSystem();
  ws.setUrl(url);
  ws.setMinWaitBetweenReconnectionRetries(500);
  ws.setMaxWaitBetweenReconnectionRetries(3000);
  ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    // websocket thread: hand over to the main thread
    Inbound in;
    switch(msg->type) {
      case ix::WebSocketMessageType::Open:
        in.kind = Inbound::Open;
        break;
      case ix::WebSocketMessageType::Close:
        in.kind = Inbound::Close;
        in.text = msg->closeInfo.reason;
        break;
      case ix::WebSocketMessageType::Error:
        in.kind = Inbound::Error;
        in.text = msg->errorInfo.reason;
        break;
      case ix::WebSocketMessageType::Message:
        in.kind = Inbound::Message;
        in.text = msg->str;
        break;
      default:
        return;
      }
    std::lock_guard<std::mutex> guard(sync);
    inbound.push_back(std::move(in));
    });
  ws.start();
  Log::i("[ai] bridge started, server: ",url);
  }

AiBridge::Impl::~Impl() {
  auto& gothic = Gothic::inst();
  gothic.onTick              .ubind(this,&Impl::onTick);
  gothic.onStartLoading      .ubind(this,&Impl::onStartLoading);
  gothic.onWorldLoaded       .ubind(this,&Impl::onWorldLoaded);
  gothic.onExternalPerception.ubind(this,&Impl::onPerception);
  gothic.consoleCommandFn  = nullptr;
  gothic.externalMessageFn = nullptr;
  gothic.externalSoundFn   = nullptr;
  gothic.saveExtFn         = nullptr;
  gothic.sessionStartFn    = nullptr;
  voices.clear();  // before the game's sound device goes
  chat.reset();
  ws.stop();
  ix::uninitNetSystem();
  }

void AiBridge::Impl::onTick(uint64_t /*dt*/) {
  std::vector<Inbound> queue;
  {
    std::lock_guard<std::mutex> guard(sync);
    queue.swap(inbound);
  }

  World* world = Gothic::inst().world();
  for(auto& in:queue) {
    switch(in.kind) {
      case Inbound::Open:
        connected = true;
        lastError.clear();
        Log::i("[ai] connected to ",url);
        send(JsonWriter("hello")
               .integer("protocol",ProtocolVersion)
               .str("client","opengothic-ai-bridge")
               .str("codepage","cp1251")
               .finish());
        session.reason = "connect";
        sendSession();
        if(world!=nullptr)
          sendWorld("world_loaded",world);
        break;
      case Inbound::Close:
      case Inbound::Error:
        if(connected) {
          Log::i("[ai] disconnected: ",in.text);
          // nobody drives the npc's anymore: hand them back to their routines
          if(world!=nullptr)
            ctrl.detachAll(*world,"server_disconnected");
          }
        else if(in.kind==Inbound::Error && in.text!=lastError) {
          Log::i("[ai] connection failed: ",in.text);
          }
        connected = false;
        if(in.kind==Inbound::Error)
          lastError = in.text;
        break;
      case Inbound::Message:
        handle(world,in.text);
        break;
      }
    }

  if(world!=nullptr) {
    ctrl.tick(*world);
    tickVoices(*world);
    }
  ensureChat();
  chat->tick();
  }

void AiBridge::Impl::onStartLoading() {
  if(Gothic::inst().checkLoading()==Gothic::LoadState::Saving) {
    // the same session continues after saving: npc's stay attached, the chat stays open
    saving = true;
    return;
    }
  saving = false;
  closeChat(false,false);
  voices.clear();  // the world and its sound device are about to go
  lastLine.clear();
  lines.clear();
  if(auto world = Gothic::inst().world())
    ctrl.detachAll(*world,"loading"); else
    ctrl.dropAll("loading");
  send(JsonWriter("loading").finish());
  }

void AiBridge::Impl::onWorldLoaded() {
  if(saving) {
    saving = false;
    send(JsonWriter("saved").str("branch",session.branch).integer("seq",session.seq).finish());
    return;
    }
  takePendingSession();
  sendWorld("world_loaded",Gothic::inst().world());
  }

void AiBridge::Impl::takePendingSession() {
  bool        pending  = false;
  bool        fromSave = false;
  std::string rec;
  {
    std::lock_guard<std::mutex> guard(saveSync);
    std::swap(pending,pendingSession);
    fromSave = pendingFromSave;
    rec      = std::move(pendingRecord);
  }
  if(!pending) {
    // a world change: same line of play
    if(session.branch.empty())
      startBranch("",0,"new_game");
    return;
    }
  std::string parent;
  int64_t     parentSeq = 0;
  if(fromSave && !rec.empty()) {
    rapidjson::Document doc;
    doc.Parse(rec.c_str(),rec.size());
    if(!doc.HasParseError() && doc.IsObject()) {
      parent    = std::string(JsonRead::str(doc,"branch"));
      parentSeq = JsonRead::integer(doc,"seq",0);
      }
    }
  startBranch(std::move(parent),parentSeq,fromSave ? "load" : "new_game");
  }

void AiBridge::Impl::startBranch(std::string parent, int64_t parentSeq, std::string_view reason) {
  session.branch    = newBranchId();
  session.parent    = std::move(parent);
  session.parentSeq = session.parent.empty() ? 0 : parentSeq;
  session.seq       = session.parentSeq;
  session.reason    = std::string(reason);
  updateSaveRecord();
  if(session.parent.empty())
    Log::i("[ai] session ",session.branch," (",reason,")"); else
    Log::i("[ai] session ",session.branch," (",reason," from ",session.parent,"@",session.parentSeq,")");
  sendSession();
  }

void AiBridge::Impl::sendSession() {
  if(session.branch.empty())
    return;
  JsonWriter j("session");
  j.str("branch",session.branch);
  if(!session.parent.empty())
    j.str("parent",session.parent).integer("parent_seq",session.parentSeq);
  j.str("reason",session.reason);
  send(j.finish());
  }

void AiBridge::Impl::updateSaveRecord() {
  auto rec = JsonWriter("ai_session").str("branch",session.branch).integer("seq",session.seq).finish();
  std::lock_guard<std::mutex> guard(saveSync);
  saveRecord = std::move(rec);
  }

void AiBridge::Impl::onPerception(Npc& self, Npc& other, Npc* victim, PercType perc) {
  if(auto world = Gothic::inst().world())
    ctrl.perception(*world,self,other,victim,perc);
  }

void AiBridge::Impl::handle(World* world, const std::string& text) {
  rapidjson::Document doc;
  doc.Parse(text.c_str(),text.size());
  if(doc.HasParseError() || !doc.IsObject()) {
    Log::e("[ai] malformed message: ",text);
    return;
    }

  const auto type = JsonRead::str(doc,"type");
  const auto req  = JsonRead::str(doc,"req");

  if(type=="ping") {
    send(JsonWriter("pong").str("req",req).finish());
    return;
    }
  if(type=="memory") {
    if(JsonRead::str(doc,"branch")==session.branch) {
      session.seq = JsonRead::integer(doc,"seq",session.seq);
      updateSaveRecord();
      }
    return;
    }
  if(type=="debug") {
    onDebug(doc);
    return;
    }

  if(world==nullptr) {
    if(type=="action")
      send(JsonWriter("action_result")
             .str("id",JsonRead::str(doc,"id"))
             .str("npc",JsonRead::str(doc,"npc"))
             .str("status","rejected")
             .str("detail","no world loaded")
             .finish()); else
      sendResult(req,false,"no world loaded");
    return;
    }

  if(type=="attach") {
    std::string err;
    const bool  ok = ctrl.attach(*world,JsonRead::str(doc,"npc"),err);
    sendResult(req,ok,err);
    }
  else if(type=="detach") {
    std::string err;
    const bool  ok = ctrl.detach(*world,JsonRead::str(doc,"npc"),"server",err);
    sendResult(req,ok,err);
    }
  else if(type=="action") {
    ctrl.action(*world,doc);
    }
  else if(type=="snapshot") {
    ctrl.requestSnapshot(JsonRead::str(doc,"npc"));
    }
  else if(type=="say") {
    onSay(*world,doc);
    }
  else if(type=="chat_text") {
    // the reply as it is generated: shown right away, its lines voice it later
    if(chat!=nullptr) {
      const auto symbol = JsonRead::str(doc,"npc");
      const auto text   = TextCodec::fromUtf8(JsonRead::str(doc,"text"));
      Npc*       npc    = npcBySymbol(*world,symbol);
      chat->upsertLine(JsonRead::str(doc,"reply"),
                       npc!=nullptr ? std::string(npc->displayName()) : std::string(symbol),
                       text,LineLibrary::durationFor(text));
      }
    }
  else if(type=="say_audio" || type=="say_end") {
    onSayAudio(*world,doc);
    }
  else if(type=="chat_status") {
    onChatStatus(doc);
    }
  else if(type=="transcript") {
    // what the player said (push-to-talk), as the hero's line
    if(chat!=nullptr && !chatNpc.empty() && JsonRead::str(doc,"npc")==chatNpc && world->player()!=nullptr) {
      const auto text = TextCodec::fromUtf8(JsonRead::str(doc,"text"));
      if(!JsonRead::boolean(doc,"final",true)) {
        chat->setPartial(text);  // still speaking: text recognized so far
        } else {
        chat->setPartial("");
        chat->setStatus("");
        chat->addLine(std::string(world->player()->displayName()),text,LineLibrary::durationFor(text));
        }
      }
    }
  else if(type=="chat_end") {
    // the npc ends the conversation
    if(!chatNpc.empty() && JsonRead::str(doc,"npc")==chatNpc)
      closeChat(false,true);
    }
  else {
    sendResult(req,false,"unknown message type: " + std::string(type));
    }
  }

void AiBridge::Impl::onDebug(const rapidjson::Value& msg) {
  const auto req = JsonRead::str(msg,"req");
  if(!CommandLine::inst().isDevMode()) {
    sendResult(req,false,"debug commands need -devmode");
    return;
    }
  if(!debugReq.empty()) {
    sendResult(req,false,"another debug command is pending");
    return;
    }
  debugReq  = std::string(req);
  debugCmd  = std::string(JsonRead::str(msg,"cmd"));
  debugSlot = "save_slot_" + std::to_string(JsonRead::integer(msg,"slot",0)) + ".sav";
  debugTimer.start(1);
  }

void AiBridge::Impl::runDebug() {
  debugTimer.stop();
  const auto req  = std::move(debugReq);
  const auto cmd  = std::move(debugCmd);
  const auto slot = std::move(debugSlot);
  debugReq.clear();
  auto& gothic = Gothic::inst();
  if(gothic.checkLoading()!=Gothic::LoadState::Idle) {
    sendResult(req,false,"busy loading or saving");
    return;
    }
  if(cmd=="save") {
    // the game refuses to save during dialogs and cutscenes
    if(gothic.isInDialog() || (gothic.world()!=nullptr && gothic.world()->currentCs()!=nullptr)) {
      sendResult(req,false,"cannot save now (dialog or cutscene)");
      return;
      }
    gothic.save(slot,"AI test");
    sendResult(req,gothic.checkLoading()==Gothic::LoadState::Saving,"save did not start");
    }
  else if(cmd=="load") {
    gothic.load(slot);
    sendResult(req,true,"");
    }
  else {
    sendResult(req,false,"unknown debug command: " + cmd);
    }
  }

void AiBridge::Impl::send(std::string&& text) {
  if(!connected)
    return;
  ws.sendText(text);
  }

void AiBridge::Impl::sendResult(std::string_view req, bool ok, std::string_view err) {
  JsonWriter j("result");
  j.str("req",req).boolean("ok",ok);
  if(!ok)
    j.str("error",err);
  send(j.finish());
  }

void AiBridge::Impl::sendWorld(std::string_view type, World* world) {
  if(world==nullptr)
    return;
  send(JsonWriter(type).str("world",world->name()).finish());
  }

bool AiBridge::Impl::onConsoleCommand(std::string_view cmd, std::string& reply) {
  auto args = split(cmd);
  if(args.empty() || lower(args[0])!="ai")
    return false;

  World*     world = Gothic::inst().world();
  const auto sub   = args.size()>1 ? lower(args[1]) : std::string("status");

  if(sub=="status") {
    reply  = std::string(connected ? "connected to " : "NOT connected to ") + url;
    reply += "; " + ctrl.status(world);
    return true;
    }

  if(world==nullptr) {
    reply = "no world loaded";
    return true;
    }

  std::string err;
  if(sub=="attach" && args.size()>2) {
    if(ctrl.attach(*world,args[2],err))
      reply = "attached " + args[2] + (connected ? "" : " (server is not connected!)"); else
      reply = err;
    return true;
    }
  if(sub=="detach") {
    if(args.size()>2) {
      reply = ctrl.detach(*world,args[2],"console",err) ? "detached " + args[2] : err;
      } else {
      ctrl.detachAll(*world,"console");
      reply = "detached all";
      }
    return true;
    }

  reply = "usage: ai status | ai attach <npc> | ai detach [npc]";
  return true;
  }

AiBridge::AiBridge() {
  const char* env = std::getenv("GOTHIC_AI");
  if(env!=nullptr && std::string_view(env)=="0") {
    Log::i("[ai] bridge disabled (GOTHIC_AI=0)");
    return;
    }
  impl = std::make_unique<Impl>();
  }

AiBridge::~AiBridge() = default;

void AiBridge::Impl::onSay(World& world, const rapidjson::Value& msg) {
  const auto symbol = std::string(JsonRead::str(msg,"npc"));
  const auto line   = std::string(JsonRead::str(msg,"line"));
  Npc*       npc    = ctrl.find(world,symbol);
  Npc*       pl     = world.player();
  if(npc==nullptr || pl==nullptr || !LineLibrary::isExternal(line)) {
    Log::e("[ai] say: npc is not attached or bad line id: ",symbol," ",line);
    return;
    }
  auto wav   = fromBase64(JsonRead::str(msg,"audio"));
  auto voice = std::shared_ptr<VoiceBuffer>();
  if(JsonRead::boolean(msg,"stream",false))
    voice = std::make_shared<VoiceBuffer>(uint32_t(JsonRead::integer(msg,"rate",24000)));
  lines.add(line,symbol,TextCodec::fromUtf8(JsonRead::str(msg,"text")),std::move(wav),std::move(voice),
            std::string(JsonRead::str(msg,"reply")));
  npc->aiPush(AiQueue::aiOutput(*pl,line,0));
  }

void AiBridge::Impl::onSayAudio(World& world, const rapidjson::Value& msg) {
  auto* line = lines.find(JsonRead::str(msg,"line"));
  if(line==nullptr || line->voice==nullptr)
    return;
  if(JsonRead::str(msg,"type")=="say_audio") {
    const auto pcm = fromBase64(JsonRead::str(msg,"data"));
    line->voice->append(pcm.data(),pcm.size());
    return;
    }
  line->voice->finish();
  for(auto& v:voices)
    if(v.line==line->id)
      correctBarrier(world,v);
  }

void AiBridge::Impl::startVoice(const LineLibrary::Line& line) {
  World*       world   = Gothic::inst().world();
  GameSession* session = Gothic::inst().gameSession();
  Npc*         npc     = world!=nullptr ? npcBySymbol(*world,line.npc) : nullptr;
  if(npc==nullptr || session==nullptr)
    return;
  Voice v;
  v.line      = line.id;
  v.npc       = line.npc;
  v.buf       = line.voice;
  v.startedAt = world->tickCount();
  v.sized     = line.voice->isFinished();  // then the line already got the exact length
  v.effect    = session->loadSound(VoiceBuffer::producer(line.voice));
  if(!v.effect.isEmpty()) {
    v.effect.setPosition(npc->mapHeadBone());
    v.effect.setMaxDistance(WorldSound::talkRange);
    v.effect.setVolume(2.f);  // as WorldSound does for dialog speech
    v.effect.play();
    }
  Log::i("[ai] voice ",line.id," starts with ",line.voice->durationMs()," ms of audio",
         line.voice->isFinished() ? " (complete)" : " (streaming)");
  voices.push_back(std::move(v));
  }

void AiBridge::Impl::correctBarrier(World& world, Voice& v) {
  if(v.sized)
    return;
  v.sized = true;
  Npc* npc = npcBySymbol(world,v.npc);
  if(npc==nullptr || lastLine[v.npc]!=v.line)
    return;  // the npc has moved on
  const uint64_t end = v.startedAt + v.buf->durationMs() + LineLibrary::VoicePauseMs;
  const uint64_t now = world.tickCount();
  npc->setAiOutputBarrier(end>now ? end-now : 0, false);
  }

void AiBridge::Impl::tickVoices(World& world) {
  const uint64_t now = Application::tickCount();
  for(auto& v:voices) {
    if(v.buf->isFinished() && !v.sized)
      correctBarrier(world,v);
    if(!v.effect.isEmpty())
      if(auto npc = npcBySymbol(world,v.npc))
        v.effect.setPosition(npc->mapHeadBone());
    if(v.drainedAt==0 && v.buf->isDrained()) {
      v.drainedAt = now;
      Log::i("[ai] voice ",v.line," played, ",v.buf->durationMs()," ms");
      }
    }
  // keep the source a little after the last sample: OpenAL still holds a buffer of it
  voices.erase(std::remove_if(voices.begin(),voices.end(),[now](const Voice& v) {
    return v.drainedAt!=0 && now>v.drainedAt+500;
    }),voices.end());
  }

void AiBridge::Impl::onChatStatus(const rapidjson::Value& msg) {
  if(chat==nullptr || chatNpc.empty() || JsonRead::str(msg,"npc")!=chatNpc)
    return;
  const auto status = JsonRead::str(msg,"status");
  if(status=="thinking")
    chat->setStatus(chatNpcName + TextCodec::fromUtf8(" думает..."));
  else if(status=="recognizing")
    chat->setStatus(TextCodec::fromUtf8("Распознаю речь..."));
  else if(status=="notice" || status=="error") {
    chat->setPartial("");
    const auto detail = TextCodec::fromUtf8(JsonRead::str(msg,"detail"));
    chat->setStatus(status=="error" ? TextCodec::fromUtf8("Ошибка: ") + detail : detail);
    }
  else
    chat->setStatus("");
  }

void AiBridge::Impl::ensureChat() {
  if(chat!=nullptr)
    return;
  chat = std::make_unique<ChatOverlay>();
  chat->canOpen       = [this]() { return canOpenChat(); };
  chat->onOpenRequest = [this]() { openChat(); };
  chat->onSubmit      = [this](const std::string& text) { submitChat(text); };
  chat->onClose       = [this]() { closeChat(true,true); };
  chat->onPttStart    = [this]() { sendPtt("ptt_start"); };
  chat->onPttStop     = [this]() {
    sendPtt("ptt_stop");
    chat->setStatus(TextCodec::fromUtf8("Распознаю речь..."));
    };
  chat->onPttCancel   = [this]() { sendPtt("ptt_cancel"); };
  SystemApi::addOverlay(chat.get());
  }

bool AiBridge::Impl::canOpenChat() {
  auto&  gothic = Gothic::inst();
  World* world  = gothic.world();
  Npc*   pl     = world!=nullptr ? world->player() : nullptr;
  if(pl==nullptr || gothic.isPause() || gothic.isInDialog() || pl->isDown())
    return false;
  Npc* npc = pl->target();
  return npc!=nullptr && !npc->isPlayer() && !npc->isDown();
  }

void AiBridge::Impl::openChat() {
  World* world = Gothic::inst().world();
  Npc*   pl    = world!=nullptr ? world->player() : nullptr;
  Npc*   npc   = pl!=nullptr ? pl->target() : nullptr;
  if(npc==nullptr)
    return;
  if(!connected) {
    Gothic::inst().onPrint(TextCodec::fromUtf8("ИИ-сервер не подключён"));
    return;
    }

  const auto  symbol = npcSymbol(*world,*npc);
  std::string err;
  if(!ctrl.attach(*world,symbol,err)) {
    Gothic::inst().onPrint(err);
    return;
    }
  npc->aiPush(AiQueue::aiTurnToNpc(pl));

  chatNpc     = symbol;
  chatNpcName = std::string(npc->displayName());
  chat->open(chatNpcName);
  send(JsonWriter("chat_open").str("npc",symbol).str("name",TextCodec::toUtf8(chatNpcName)).finish());
  }

void AiBridge::Impl::submitChat(const std::string& text) {
  World* world = Gothic::inst().world();
  if(chatNpc.empty() || world==nullptr || world->player()==nullptr)
    return;
  chat->addLine(std::string(world->player()->displayName()),text,LineLibrary::durationFor(text));
  send(JsonWriter("player_text").str("npc",chatNpc).str("text",TextCodec::toUtf8(text)).finish());
  }

void AiBridge::Impl::closeChat(bool notifyServer, bool release) {
  if(chat!=nullptr)
    chat->close();
  if(chatNpc.empty())
    return;
  const auto symbol = std::move(chatNpc);
  chatNpc.clear();
  if(notifyServer)
    send(JsonWriter("chat_close").str("npc",symbol).finish());
  if(!release || ctrl.isFollowing(symbol))
    return;
  std::string err;
  if(auto world = Gothic::inst().world())
    ctrl.detach(*world,symbol,"chat_closed",err);
  }

void AiBridge::Impl::sendPtt(std::string_view type) {
  if(!chatNpc.empty())
    send(JsonWriter(type).str("npc",chatNpc).finish());
  }

void AiBridge::Impl::onLineStarted(const LineLibrary::Line& line) {
  const bool voiced = !line.wav.empty() || line.voice!=nullptr;
  send(JsonWriter("line_started").str("npc",line.npc).str("line",line.id).boolean("voiced",voiced).finish());
  lastLine[line.npc] = line.id;
  if(line.voice!=nullptr)
    startVoice(line);
  if(chat==nullptr)
    return;
  if(!line.reply.empty()) {
    chat->extend(line.reply,line.time);  // its text is on screen already
    return;
    }
  std::string name = line.npc;
  if(auto world = Gothic::inst().world())
    if(auto npc = ctrl.find(*world,line.npc))
      name = std::string(npc->displayName());
  chat->addLine(std::move(name),line.text,line.time);
  }

void AiBridge::Impl::onNpcDetached(const std::string& symbol, std::string_view reason) {
  if(symbol!=chatNpc)
    return;
  closeChat(false,false);
  if(reason=="server_disconnected")
    Gothic::inst().onPrint(TextCodec::fromUtf8("Связь с ИИ-сервером потеряна"));
  }
