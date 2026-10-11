#include "linelibrary.h"

#include <algorithm>
#include <cstring>

namespace {

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }

// how long a line whose voice is still streaming in may last (the real length shortens it later)
uint32_t streamedTime(const LineLibrary::Line& l) {
  const uint64_t audio = l.voice->durationMs();
  if(l.voice->isFinished())
    return uint32_t(audio) + LineLibrary::VoicePauseMs;
  return std::max<uint32_t>(LineLibrary::durationFor(l.text)*3/2, uint32_t(audio) + 1000);
  }

}

uint32_t LineLibrary::wavDuration(const std::vector<uint8_t>& wav) {
  if(wav.size()<12 || std::memcmp(wav.data(),"RIFF",4)!=0 || std::memcmp(wav.data()+8,"WAVE",4)!=0)
    return 0;
  uint32_t byteRate = 0;
  for(size_t at=12; at+8<=wav.size();) {
    const uint32_t size = le32(wav.data()+at+4);
    if(std::memcmp(wav.data()+at,"fmt ",4)==0 && at+8+16<=wav.size()) {
      byteRate = le32(wav.data()+at+8+8);
      }
    else if(std::memcmp(wav.data()+at,"data",4)==0) {
      const uint32_t data = uint32_t(std::min<size_t>(size,wav.size()-at-8));
      return byteRate>0 ? uint32_t(uint64_t(data)*1000/byteRate) : 0;
      }
    at += 8 + size + (size&1);
    }
  return 0;
  }

uint32_t LineLibrary::durationFor(std::string_view text) {
  // ~15 characters per second of reading, plus a breath
  const auto t = 800 + 65*uint32_t(text.size());
  return std::clamp<uint32_t>(t, 1500, 12000);
  }

void LineLibrary::add(std::string id, std::string npc, std::string text, std::vector<uint8_t> wav,
                      std::shared_ptr<VoiceBuffer> voice, std::string reply) {
  Line l;
  const uint32_t audio = wavDuration(wav);
  if(audio==0)
    wav.clear();
  l.time = audio>0 ? audio + VoicePauseMs : durationFor(text);
  l.wav   = std::move(wav);
  l.voice = std::move(voice);
  l.reply = std::move(reply);
  l.id   = std::move(id);
  l.npc  = std::move(npc);
  l.text = std::move(text);
  lines.push_back(std::move(l));
  // the engine holds string_view's into the text only while a line is being started,
  // old lines are long finished when they drop out
  while(lines.size()>MaxLines)
    lines.pop_front();
  }

LineLibrary::Line* LineLibrary::find(std::string_view id) {
  for(auto it=lines.rbegin(); it!=lines.rend(); ++it)
    if(it->id==id)
      return &*it;
  return nullptr;
  }

bool LineLibrary::sound(std::string_view file, std::vector<uint8_t>& wav) const {
  if(!isExternal(file))
    return false;
  const auto id = file.substr(0,file.find('.'));
  for(auto it=lines.rbegin(); it!=lines.rend(); ++it) {
    if(it->id!=id)
      continue;
    if(it->wav.empty())
      return false;
    wav = it->wav;
    return true;
    }
  return false;
  }

bool LineLibrary::lookup(std::string_view id, std::string_view& text, uint32_t& time) {
  if(!isExternal(id))
    return false;
  for(auto it=lines.rbegin(); it!=lines.rend(); ++it) {
    if(it->id!=id)
      continue;
    text = it->text;
    time = it->voice!=nullptr ? streamedTime(*it) : it->time;
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
