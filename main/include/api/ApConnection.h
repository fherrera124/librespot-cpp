#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

// Library includes
#include "AuthInfo.h"
#include "bell/Result.h"
#include "bell/net/SocketPollListener.h"
#include "bell/net/TCPSocket.h"

// Own includes
#include "crypto/DiffieHellman.h"
#include "crypto/Shannon.h"

// Protobufs
#include "proto/AuthenticationPb.h"
#include "proto/KeyexchangePb.h"

namespace cspot {
class ApConnection {
 public:
  ApConnection(const std::shared_ptr<cspot::AuthInfo>& authInfo);

  using ConnectionPacketHandler = std::function<void(
      uint8_t packetType, const std::byte* data, size_t len)>;

  // Connects to the AP, address fetched from the credential resolver.
  bell::Result<> connect(
      const std::string& apAddress,
      const std::shared_ptr<bell::SocketPollListener>& socketPoll);

  // Sends a Shannon-encrypted packet to the AP.
  bell::Result<> sendPacket(uint8_t cmd, const std::byte* packetData,
                            uint16_t packetSize);

  // Receives a Shannon-encrypted packet from the AP.
  bell::Result<std::byte*> receivePacket(uint8_t& cmd, uint16_t& packetSize);

  void setPacketHandler(ConnectionPacketHandler handler);

  // Authenticates the connection with the AP, using data from the login blob.
  bell::Result<> authenticate(
      const cspot_proto::LoginCredentials& loginCredentials,
      const std::string& deviceId);

  std::shared_ptr<bell::net::TCPSocket> getSocket() { return apSock; }

  bool isConnected() const { return state == State::CONNECTED_SHANNON; }
  bool hasFailed() const { return state == State::ERROR; }

  // Tears down the connection (closes the socket, unregisters it from the
  // poller) and marks it failed, same as an internal error would. Used by
  // ApClient's ping watchdog to force a reconnect when the AP has gone
  // silently dead (socket still looks fine, no error ever fires).
  void disconnect();

 private:
  const char* LOG_TAG = "ApConnection";
  // inline: odr-used below via std::chrono::milliseconds(operationTimeout),
  // which binds by reference - plain "const static" without an
  // out-of-class definition links only as long as every use stays a pure
  // rvalue, and stops as soon as one doesn't.
  const static inline uint32_t operationTimeout = 3000;

  std::shared_ptr<cspot::AuthInfo> authInfo;
  std::shared_ptr<bell::net::TCPSocket> apSock;
  std::shared_ptr<bell::SocketPollListener> socketPoll;

  // unique_ptr, not a plain member: DH wraps mbedtls_mpi structs freed in
  // its destructor with no user-defined move/copy, so regenerating it on
  // every (re)connect via plain assignment (`dhPair = DH()`) would
  // shallow-copy those pointers and double-free/dangle. Replacing the
  // pointer instead destroys the old instance cleanly.
  std::unique_ptr<DH> dhPair = std::make_unique<DH>();

  uint32_t shanRecvNonce = 0;
  uint32_t shanSendNonce = 0;

  Shannon recvCipher{};
  Shannon sendCipher{};

  enum class State { INITIAL, SENT_HELLO, CONNECTED_SHANNON, ERROR };

  State state = State::INITIAL;

  cspot_proto::ClientHello pbClientHello{};
  cspot_proto::APResponseMessage pbApResponse{};
  cspot_proto::ClientResponsePlaintext pbClientResponse{};
  cspot_proto::ClientResponseEncrypted pbClientResponseEncrypted{};

  std::vector<std::byte> connectionBuffer;

  // Holds the initially transferred messages, used for the handshake challenge
  std::vector<std::byte> accumulatedExchangeBuffer;

  ConnectionPacketHandler packetHandler = nullptr;

  void handleRead();

  bell::Result<> sendClientHelloPacket();
  bell::Result<> solveHelloChallenge(const std::byte* apResponsePacket,
                                     size_t apResponsePacketSize);

  bell::Result<> sendPlainPacket(const std::byte* data, size_t len,
                                 std::optional<uint16_t> cmd);

  bell::Result<size_t> receivePlainPacket();

  // Reads exactly `len` bytes, transparently retrying on EAGAIN/EWOULDBLOCK
  // (apSock is non-blocking, so a read finding 0 bytes currently available
  // is normal, not an error). Bounded by operationTimeout so a genuinely
  // dead connection still surfaces as a real error instead of hanging
  // forever.
  bell::Result<> readExact(std::byte* buf, size_t len);

  static void updateShannonNonce(uint32_t& nonce, Shannon& cipher);
};
}  // namespace cspot
