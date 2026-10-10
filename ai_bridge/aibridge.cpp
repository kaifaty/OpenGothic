#include "aibridge.h"

#include <Tempest/Log>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <cctype>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "game/gamescript.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "gothic.h"

#include "chatoverlay.h"
#include "jsonread.h"
#include "jsonwriter.h"
#include "linelibrary.h"
#include "npccontroller.h"
#include "textcodec.h"

using namespace Tempest;

namespace {

constexpr int         ProtocolVersion = 2;
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

std::vector<std::string> split(std::string_view s) {
  std::vector<std::string> ret;
  std::istringstream       in{std::string(s)};
  std::string              tok;
  while(in >> tok)
    ret.push_back(tok);
  return ret;
  }

}

struct AiBridge::Impl final {
  struct Inbound final {
    enum Kind : uint8_t { Open, Close, Error, Message };
    Kind        kind = Message;
    std::string text;
    };

  Impl();
  ~Impl();

  void onTick(uint64_t dt);
  void onStartLoading();
  void onWorldLoaded();
  void onPerception(Npc& self, Npc& other, Npc* victim, PercType perc);
  bool onConsoleCommand(std::string_view cmd, std::string& reply);

  void handle(World* world, const std::string& text);
  void onSay(World& world, const rapidjson::Value& msg);
  void onChatStatus(const rapidjson::Value& msg);

  // chat with the npc the player looks at
  void ensureChat();
  bool canOpenChat();
  void openChat();
  void submitChat(const std::string& text);
  void closeChat(bool byPlayer);
  void onLineStarted(const LineLibrary::Line& line);
  void onNpcDetached(const std::string& symbol, std::string_view reason);

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

  LineLibrary                  lines;
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
  lines.onStart   = [this](const LineLibrary::Line& l) { onLineStarted(l); };
  ctrl.onDetached = [this](const std::string& npc, std::string_view reason) { onNpcDetached(npc,reason); };

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

  if(world!=nullptr)
    ctrl.tick(*world);
  ensureChat();
  chat->tick();
  }

void AiBridge::Impl::onStartLoading() {
  closeChat(false);
  lines.clear();
  if(auto world = Gothic::inst().world())
    ctrl.detachAll(*world,"loading"); else
    ctrl.dropAll("loading");
  send(JsonWriter("loading").finish());
  }

void AiBridge::Impl::onWorldLoaded() {
  sendWorld("world_loaded",Gothic::inst().world());
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
  else if(type=="chat_status") {
    onChatStatus(doc);
    }
  else {
    sendResult(req,false,"unknown message type: " + std::string(type));
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
  lines.add(line,symbol,TextCodec::fromUtf8(JsonRead::str(msg,"text")));
  npc->aiPush(AiQueue::aiOutput(*pl,line,0));
  }

void AiBridge::Impl::onChatStatus(const rapidjson::Value& msg) {
  if(chat==nullptr || chatNpc.empty() || JsonRead::str(msg,"npc")!=chatNpc)
    return;
  const auto status = JsonRead::str(msg,"status");
  if(status=="thinking")
    chat->setStatus(chatNpcName + TextCodec::fromUtf8(" думает..."));
  else if(status=="error")
    chat->setStatus(TextCodec::fromUtf8("Ошибка ИИ: ") + TextCodec::fromUtf8(JsonRead::str(msg,"detail")));
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
  chat->onClose       = [this]() { closeChat(true); };
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

void AiBridge::Impl::closeChat(bool byPlayer) {
  if(chat!=nullptr)
    chat->close();
  if(chatNpc.empty())
    return;
  const auto symbol = std::move(chatNpc);
  chatNpc.clear();
  if(!byPlayer)
    return;
  send(JsonWriter("chat_close").str("npc",symbol).finish());
  std::string err;
  if(auto world = Gothic::inst().world())
    ctrl.detach(*world,symbol,"chat_closed",err);
  }

void AiBridge::Impl::onLineStarted(const LineLibrary::Line& line) {
  send(JsonWriter("line_started").str("npc",line.npc).str("line",line.id).finish());
  if(chat==nullptr)
    return;
  std::string name = line.npc;
  if(auto world = Gothic::inst().world())
    if(auto npc = ctrl.find(*world,line.npc))
      name = std::string(npc->displayName());
  chat->addLine(std::move(name),line.text,line.time);
  }

void AiBridge::Impl::onNpcDetached(const std::string& symbol, std::string_view reason) {
  if(symbol!=chatNpc)
    return;
  closeChat(false);
  if(reason=="server_disconnected")
    Gothic::inst().onPrint(TextCodec::fromUtf8("Связь с ИИ-сервером потеряна"));
  }
