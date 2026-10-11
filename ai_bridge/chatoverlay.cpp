#include "chatoverlay.h"

#include <Tempest/Application>
#include <Tempest/Painter>

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "utils/gthfont.h"
#include "gothic.h"
#include "resources.h"

#include "textcodec.h"

using namespace Tempest;

namespace {

constexpr size_t   MaxInput       = 300;
constexpr size_t   MaxLog         = 32;
constexpr size_t   OpenRows       = 6;   // entries shown while chatting
constexpr size_t   SubtitleRows   = 2;   // entries shown as subtitles when closed
constexpr uint64_t LingerMs       = 2500;
constexpr uint64_t PttHoldMs      = 300;    // T held longer than this is push-to-talk
constexpr uint64_t PttMaxMs       = 30000;  // the key-up may never come (focus lost)
constexpr auto     FontName       = "font_old_20_white.tga"; // the large game font, has cyrillic in RU

// GOTHIC_AI_CHAT_SCALE: extra size multiplier for the chat, on top of the game's interface scale
// the end of `text` that fits into `width` (live transcripts grow at the end)
std::string fitTail(const GthFont& font, std::string text, int width) {
  if(font.textSize(text).w<=width)
    return text;
  while(!text.empty() && font.textSize("..."+text).w>width) {
    const size_t sp = text.find(' ',1);
    text.erase(0,sp==std::string::npos ? 1 : sp);
    }
  return "..."+text;
  }

float chatScale() {
  static const float v = [] {
    const char* env = std::getenv("GOTHIC_AI_CHAT_SCALE");
    const float f   = env!=nullptr ? std::strtof(env,nullptr) : 0.f;
    return f>0.f ? f : 1.f;
    }();
  return v;
  }

}

ChatOverlay::ChatOverlay() {
  }

void ChatOverlay::open(std::string name) {
  active  = true;
  npcName = std::move(name);
  input.clear();
  cursor  = 0;
  status.clear();
  update();
  }

void ChatOverlay::close() {
  active = false;
  status.clear();
  partial.clear();
  pttReset();  // the server drops the recording together with the chat
  update();
  }

void ChatOverlay::setPartial(std::string text) {
  partial = std::move(text);
  update();
  }

void ChatOverlay::pttPress(char pendingChar) {
  partial.clear();
  tDown        = true;
  tDownAt      = Application::tickCount();
  tPendingChar = pendingChar;
  if(onPttStart)
    onPttStart();
  }

void ChatOverlay::pttRelease() {
  if(!tDown)
    return;
  const bool spoke = recording;
  const char typed = tPendingChar;
  pttReset();
  if(spoke) {
    if(onPttStop)
      onPttStop();
    return;
    }
  if(onPttCancel)
    onPttCancel();
  if(active && typed!='\0' && input.size()<MaxInput) {
    input.insert(cursor,1,typed);
    ++cursor;
    }
  }

void ChatOverlay::pttReset() {
  tDown        = false;
  recording    = false;
  tPendingChar = '\0';
  }

void ChatOverlay::addLine(std::string speaker, std::string text, uint32_t showMs) {
  Entry e;
  e.speaker    = std::move(speaker);
  e.text       = std::move(text);
  e.shownUntil = Application::tickCount() + showMs + LingerMs;
  log.push_back(std::move(e));
  while(log.size()>MaxLog)
    log.pop_front();
  update();
  }

void ChatOverlay::upsertLine(std::string_view key, std::string speaker, std::string text, uint32_t showMs) {
  for(auto it=log.rbegin(); it!=log.rend(); ++it) {
    if(it->key!=key)
      continue;
    it->text       = std::move(text);
    it->shownUntil = std::max(it->shownUntil, Application::tickCount() + showMs + LingerMs);
    update();
    return;
    }
  addLine(std::move(speaker),std::move(text),showMs);
  log.back().key = std::string(key);
  }

void ChatOverlay::extend(std::string_view key, uint32_t showMs) {
  for(auto it=log.rbegin(); it!=log.rend(); ++it)
    if(it->key==key) {
      it->shownUntil = std::max(it->shownUntil, Application::tickCount() + showMs + LingerMs);
      update();
      return;
      }
  }

void ChatOverlay::setStatus(std::string text) {
  status = std::move(text);
  update();
  }

bool ChatOverlay::hasVisibleContent() const {
  return !log.empty() && log.back().shownUntil>Application::tickCount();
  }

void ChatOverlay::tick() {
  if(tDown && active) {
    const uint64_t held = Application::tickCount() - tDownAt;
    if(!recording && held>=PttHoldMs)
      recording = true;
    if(recording && held>=PttMaxMs)
      pttRelease();
    }
  // repaint while something is on screen (cursor blink, subtitles expiring), and once after it is gone
  const bool visible = active || hasVisibleContent();
  if(visible || wasVisible)
    update();
  wasVisible = visible;
  }

void ChatOverlay::paintEvent(PaintEvent& e) {
  const uint64_t now = Application::tickCount();
  std::vector<const Entry*> rows;
  for(auto it=log.rbegin(); it!=log.rend(); ++it) {
    if(active ? rows.size()>=OpenRows : (rows.size()>=SubtitleRows || it->shownUntil<=now))
      break;
    rows.insert(rows.begin(),&*it);
    }
  if(!active && rows.empty())
    return;

  Painter     p(e);
  const float scale = Gothic::interfaceScale(this)*chatScale();
  auto&       text  = Resources::font(FontName,Resources::FontType::Normal,  scale);
  auto&       name  = Resources::font(FontName,Resources::FontType::Yellow,  scale);
  auto&       hint  = Resources::font(FontName,Resources::FontType::Disabled,scale);
  auto&       rec   = Resources::font(FontName,Resources::FontType::Red,     scale);
  const int   lineH = text.pixelSize();
  const int   pad   = std::max(8,lineH/2);
  // ~75 characters per line of the large font, at least 60% of the window
  const int   boxW  = std::min(w()-2*pad, std::max(int(1000*scale), w()*3/5));
  const int   textW = boxW-2*pad;
  const int   x0    = (w()-boxW)/2;

  int total = pad;
  for(auto r:rows)
    total += lineH + text.textSize(textW,r->text).h + pad/2;
  if(active)
    total += (status.empty() && partial.empty() && !recording ? 0 : lineH) + lineH + pad;

  int y = h() - 2*lineH - total;
  p.setBrush(Color(0.f,0.f,0.f,0.6f));
  p.drawRect(x0,y,boxW,total);

  y += pad;
  for(auto r:rows) {
    name.drawText(p,x0+pad,y+lineH,r->speaker);
    y += lineH;
    text.drawText(p,x0+pad,y+lineH,textW,0,r->text,AlignLeft);
    y += text.textSize(textW,r->text).h + pad/2;
    }

  if(!active)
    return;
  if(recording) {
    static const std::string label = TextCodec::fromUtf8("Запись: ");
    static const std::string speak = TextCodec::fromUtf8("говорите... отпустите T, чтобы отправить");
    rec.drawText(p,x0+pad,y+lineH,label+fitTail(rec,partial.empty() ? speak : partial,textW-rec.textSize(label).w));
    y += lineH;
    }
  else if(!partial.empty()) {
    static const std::string label = TextCodec::fromUtf8("Распознаю: ");
    hint.drawText(p,x0+pad,y+lineH,label+fitTail(hint,partial,textW-hint.textSize(label).w));
    y += lineH;
    }
  else if(!status.empty()) {
    hint.drawText(p,x0+pad,y+lineH,status);
    y += lineH;
    }
  const std::string prompt = "> ";
  text.drawText(p,x0+pad,y+lineH,prompt+input);
  // blinking cursor
  const int   cx = x0+pad + text.textSize(prompt+input.substr(0,cursor)).w;
  const float a  = float(now%1000)/1000.f;
  p.setBrush(Color(1.f,1.f,1.f,a<0.5f ? 1.f : 0.f));
  p.drawRect(cx,y+2,std::max(1,int(scale)),lineH-4);
  }

void ChatOverlay::keyDownEvent(KeyEvent& e) {
  const bool plainT = e.key==Event::K_T && e.modifier==Event::M_NoModifier;
  if(!active) {
    if(plainT && canOpen && canOpen()) {
      e.accept();
      if(onOpenRequest)
        onOpenRequest();
      if(active)
        pttPress('\0');  // held, it is push-to-talk right away
      return;
      }
    e.ignore();
    return;
    }
  if(Event::K_F1<=e.key && e.key<=Event::K_F12) {
    e.ignore();
    return;
    }
  e.accept();
  if(tDown && e.key!=Event::K_ESCAPE)
    return;
  if(plainT && input.empty()) {
    // a word may start with this letter: decided on release (short tap) or after the hold delay
    pttPress(TextCodec::encode(e.code));
    return;
    }
  edit(e);
  }

void ChatOverlay::keyRepeatEvent(KeyEvent& e) {
  if(!active) {
    e.ignore();
    return;
    }
  e.accept();
  if(!tDown)
    edit(e);
  }

void ChatOverlay::keyUpEvent(KeyEvent& e) {
  if(e.key==Event::K_T)
    pttRelease();
  if(active)
    e.accept(); else
    e.ignore();
  }

void ChatOverlay::edit(KeyEvent& e) {
  switch(e.key) {
    case Event::K_ESCAPE:
      if(onClose)
        onClose();
      return;
    case Event::K_Return:
      if(!input.empty() && onSubmit) {
        const std::string txt = input;
        input.clear();
        cursor = 0;
        onSubmit(txt);
        }
      break;
    case Event::K_Back:
      if(cursor>0)
        input.erase(--cursor,1);
      break;
    case Event::K_Delete:
      if(cursor<input.size())
        input.erase(cursor,1);
      break;
    case Event::K_Left:
      if(cursor>0)
        --cursor;
      break;
    case Event::K_Right:
      if(cursor<input.size())
        ++cursor;
      break;
    case Event::K_Home:
      cursor = 0;
      break;
    case Event::K_End:
      cursor = input.size();
      break;
    default: {
      if(e.code<32 || (e.modifier & (Event::M_Ctrl|Event::M_Command))!=0)
        break;
      const char c = TextCodec::encode(e.code);
      if(c!='\0' && input.size()<MaxInput) {
        input.insert(cursor,1,c);
        ++cursor;
        }
      break;
      }
    }
  update();
  }

void ChatOverlay::mouseDownEvent(MouseEvent& e) {
  if(active) e.accept(); else e.ignore();
  }

void ChatOverlay::mouseUpEvent(MouseEvent& e) {
  if(active) e.accept(); else e.ignore();
  }

void ChatOverlay::mouseMoveEvent(MouseEvent& e) {
  if(active) e.accept(); else e.ignore();
  }

void ChatOverlay::mouseWheelEvent(MouseEvent& e) {
  if(active) e.accept(); else e.ignore();
  }
