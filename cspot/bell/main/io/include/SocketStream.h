#pragma once

#include <iostream>  // for streamsize, basic_streambuf<>::int_type, ios...
#include <memory>    // for unique_ptr, operator!=
#include <string>    // for char_traits, string

#include "BellSocket.h"  // for Socket

namespace bell {
class SocketBuffer : public std::streambuf {
 private:
  std::unique_ptr<bell::Socket> internalSocket;

  static const int bufLen = 1024;
  char ibuf[bufLen], obuf[bufLen];

 public:
  SocketBuffer() { internalSocket = nullptr; }

  SocketBuffer(const std::string& hostname, int port, bool isSSL = false) {
    // Was `open(hostname, port)` - silently dropped the caller's isSSL,
    // always opening plain TCP regardless of what was passed. No current
    // caller (this constructor is unused today), but a real trap for the
    // next one. See docs/dealer_websocket_migration.md §48.
    open(hostname, port, isSSL);
  }

  int open(const std::string& hostname, int port, bool isSSL = false);

  int close();

  bool isOpen() {
    return internalSocket != nullptr && internalSocket->isOpen();
  }

  // Underlying descriptor (-1 if closed) - lets an owner with a reader
  // thread blocked in read() unblock it via ::shutdown() BEFORE close()
  // destroys the socket object under that reader's feet (use-after-free
  // otherwise). No current caller needs this (it was added for the
  // now-removed PortableWebSocketTransport::disconnect(), which had a
  // separate reader thread - WebSocketTransport.cpp's single-task design
  // doesn't); kept for the next owner with the same reader-thread shape.
  int getFd() {
    return internalSocket != nullptr ? internalSocket->getFd() : -1;
  }

  ~SocketBuffer() { close(); }

 protected:
  virtual int sync();

  virtual int_type underflow();

  virtual int_type overflow(int_type c = traits_type::eof());

  virtual std::streamsize xsgetn(char_type* __s, std::streamsize __n);

  virtual std::streamsize xsputn(const char_type* __s, std::streamsize __n);
};

class SocketStream : public std::iostream {
 private:
  SocketBuffer socketBuf;

 public:
  SocketStream() : std::iostream(&socketBuf) {}

  SocketStream(const std::string& hostname, int port, bool isSSL = false)
      : std::iostream(&socketBuf) {
    open(hostname, port, isSSL);
  }

  SocketBuffer* rdbuf() { return &socketBuf; }

  int open(const std::string& hostname, int port, bool isSSL = false) {
    int err = socketBuf.open(hostname, port, isSSL);
    if (err)
      setstate(std::ios::failbit);
    return err;
  }

  int close() { return socketBuf.close(); }

  bool isOpen() { return socketBuf.isOpen(); }

  int getFd() { return socketBuf.getFd(); }
};
}  // namespace bell
