#include "voicebuffer.h"

#include <algorithm>
#include <cstring>

namespace {

class Producer final : public Tempest::SoundProducer {
  public:
    explicit Producer(std::shared_ptr<VoiceBuffer> buf)
      :SoundProducer(uint16_t(buf->rate()),1), buf(std::move(buf)) {}

    void renderSound(int16_t* out, size_t n) override {
      // Tempest's buffer callback passes the size in stereo frames (bytes/4) for every source;
      // the buffer of a mono source holds twice as many samples
      buf->render(out,2*n);
      }

  private:
    std::shared_ptr<VoiceBuffer> buf;
  };

}

void VoiceBuffer::append(const uint8_t* pcm16, size_t bytes) {
  std::lock_guard<std::mutex> guard(sync);
  const size_t at = pcm.size();
  pcm.resize(at + bytes/2);
  std::memcpy(pcm.data()+at, pcm16, (bytes/2)*2);
  }

void VoiceBuffer::finish() {
  std::lock_guard<std::mutex> guard(sync);
  ended = true;
  }

bool VoiceBuffer::isFinished() const {
  std::lock_guard<std::mutex> guard(sync);
  return ended;
  }

bool VoiceBuffer::isDrained() const {
  std::lock_guard<std::mutex> guard(sync);
  return ended && pos>=pcm.size();
  }

uint64_t VoiceBuffer::durationMs() const {
  std::lock_guard<std::mutex> guard(sync);
  return freq>0 ? uint64_t(pcm.size())*1000/freq : 0;
  }

void VoiceBuffer::render(int16_t* out, size_t n) {
  std::lock_guard<std::mutex> guard(sync);
  const size_t have = std::min(n, pcm.size()-pos);
  std::memcpy(out, pcm.data()+pos, have*sizeof(int16_t));
  std::memset(out+have, 0, (n-have)*sizeof(int16_t));
  pos += have;
  }

std::unique_ptr<Tempest::SoundProducer> VoiceBuffer::producer(std::shared_ptr<VoiceBuffer> buf) {
  return std::make_unique<Producer>(std::move(buf));
  }
