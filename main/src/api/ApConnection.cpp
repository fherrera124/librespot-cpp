#include "api/ApConnection.h"

#include <chrono>

#include "Utils.h"
#include "authentication.pb.h"
#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/utils/DigestCrypto.h"
#include "bell/utils/Utils.h"
#include "mbedtls/md.h"
#include "tl/expected.hpp"

using namespace cspot;

namespace {
const long long SPOTIFY_VERSION = 0x10800000000;
const size_t shannonMacSize = 4;
}  // namespace

ApConnection::ApConnection(const std::shared_ptr<cspot::AuthInfo>& authInfo)
    : authInfo(authInfo) {}

bell::Result<> ApConnection::connect(
    const std::string& apAddress,
    const std::shared_ptr<bell::SocketPollListener>& socketPoll) {
  BELL_LOG(debug, LOG_TAG, "Connecting to AP");
  apSock = std::make_unique<bell::net::TCPSocket>();

  // Split the address into hostname and port
  auto colonPos = apAddress.find(':');
  if (colonPos == std::string::npos) {
    return bell::make_unexpected_errc(std::errc::invalid_argument);
  }

  auto hostname = apAddress.substr(0, colonPos);
  auto portStr = apAddress.substr(colonPos + 1);

  // Connect to the AP
  auto res = apSock->connect(hostname, std::stoi(portStr), 0);

  if (!res) {
    BELL_LOG(error, LOG_TAG, "Could not connect to AP at {}: {}", apAddress,
             res.error());
    return tl::make_unexpected(res.error());
  }

  // Register readable listener
  socketPoll->registerSocket(apSock, bell::PollEvent::Readable,
                             [this](auto& /*sock*/) { this->handleRead(); });

  // Register writeable / connected listener
  socketPoll->registerSocket(
      apSock, bell::PollEvent::Writeable, [socketPoll, this](auto& /*sock*/) {
        // SO_ERROR is get-and-clear (socket(7)): reading it resets the
        // pending error to 0 as a side effect. Calling lastError() twice
        // here (once for the check, once for the log) meant the second
        // call always saw an already-cleared 0/"Success", masking the
        // real error - reproduced on real hardware ("AP connection
        // error: Success"). Capture it once, like DealerClient.cpp and
        // TCPSocket.cpp already do.
        auto err = apSock->lastError();
        if (err) {
          BELL_LOG(error, LOG_TAG, "AP connection error: {}", err);
          state = State::ERROR;
          apSock->close();
          return;
        }

        BELL_LOG(info, LOG_TAG, "AP connection established");
        if (state == State::INITIAL) {
          auto res = sendClientHelloPacket();
          if (!res) {
            BELL_LOG(error, LOG_TAG, "Could not send ClientHello packet: {}",
                     res.error());
            state = State::ERROR;
          } else {
            state = State::SENT_HELLO;
          }
        }

        // We are connected, unregister the writeable event
        socketPoll->unregisterSocket(apSock, bell::PollEvent::Writeable);
      });

  return {};
}

void ApConnection::handleRead() {
  if (state == State::ERROR) {
    BELL_LOG(error, LOG_TAG, "Connection is in error state, cannot read");
    apSock->close();
    return;
  }

  // Read the incoming packet
  if (state == State::CONNECTED_SHANNON) {
    // Receive shannon encrypted packet
    uint8_t cmd = 0;
    uint16_t packetSize = 0;
    auto packetResult = receivePacket(cmd, packetSize);
    if (!packetResult) {
      BELL_LOG(error, LOG_TAG, "Could not receive packet: {}",
               packetResult.error());
      state = State::ERROR;
      return;
    }

    BELL_LOG(debug, LOG_TAG, "Received packet with cmd: {}, size: {}", cmd,
             packetSize);

    // Call the packet handler if set
    if (packetHandler) {
      packetHandler(cmd, *packetResult, packetSize);
    }
  } else if (state == State::SENT_HELLO) {
    auto packetResult = receivePlainPacket();
    if (!packetResult) {
      BELL_LOG(error, LOG_TAG, "Could not receive packet: {}",
               packetResult.error());
      state = State::ERROR;
      return;
    }

    // We received the AP response, now we can solve the challenge
    auto res = solveHelloChallenge(connectionBuffer.data(), *packetResult);
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Could not solve hello challenge: {}",
               res.error());
      state = State::ERROR;
      return;
    }

    BELL_LOG(info, LOG_TAG, "Hello challenge solved successfully");
    state = State::CONNECTED_SHANNON;

    if (authInfo->loginCredentials) {
      res = authenticate(*authInfo->loginCredentials, authInfo->deviceId);

      if (!res) {
        BELL_LOG(error, LOG_TAG, "Could not authenticate with AP: {}",
                 res.error());
        state = State::ERROR;
        return;
      }
    } else {
      BELL_LOG(error, LOG_TAG,
               "No login credentials available for authentication");
      state = State::ERROR;
      return;
    }
  }
}

bell::Result<> ApConnection::sendClientHelloPacket() {
  pbClientHello = {};

  // Prepare the ClientHello message
  pbClientHello.loginCryptoHello.diffieHellman.value.serverKeysKnown = 1;
  pbClientHello.loginCryptoHello.diffieHellman.hasValue = true;

  pbClientHello.buildInfo.product = Product_PRODUCT_CLIENT;
  pbClientHello.buildInfo.platform = Platform2_PLATFORM_LINUX_X86;
  pbClientHello.buildInfo.version = SPOTIFY_VERSION;
  pbClientHello.featureSet.autoupdate2 = true;
  pbClientHello.cryptosuitesSupported.push_back(
      Cryptosuite_CRYPTO_SUITE_SHANNON);
  pbClientHello.padding.push_back(std::byte{0x1E});

  // Copy the public key into the ClientHello message
  auto publicKey = dhPair.getPublicKey();
  auto& pbGcArr = pbClientHello.loginCryptoHello.diffieHellman.value.gc;
  assert(publicKey.size() == pbGcArr.size());
  std::copy(publicKey.begin(), publicKey.end(), pbGcArr.begin());

  // Fill nonce with random data
  for (std::byte& nonceByte : pbClientHello.clientNonce) {
    nonceByte = static_cast<std::byte>(rand() % 256);
  }

  std::vector<std::byte> encodedHelloPacket = {};
  auto encodeRes =
      nanopb_helper::encodeToVector(pbClientHello, encodedHelloPacket);

  if (!encodeRes) {
    BELL_LOG(error, LOG_TAG, "Could not encode ClientHello message");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  // Send the packet
  auto res = sendPlainPacket(encodedHelloPacket.data(),
                             encodedHelloPacket.size(), 0x04);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Could not send ClientHello packet, {} len {}",
             res.error(), encodedHelloPacket.size());
    return tl::make_unexpected(res.error());
  }

  return {};
}

bell::Result<> ApConnection::solveHelloChallenge(
    const std::byte* apResponsePacket, size_t apResponsePacketSize) {

  bool res = nanopb_helper::decodeFromBuffer(pbApResponse, apResponsePacket,
                                             apResponsePacketSize);

  if (!res) {
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  std::array<std::byte, 96> sharedKey{};

  // Compute the diffie hellman shared key based on the response
  dhPair.computeSharedKey(pbApResponse.challenge.value.loginCryptoChallenge
                              .diffieHellman.value.gs.data(),
                          96, sharedKey.data());

  // Init client packet + Init server packets are required for the hmac challenge
  accumulatedExchangeBuffer.push_back(
      std::byte{0x00});  // Add a terminator byte

  bell::utils::DigestCrypto sha1Context{MBEDTLS_MD_SHA1};

  std::array<std::byte, 100> challengeResult{};
  // Solve the hmac challenge
  for (uint8_t x = 0; x < 5; x++) {
    accumulatedExchangeBuffer[accumulatedExchangeBuffer.size() - 1] =
        static_cast<std::byte>(x + 1);

    // Calculate the hmac
    sha1Context.getHmac(
        sharedKey.data(), sharedKey.size(), accumulatedExchangeBuffer.data(),
        accumulatedExchangeBuffer.size(), &challengeResult[x * 20]);
  }

  std::array<std::byte, 20> responseHmac{};
  sha1Context.getHmac(
      challengeResult.data(), 20, accumulatedExchangeBuffer.data(),
      accumulatedExchangeBuffer.size() - 1, responseHmac.data());

  pbClientResponse.loginCryptoResponse.diffieHellman.hasValue = true;
  std::copy(
      responseHmac.begin(), responseHmac.end(),
      pbClientResponse.loginCryptoResponse.diffieHellman.value.hmac.data());

  std::array<std::byte, 32> shanSendKey{};
  std::array<std::byte, 32> shanRecvKey{};

  // Shan send key = [0x14:0x34]
  std::copy(challengeResult.begin() + 0x14, challengeResult.begin() + 0x34,
            shanSendKey.begin());

  // Shan recv key = [0x34:0x54]
  std::copy(challengeResult.begin() + 0x34, challengeResult.begin() + 0x54,
            shanRecvKey.begin());

  state = State::CONNECTED_SHANNON;
  res = nanopb_helper::encodeToVector(pbClientResponse,
                                      accumulatedExchangeBuffer);
  if (!res) {
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  // Send the response
  auto sendRes =
      sendPlainPacket(accumulatedExchangeBuffer.data(),
                      accumulatedExchangeBuffer.size(), std::nullopt);
  if (!sendRes) {
    return tl::make_unexpected(sendRes.error());
  }

  // At this point, the handshake is complete, and the connection is no longer plaintext
  BELL_LOG(info, LOG_TAG, "Handshake complete");

  // Initialize the Shannon ciphers
  recvCipher.key(shanRecvKey.data(), shanRecvKey.size());
  sendCipher.key(shanSendKey.data(), shanSendKey.size());

  // Reset the nonce for the Shannon ciphers
  shanRecvNonce = 0;
  shanSendNonce = 0;
  updateShannonNonce(shanRecvNonce, recvCipher);
  updateShannonNonce(shanSendNonce, sendCipher);

  return {};
}

bell::Result<> ApConnection::sendPlainPacket(const std::byte* data, size_t len,
                                             std::optional<uint16_t> cmd) {
  // Send the packet size
  uint32_t packetSize = htonl(len + 4 + (cmd.has_value() ? 2 : 0));
  if (cmd.has_value()) {
    uint32_t prefix = htons(cmd.value());
    auto res = apSock->write(reinterpret_cast<const std::byte*>(&prefix),
                             sizeof(uint16_t));
    if (!res) {
      return tl::make_unexpected(res.error());
    }

    if (state != State::CONNECTED_SHANNON) {
      accumulatedExchangeBuffer.insert(
          accumulatedExchangeBuffer.end(),
          reinterpret_cast<const std::byte*>(&prefix),
          reinterpret_cast<const std::byte*>(&prefix) + sizeof(uint16_t));
    }
  }
  auto res = apSock->write(reinterpret_cast<const std::byte*>(&packetSize),
                           sizeof(packetSize));
  if (!res) {
    return tl::make_unexpected(res.error());
  }

  if (state != State::CONNECTED_SHANNON) {
    accumulatedExchangeBuffer.insert(
        accumulatedExchangeBuffer.end(),
        reinterpret_cast<const std::byte*>(&packetSize),
        reinterpret_cast<const std::byte*>(&packetSize) + sizeof(packetSize));
  }

  // Send the packet data
  res = apSock->write(data, len);
  if (!res) {
    return tl::make_unexpected(res.error());
  }

  if (state != State::CONNECTED_SHANNON) {
    accumulatedExchangeBuffer.insert(accumulatedExchangeBuffer.end(), data,
                                     data + len);
  }

  // Ensure the accumulated exchange buffer does not exceed a certain size
  assert(accumulatedExchangeBuffer.size() <= 1024);

  return {};
}

bell::Result<> ApConnection::authenticate(
    const cspot_proto::LoginCredentials& loginCredentials,
    const std::string& deviceId) {

  // Prepare the authentication request
  pbClientResponseEncrypted.loginCredentials = loginCredentials;
  pbClientResponseEncrypted.systemInfo.cpuFamily = CpuFamily_CPU_UNKNOWN;
  pbClientResponseEncrypted.systemInfo.os = Os_OS_UNKNOWN;
  pbClientResponseEncrypted.systemInfo.systemInformationString = "cspot-player";
  pbClientResponseEncrypted.systemInfo.deviceId = deviceId;
  pbClientResponseEncrypted.versionString = "cspot-1.1";

  // Encode the ClientResponseEncrypted message
  std::vector<std::byte> encodedResponse;
  auto encodeRes =
      nanopb_helper::encodeToVector(pbClientResponseEncrypted, encodedResponse);
  if (!encodeRes) {
    BELL_LOG(error, LOG_TAG,
             "Could not encode ClientResponseEncrypted message");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  // Send the authentication packet
  auto res = sendPacket(0xAB, encodedResponse.data(), encodedResponse.size());
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Could not send authentication packet");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  return {};
}

void ApConnection::setPacketHandler(ConnectionPacketHandler handler) {
  packetHandler = std::move(handler);
}

bell::Result<size_t> ApConnection::receivePlainPacket() {
  uint32_t packetSize = 0;

  // Not using a BinaryStream here, as we only really need to read a single uint32_t
  auto res = readExact(reinterpret_cast<std::byte*>(&packetSize),
                       sizeof(packetSize));

  if (!res) {
    return tl::make_unexpected(res.error());
  }

  if (state != State::CONNECTED_SHANNON) {
    accumulatedExchangeBuffer.insert(
        accumulatedExchangeBuffer.end(),
        reinterpret_cast<std::byte*>(&packetSize),
        reinterpret_cast<std::byte*>(&packetSize) + sizeof(packetSize));
  }

  packetSize = ntohl(packetSize);

  // TODO: Verify maximum packet size
  if (packetSize > connectionBuffer.size()) {
    connectionBuffer.resize(packetSize);
  }

  // Already read the packet size, so subtract it from the total size
  packetSize -= 4;

  res = readExact(connectionBuffer.data(), packetSize);
  if (!res) {
    return tl::make_unexpected(res.error());
  }

  if (state != State::CONNECTED_SHANNON) {
    accumulatedExchangeBuffer.insert(accumulatedExchangeBuffer.end(),
                                     connectionBuffer.data(),
                                     connectionBuffer.data() + packetSize);
  }

  // Ensure the accumulated exchange buffer does not exceed a certain size
  assert(accumulatedExchangeBuffer.size() <= 1024);
  return packetSize;
}

bell::Result<> ApConnection::readExact(std::byte* buf, size_t len) {
  size_t readBytes = 0;
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(operationTimeout);

  while (readBytes < len) {
    auto res = apSock->read(buf + readBytes, len - readBytes);
    if (!res) {
      if (res.error() == std::errc::operation_would_block ||
          res.error() == std::errc::interrupted) {
        if (std::chrono::steady_clock::now() >= deadline) {
          return bell::make_unexpected_errc(std::errc::timed_out);
        }
        bell::utils::sleepMs(1);
        continue;
      }
      return tl::make_unexpected(res.error());
    }

    if (*res == 0) {
      // Peer closed mid-packet - a successful read of 0 bytes, not an
      // error (see Socket::read()'s own doc comment), so don't touch
      // res.error() here.
      return bell::make_unexpected_errc(std::errc::connection_reset);
    }

    readBytes += *res;
  }

  return {};
}

void ApConnection::updateShannonNonce(uint32_t& nonce, Shannon& cipher) {
  std::array<std::byte, 4> nonceData{};
  uint32_t packedNonce = htonl(nonce);

  std::copy(reinterpret_cast<std::byte*>(&packedNonce),
            reinterpret_cast<std::byte*>(&packedNonce) + 4, nonceData.begin());

  cipher.nonce(nonceData.data(), nonceData.size());
}

bell::Result<> ApConnection::sendPacket(uint8_t cmd,
                                        const std::byte* packetData,
                                        uint16_t packetSize) {
  if (state != State::CONNECTED_SHANNON) {
    return bell::make_unexpected_errc(std::errc::operation_not_permitted);
  }

  // Packet + size + cmd
  size_t totalSize = packetSize + sizeof(uint16_t) + 1 + shannonMacSize;

  if (connectionBuffer.size() < totalSize) {
    connectionBuffer.resize(totalSize);
  }

  // Set the command byte
  connectionBuffer[0] = static_cast<std::byte>(cmd);

  // Copy the packet data
  std::copy(packetData, packetData + packetSize,
            &connectionBuffer[sizeof(uint16_t) + 1]);

  // Encode the packet size
  packetSize = htons(packetSize);
  std::copy(reinterpret_cast<std::byte*>(&packetSize),
            reinterpret_cast<std::byte*>(&packetSize) + sizeof(uint16_t),
            &connectionBuffer[1]);

  // Encrypt the packet
  sendCipher.encrypt(connectionBuffer.data(), totalSize - shannonMacSize);

  // Generate mac
  sendCipher.finish(&connectionBuffer[totalSize - shannonMacSize],
                    shannonMacSize);

  // Update the nonce
  shanSendNonce += 1;
  updateShannonNonce(shanSendNonce, sendCipher);

  // Send the packet
  auto res = apSock->write(connectionBuffer.data(), totalSize);
  if (!res) {
    return tl::make_unexpected(res.error());
  }

  return {};
}

bell::Result<std::byte*> ApConnection::receivePacket(uint8_t& cmd,
                                                     uint16_t& packetSize) {
  if (state != State::CONNECTED_SHANNON) {
    return bell::make_unexpected_errc<std::byte*>(
        std::errc::operation_not_permitted);
  }

  // Receive 3 bytes, cmd + size
  auto res = readExact(connectionBuffer.data(), 3);
  if (!res) {
    return tl::make_unexpected(res.error());
  }

  recvCipher.decrypt(connectionBuffer.data(), 3);

  // Extract the command byte
  cmd = static_cast<uint8_t>(connectionBuffer[0]);

  std::copy(&connectionBuffer[1], &connectionBuffer[3],
            reinterpret_cast<std::byte*>(&packetSize));
  packetSize = ntohs(packetSize);

  if (packetSize + shannonMacSize > connectionBuffer.size()) {
    connectionBuffer.resize(packetSize + shannonMacSize);
  }

  res = readExact(connectionBuffer.data(), packetSize + shannonMacSize);
  if (!res) {
    return tl::make_unexpected(res.error());
  }

  // Decrypt the packet
  recvCipher.decrypt(connectionBuffer.data(), packetSize);

  // Generate mac
  std::array<std::byte, shannonMacSize> mac{};
  recvCipher.finish(mac.data(), mac.size());

  // Compare the received mac with the calculated mac
  if (std::memcmp(mac.data(), &connectionBuffer[packetSize], shannonMacSize) !=
      0) {
    BELL_LOG(error, LOG_TAG, "MAC mismatch in the received packet");
    for (auto& byte : mac) {
      std::cout << std::hex << static_cast<int>(byte) << ", ";
    }
    for (size_t i = 0; i < shannonMacSize; i++) {
      std::cout << std::hex
                << static_cast<int>(connectionBuffer[packetSize + i]) << ", ";
    }
    std::cout << std::dec << std::endl;
    throw std::runtime_error("MAC mismatch in the received packet");
  }

  // Update the nonce
  shanRecvNonce += 1;
  updateShannonNonce(shanRecvNonce, recvCipher);

  return connectionBuffer.data();
}
