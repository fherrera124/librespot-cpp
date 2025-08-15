#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

// Library includes
#include "SessionContext.h"
#include "bell/Result.h"
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
  ApConnection(std::shared_ptr<SessionContext> sessionContext);
  ~ApConnection();

  // Type for the packet handler function
  using ConnectionPacketHandler =
      std::function<void(uint8_t packetType, const uint8_t* data, size_t len)>;

  /**
   * @brief Connects to the AP, address fetched from the credential resolver
   */
  bell::Result<> connect();

  /**
   * @brief Sends a shannon encrypted packet to the AP
   *
   * @param cmd Packet command
   * @param packetData Buffer containing the packet data
   * @param packetSize Size of the packet data
   */
  bell::Result<> sendPacket(uint8_t cmd, const uint8_t* packetData,
                            uint16_t packetSize);

  /**
   * @brief Receives a shannon encrypted packet from the AP
   *
   * @param cmd Reference to the packet command byte
   * @param packetSize Reference to the packet size
   * @return uint8_t* Buffer containing the received packet data
   */
  bell::Result<uint8_t*> receivePacket(uint8_t& cmd, uint16_t& packetSize);

  /**
   * @brief Assigns a handler called when an encrypted packet is received
   */
  void setPacketHandler(ConnectionPacketHandler handler);

  /**
   * @brief Returns the underlying socket used for the connection
   */
  std::shared_ptr<bell::net::TCPSocket> getSocket() { return apSock; }

 private:
  const char* LOG_TAG = "ApConnection";
  const static uint32_t operationTimeout = 3000;

  std::shared_ptr<SessionContext> sessionContext;
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

  std::vector<uint8_t> connectionBuffer;

  // Holds the initially transferred messages, used for the handshake challenge
  std::vector<uint8_t> accumulatedExchangeBuffer;

  // Packet handler for incoming packets
  ConnectionPacketHandler packetHandler = nullptr;

  void handleRead();

  bell::Result<> sendClientHelloPacket();
  bell::Result<> solveHelloChallenge(const uint8_t* apResponsePacket,
                                     size_t apResponsePacketSize);

  bell::Result<> sendPlainPacket(const uint8_t* data, size_t len,
                                 std::optional<uint16_t> cmd);

  bell::Result<size_t> receivePlainPacket();

  /**
   * @brief Authenticates the connection with the AP, using data from login blob
   */
  bell::Result<> authenticate();

  static void updateShannonNonce(uint32_t& nonce, Shannon& cipher);
};
}  // namespace cspot
