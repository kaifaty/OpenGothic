#pragma once

#include <Tempest/UiOverlay>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

// Free-form chat with an npc, drawn over the game. Closed, it only shows recent npc lines
// (subtitles) and lets every input through except the chat key (T).
// All strings are in the game codepage.
class ChatOverlay final : public Tempest::UiOverlay {
  public:
    ChatOverlay();

    std::function<bool()>                   canOpen;
    std::function<void()>                   onOpenRequest;
    std::function<void(const std::string&)> onSubmit;
    std::function<void()>                   onClose;

    void open(std::string npcName);
    void close();
    bool isOpen() const { return active; }

    void addLine(std::string speaker, std::string text, uint32_t showMs);
    void setStatus(std::string text);
    void tick();

  protected:
    void paintEvent    (Tempest::PaintEvent& e) override;
    void keyDownEvent  (Tempest::KeyEvent&   e) override;
    void keyRepeatEvent(Tempest::KeyEvent&   e) override;
    void keyUpEvent    (Tempest::KeyEvent&   e) override;
    void mouseDownEvent(Tempest::MouseEvent& e) override;
    void mouseUpEvent  (Tempest::MouseEvent& e) override;
    void mouseMoveEvent(Tempest::MouseEvent& e) override;
    void mouseWheelEvent(Tempest::MouseEvent& e) override;

  private:
    struct Entry final {
      std::string speaker;
      std::string text;
      uint64_t    shownUntil = 0;
      };

    void edit(Tempest::KeyEvent& e);
    bool hasVisibleContent() const;

    bool               active      = false;
    bool               openKeyDown = false;
    bool               wasVisible  = false;
    std::string        npcName;
    std::string        input;
    size_t             cursor = 0;
    std::string        status;
    std::deque<Entry>  log;
  };
