#pragma once

#include <memory>

// Connects the game to the external AI server (WebSocket + JSON, see docs/protocol.md in gothic-ai).
// Server address: env GOTHIC_AI_URL (default ws://127.0.0.1:47817); GOTHIC_AI=0 disables the bridge.
class AiBridge final {
  public:
    AiBridge();
    ~AiBridge();

    AiBridge(const AiBridge&) = delete;
    AiBridge& operator=(const AiBridge&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
  };
