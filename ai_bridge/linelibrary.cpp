#include "linelibrary.h"

#include <algorithm>

uint32_t LineLibrary::durationFor(std::string_view text) {
  // ~15 characters per second of reading, plus a breath
  const auto t = 800 + 65*uint32_t(text.size());
  return std::clamp<uint32_t>(t, 1500, 12000);
  }

void LineLibrary::add(std::string id, std::string npc, std::string text) {
  Line l;
  l.time = durationFor(text);
  l.id   = std::move(id);
  l.npc  = std::move(npc);
  l.text = std::move(text);
  lines.push_back(std::move(l));
  // the engine holds string_view's into the text only while a line is being started,
  // old lines are long finished when they drop out
  while(lines.size()>MaxLines)
    lines.pop_front();
  }

bool LineLibrary::lookup(std::string_view id, std::string_view& text, uint32_t& time) {
  if(!isExternal(id))
    return false;
  for(auto it=lines.rbegin(); it!=lines.rend(); ++it) {
    if(it->id!=id)
      continue;
    text = it->text;
    time = it->time;
    if(!it->started) {
      it->started = true;
      if(onStart)
        onStart(*it);
      }
    return true;
    }
  // unknown external line (e.g. after load): speak nothing, briefly
  text = "";
  time = 1;
  return true;
  }
