#include "Session.h"

#include <limits.h>     // for CHAR_BIT
#include <cstdint>      // for uint8_t
#include <exception>    // for exception
#include <functional>   // for __base
#include <memory>       // for shared_ptr, unique_ptr, make_unique
#include <random>       // for default_random_engine, independent_bi...
#include <type_traits>  // for remove_extent_t
#include <utility>      // for move

#include "ApResolve.h"          // for ApResolve, cspot
#include "AuthChallenges.h"     // for AuthChallenges
#include "BellLogger.h"         // for AbstractLogger
#include "Logger.h"             // for CSPOT_LOG
#include "LoginBlob.h"          // for LoginBlob
#include "Packet.h"             // for Packet
#include "PlainConnection.h"    // for PlainConnection, timeoutCallback
#include "ShannonConnection.h"  // for ShannonConnection

#include "NanoPBHelper.h"  // for pbPutString, pbEncode, pbDecode
#include "pb_decode.h"
#include "protobuf/authentication.pb.h"

using random_bytes_engine =
    std::independent_bits_engine<std::default_random_engine, CHAR_BIT, uint8_t>;

using namespace cspot;

Session::Session() {
  this->challenges = std::make_unique<cspot::AuthChallenges>();
}

Session::~Session() {}

void Session::connect(std::unique_ptr<cspot::PlainConnection> connection) {
  // Handshake happens entirely on local variables, not the conn/shanConn
  // members - the AP handshake + auth is several blocking network
  // round-trips (seconds, longer on a bad connection), and holding
  // shanConnMutex for all of that would make any other task's
  // getShanConn() hang for just as long, instead of failing fast the way
  // it does today. The members only get swapped in, under the lock, once
  // the new connection is actually ready. See F93.
  auto localConn =
      std::shared_ptr<cspot::PlainConnection>(std::move(connection));
  localConn->timeoutHandler = [this]() {
    return this->triggerTimeout();
  };
  auto helloPacket = localConn->sendPrefixPacket(
      {0x00, 0x04}, this->challenges->prepareClientHello());
  auto apResponse = localConn->recvPacket();
  CSPOT_LOG(info, "Received APHello response");

  auto solvedHello = this->challenges->solveApHello(helloPacket, apResponse);

  localConn->sendPrefixPacket({}, solvedHello);
  CSPOT_LOG(debug, "Received shannon keys");

  // Generates the public and priv key
  auto localShanConn = std::make_shared<ShannonConnection>();

  // Init shanno-encrypted connection
  localShanConn->wrapConnection(localConn, challenges->shanSendKey,
                                challenges->shanRecvKey);

  std::scoped_lock lock(shanConnMutex);
  this->conn = localConn;
  this->shanConn = localShanConn;
}

void Session::connectWithRandomAp() {
  auto apResolver = std::make_unique<ApResolve>("");
  auto apAddrs = apResolver->fetchApAddresses();

  // Try every candidate before giving up, instead of getting stuck on a
  // single bad AP - same pattern as PlainConnection::connect()'s own F17
  // fix one level down (per-hostname address rotation), and
  // DealerClient::connectOnce()'s dealer address rotation.
  for (const auto& apAddr : apAddrs) {
    auto conn = std::make_unique<cspot::PlainConnection>();
    conn->timeoutHandler = [this]() {
      return this->triggerTimeout();
    };

    CSPOT_LOG(debug, "Connecting with AP <%s>", apAddr.c_str());
    try {
      conn->connect(apAddr);
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "AP connect to %s failed: %s, trying next",
               apAddr.c_str(), e.what());
      continue;
    }

    this->connect(std::move(conn));
    return;
  }

  throw std::runtime_error("Can't connect to any Spotify access point");
}

std::vector<uint8_t> Session::authenticate(std::shared_ptr<LoginBlob> blob) {
  // save auth blob for reconnection purposes
  authBlob = blob;
  // prepare authentication request proto
  auto data = challenges->prepareAuthPacket(blob->authData, blob->authType,
                                            deviceId, blob->username);

  // Send login request
  this->shanConn->sendPacket(LOGIN_REQUEST_COMMAND, data);

  auto packet = this->shanConn->recvPacket();
  switch (packet.command) {
    case AUTH_SUCCESSFUL_COMMAND: {
      APWelcome welcome;
      CSPOT_LOG(debug, "Authorization successful");
      pbDecode(welcome, APWelcome_fields, packet.data);
      return std::vector<uint8_t>(welcome.reusable_auth_credentials.bytes,
                                  welcome.reusable_auth_credentials.bytes +
                                      welcome.reusable_auth_credentials.size);
      break;
    }
    case AUTH_DECLINED_COMMAND: {
      CSPOT_LOG(error, "Authorization declined");
      break;
    }
    default:
      CSPOT_LOG(error, "Unknown auth fail code %d", packet.command);
  }

  return std::vector<uint8_t>(0);
}

void Session::close() {
  this->conn->close();
}

std::shared_ptr<cspot::ShannonConnection> Session::getShanConn() {
  std::scoped_lock lock(shanConnMutex);
  return shanConn;
}
