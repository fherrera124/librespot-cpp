#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include "AudioSinkALSA.h"
#include "AuthInfo.h"
#include "ConnectReceiver.h"
#include "PlaybackNotifications.h"
#include "bell/Logger.h"

namespace {
const char* sessionFilePath = "session.json";

void printLocalControlHelp() {
  std::cout << "Local control: right=next, left=previous, space=play/pause, "
               "g=print position, q=quit\n";
}

// Raw terminal mode (Fase 1 CLI smoke test): reads arrow keys/space
// immediately, without waiting for Enter. Linux/POSIX-only, matching this
// target's own ALSA dependency.
struct termios originalTermios;
bool rawModeEnabled = false;

void restoreTerminal() {
  if (rawModeEnabled) {
    tcsetattr(STDIN_FILENO, TCSANOW, &originalTermios);
    rawModeEnabled = false;
  }
}

// Only SIGINT/SIGTERM are handled - anything else (SIGSEGV, etc.) isn't
// expected to leave the terminal in a recoverable state anyway. tcsetattr()/
// _Exit() are both async-signal-safe, so this is safe to run from a signal
// handler.
void handleTerminatingSignal(int sig) {
  restoreTerminal();
  std::_Exit(128 + sig);
}

void enableRawMode() {
  if (!isatty(STDIN_FILENO)) {
    std::cout << "stdin is not a tty - arrow keys won't work, falling back "
                 "to line mode (type a command, then Enter)\n";
    return;
  }
  if (tcgetattr(STDIN_FILENO, &originalTermios) != 0) {
    std::cout << "tcgetattr failed (" << strerror(errno)
              << ") - falling back to line mode\n";
    return;
  }
  std::atexit(restoreTerminal);
  std::signal(SIGINT, handleTerminatingSignal);
  std::signal(SIGTERM, handleTerminatingSignal);

  struct termios raw = originalTermios;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    std::cout << "tcsetattr failed (" << strerror(errno)
              << ") - falling back to line mode\n";
    return;
  }
  rawModeEnabled = true;
  std::cout << "raw mode enabled\n";
}

// kKeyUp/kKeyDown are recognized but deliberately unhandled below - no
// local volume control exists yet (neither AudioSinkALSA nor the engine
// expose one), out of scope for this smoke test.
enum SpecialKey { kKeyUp = 1000, kKeyDown, kKeyRight, kKeyLeft, kKeyEOF = -1 };

// Blocks for one keypress. Arrow keys arrive as a 3-byte ANSI escape
// sequence (ESC '[' A/B/C/D) - a lone Escape keypress (no follow-up bytes)
// or any other unrecognized escape sequence is swallowed, not misread as
// some other key.
int readKey() {
  for (;;) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
      return kKeyEOF;
    }
    if (c != 0x1b) {
      return static_cast<unsigned char>(c);
    }
    char seq[2];
    if (read(STDIN_FILENO, &seq[0], 1) != 1 ||
        read(STDIN_FILENO, &seq[1], 1) != 1) {
      return kKeyEOF;
    }
    if (seq[0] == '[') {
      switch (seq[1]) {
        case 'A':
          return kKeyUp;
        case 'B':
          return kKeyDown;
        case 'C':
          return kKeyRight;
        case 'D':
          return kKeyLeft;
      }
    }
    // Unrecognized escape sequence - loop back for the next real key.
  }
}

// Fase 2 smoke test: printed both for remote Spotify commands and for the
// local control above (e.g. TrackChanged fires either way, once the new
// track actually resolves - see StreamPlayer::announceState()'s own
// comment). Mirrors master's own extras/cli/main.cpp handleEngineEvent().
void handlePlaybackNotification(const cspot::PlaybackNotificationEvent& event) {
  using cspot::PlaybackNotification;
  switch (event.type) {
    case PlaybackNotification::TrackChanged: {
      auto& metadata = std::get<cspot::TrackMetadata>(event.data);
      std::cout << "Now playing: " << metadata.name << " - "
                << metadata.artist << "\n";
      break;
    }
    case PlaybackNotification::PlayPauseChanged: {
      bool paused = std::get<bool>(event.data);
      std::cout << (paused ? "Paused\n" : "Playing\n");
      break;
    }
    case PlaybackNotification::Seeked: {
      auto positionMs = std::get<uint32_t>(event.data);
      std::cout << "Seeked to " << positionMs << "ms\n";
      break;
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  bell::registerDefaultLogger();

  auto authInfo = std::make_shared<cspot::AuthInfo>("Cspot player");

  auto audioSink = std::make_shared<cspot::AudioSinkALSA>();

  cspot::ConnectReceiver receiver(authInfo, sessionFilePath, audioSink,
                                  handlePlaybackNotification);

  std::thread receiverThread([&receiver]() { receiver.run(); });

  // Local-control smoke test (Fase 1): drives ConnectReceiver's control API
  // directly from stdin, the same way any other embedding app would,
  // without going through a remote Spotify command. `paused` is this
  // harness's own guess at play/pause state for the toggle key - it isn't
  // read back from the device, so it can drift if playback also changes
  // remotely; good enough for a manual smoke test, not a real "now
  // playing" UI (that's Fase 2).
  enableRawMode();
  printLocalControlHelp();
  bool paused = false;
  bool quit = false;
  int key;
  while (!quit && (key = readKey()) != kKeyEOF) {
    switch (key) {
      case kKeyRight:
        std::cout << (receiver.requestNext() ? "next: ok\n"
                                             : "next: no active session\n");
        break;
      case kKeyLeft:
        std::cout << (receiver.requestPrevious()
                          ? "previous: ok\n"
                          : "previous: no active session\n");
        break;
      case ' ':
        paused = !paused;
        std::cout << (receiver.requestPlayPause(!paused)
                          ? (paused ? "paused\n" : "playing\n")
                          : "play/pause: no active session\n");
        break;
      case 'g':
        std::cout << "position: " << receiver.getPositionMs() << "ms\n";
        break;
      case 'q':
        quit = true;
        break;
      default:
        break;
    }
  }
  restoreTerminal();

  if (quit) {
    // Nothing left to coordinate with - same as killing the original
    // blocking run() via Ctrl+C, just triggered from inside the process.
    receiverThread.detach();
    return 0;
  }

  // stdin closed (no tty - e.g. running under a supervisor): fall back to
  // the original blocking behavior instead of exiting early.
  receiverThread.join();
  return 0;
}
