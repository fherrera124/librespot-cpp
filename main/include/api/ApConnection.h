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

  // Type for the packet handler function
  using ConnectionPacketHandler = std::function<void(
      uint8_t packetType, const std::byte* data, size_t len)>;

  /**
   * @brief Connects to the AP, address fetched from the credential resolver
   */
  bell::Result<> connect(
      const std::string& apAddress,
      const std::shared_ptr<bell::SocketPollListener>& socketPoll);

  /**
   * @brief Sends a shannon encrypted packet to the AP
   *
   * @param cmd Packet command
   * @param packetData Buffer containing the packet data
   * @param packetSize Size of the packet data
   */
  bell::Result<> sendPacket(uint8_t cmd, const std::byte* packetData,
                            uint16_t packetSize);

  /**
   * @brief Receives a shannon encrypted packet from the AP
   *
   * @param cmd Reference to the packet command byte
   * @param packetSize Reference to the packet size
   * @return uint8_t* Buffer containing the received packet data
   */
  bell::Result<std::byte*> receivePacket(uint8_t& cmd, uint16_t& packetSize);

  /**
   * @brief Assigns a handler called when an encrypted packet is received
   */
  void setPacketHandler(ConnectionPacketHandler handler);

  /**
   * @brief Authenticates the connection with the AP, using data from login blob
   */
  bell::Result<> authenticate(
      const cspot_proto::LoginCredentials& loginCredentials,
      const std::string& deviceId);

  /**
   * @brief Returns the underlying socket used for the connection
   */
  std::shared_ptr<bell::net::TCPSocket> getSocket() { return apSock; }

 private:
  const char* LOG_TAG = "ApConnection";
  const static uint32_t operationTimeout = 3000;

  std::shared_ptr<cspot::AuthInfo> authInfo;
  std::shared_ptr<bell::net::TCPSocket> apSock;
  DH dhPair;

  // Nonce counters for Shannon ciphers
  uint32_t shanRecvNonce = 0;
  uint32_t shanSendNonce = 0;

  Shannon recvCipher{};
  Shannon sendCipher{};

  // Connection state machine enumeration
  enum class State { INITIAL, SENT_HELLO, CONNECTED_SHANNON, ERROR };

  State state = State::INITIAL;

  // Protobufs
  cspot_proto::ClientHello pbClientHello{};
  cspot_proto::APResponseMessage pbApResponse{};
  cspot_proto::ClientResponsePlaintext pbClientResponse{};
  cspot_proto::ClientResponseEncrypted pbClientResponseEncrypted{};

  std::vector<std::byte> connectionBuffer;

  // Holds the initially transferred messages, used for the handshake challenge
  std::vector<std::byte> accumulatedExchangeBuffer;

  // Packet handler for incoming packets
  ConnectionPacketHandler packetHandler = nullptr;

  void handleRead();

  bell::Result<> sendClientHelloPacket();
  bell::Result<> solveHelloChallenge(const std::byte* apResponsePacket,
                                     size_t apResponsePacketSize);

  bell::Result<> sendPlainPacket(const std::byte* data, size_t len,
                                 std::optional<uint16_t> cmd);

  bell::Result<size_t> receivePlainPacket();

  // Reads exactly `len` bytes, transparently retrying on EAGAIN/EWOULDBLOCK
  // (apSock is non-blocking - see the connect(..., timeoutMs=0) call site -
  // so a read finding 0 bytes currently available is normal, not an error,
  // and used to be treated as fatal here: receivePacket()/receivePlainPacket()
  // read a multi-byte packet across a handful of individual read() calls,
  // and if the rest of the packet hadn't arrived from the wire yet, that
  // transient EAGAIN killed the whole AP connection permanently (state
  // dropped to ERROR), which is exactly what a real hardware session hit -
  // every single audio key request failing with "Not owner" (this
  // toolchain's newlib strerror() text for EPERM, returned by
  // sendPacket()/receivePacket() once state != CONNECTED_SHANNON) because
  // the connection had already silently died within the first second or
  // two. Bounded by operationTimeout so a genuinely dead connection still
  // surfaces as a real error instead of hanging forever.
  bell::Result<> readExact(std::byte* buf, size_t len);

  static void updateShannonNonce(uint32_t& nonce, Shannon& cipher);
};
}  // namespace cspot
