#include "chatoverlay.h"

#include <Tempest/Application>
#include <Tempest/Painter>

#include <algorithm>
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
  update();
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

void ChatOverlay::setStatus(std::string text) {
  status = std::move(text);
  update();
  }

bool ChatOverlay::hasVisibleContent() const {
  return !log.empty() && log.back().shownUntil>Application::tickCount();
  }

void ChatOverlay::tick() {
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
  const float scale = Gothic::interfaceScale(this);
  auto&       text  = Resources::font(Resources::FontType::Normal,  scale);
  auto&       name  = Resources::font(Resources::FontType::Yellow,  scale);
  auto&       hint  = Resources::font(Resources::FontType::Disabled,scale);
  const int   lineH = text.pixelSize();
  const int   pad   = int(10*scale);
  const int   boxW  = std::min(w()-2*pad, int(900*scale));
  const int   textW = boxW-2*pad;
  const int   x0    = (w()-boxW)/2;

  int total = pad;
  for(auto r:rows)
    total += lineH + text.textSize(textW,r->text).h + pad/2;
  if(active)
    total += (status.empty() ? 0 : lineH) + lineH + pad;

  int y = h() - int(40*scale) - total;
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
  if(!status.empty()) {
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
  if(!active) {
    if(e.key==Event::K_T && e.modifier==Event::M_NoModifier && canOpen && canOpen()) {
      e.accept();
      openKeyDown = true;
      if(onOpenRequest)
        onOpenRequest();
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
  edit(e);
  }

void ChatOverlay::keyRepeatEvent(KeyEvent& e) {
  if(!active) {
    e.ignore();
    return;
    }
  e.accept();
  if(!openKeyDown)
    edit(e);
  }

void ChatOverlay::keyUpEvent(KeyEvent& e) {
  if(e.key==Event::K_T)
    openKeyDown = false;
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
      if(openKeyDown || e.code<32 || (e.modifier & (Event::M_Ctrl|Event::M_Command))!=0)
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
