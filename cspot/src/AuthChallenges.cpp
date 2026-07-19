#include "AuthChallenges.h"

#include <algorithm>  // for copy
#include <stdexcept>  // for runtime_error

#include "NanoPBHelper.h"  // for pbPutString, pbEncode, pbDecode
#include "pb.h"            // for pb_byte_t
#include "pb_decode.h"     // for pb_release

using namespace cspot;

AuthChallenges::AuthChallenges() {
  this->crypto = std::make_unique<Crypto>();
  this->clientHello = {};
  this->apResponse = {};
  this->authRequest = {};
  this->clientResPlaintext = {};
}

AuthChallenges::~AuthChallenges() {
  pb_release(ClientHello_fields, &clientHello);
  pb_release(APResponseMessage_fields, &apResponse);
  pb_release(ClientResponsePlaintext_fields, &clientResPlaintext);
  pb_release(ClientResponseEncrypted_fields, &authRequest);
}

std::vector<uint8_t> AuthChallenges::prepareAuthPacket(
    std::vector<uint8_t>& authData, int authType, const std::string& deviceId,
    const std::string& username) {
  pbPutString(username, authRequest.login_credentials.username);

  // auth_data is a fixed array (max_size:512) - authData comes from an
  // unauthenticated HTTP POST body on the ZeroConf pairing path
  // (LoginBlob.cpp), so an oversized value must be rejected here or it
  // overruns auth_data.bytes. See F21.
  if (authData.size() > sizeof(authRequest.login_credentials.auth_data.bytes)) {
    throw std::runtime_error("authData too large for auth_data.bytes");
  }

  std::copy(authData.begin(), authData.end(),
            authRequest.login_credentials.auth_data.bytes);
  authRequest.login_credentials.auth_data.size = authData.size();

  authRequest.login_credentials.typ = (AuthenticationType)authType;
  authRequest.system_info.cpu_family = CpuFamily_CPU_UNKNOWN;
  authRequest.system_info.os = Os_OS_UNKNOWN;

  auto infoStr = std::string("cspot-player");
  pbPutString(infoStr, authRequest.system_info.system_information_string);
  pbPutString(deviceId, authRequest.system_info.device_id);

  auto versionStr = std::string("cspot-1.1");
  pbPutString(versionStr, authRequest.version_string);
  authRequest.has_version_string = true;

  return pbEncode(ClientResponseEncrypted_fields, &authRequest);
}

std::vector<uint8_t> AuthChallenges::solveApHello(
    std::vector<uint8_t>& helloPacket, std::vector<uint8_t>& data) {
  // `data` is a raw network packet - guard against a short/malformed
  // AP-Hello response before slicing off its first 4 bytes below. See F23.
  if (data.size() < 4) {
    throw std::runtime_error("AP-Hello response too short");
  }

  auto skipSize = std::vector<uint8_t>(data.begin() + 4, data.end());

  pb_release(APResponseMessage_fields, &apResponse);
  pbDecode(apResponse, APResponseMessage_fields, skipSize);

  auto diffieKey = std::vector<uint8_t>(
      apResponse.challenge.login_crypto_challenge.diffie_hellman.gs,
      apResponse.challenge.login_crypto_challenge.diffie_hellman.gs + 96);

  auto sharedKey = this->crypto->dhCalculateShared(diffieKey);

  // The hmac challenge is computed over client hello + server response.
  data.insert(data.begin(), helloPacket.begin(), helloPacket.end());

  // Solve the hmac challenge
  auto resultData = std::vector<uint8_t>(0);
  for (int x = 1; x < 6; x++) {
    auto challengeVector = std::vector<uint8_t>(1);
    challengeVector[0] = x;

    challengeVector.insert(challengeVector.begin(), data.begin(), data.end());
    auto digest = crypto->sha1HMAC(sharedKey, challengeVector);
    resultData.insert(resultData.end(), digest.begin(), digest.end());
  }

  auto lastVec =
      std::vector<uint8_t>(resultData.begin(), resultData.begin() + 0x14);

  auto digest = crypto->sha1HMAC(lastVec, data);
  clientResPlaintext.login_crypto_response.has_diffie_hellman = true;

  std::copy(digest.begin(), digest.end(),
            clientResPlaintext.login_crypto_response.diffie_hellman.hmac);

  this->shanSendKey = std::vector<uint8_t>(resultData.begin() + 0x14,
                                           resultData.begin() + 0x34);
  this->shanRecvKey = std::vector<uint8_t>(resultData.begin() + 0x34,
                                           resultData.begin() + 0x54);

  return pbEncode(ClientResponsePlaintext_fields, &clientResPlaintext);
}

std::vector<uint8_t> AuthChallenges::prepareClientHello() {
  this->crypto->dhInit();

  std::copy(this->crypto->publicKey.begin(), this->crypto->publicKey.end(),
            clientHello.login_crypto_hello.diffie_hellman.gc);

  clientHello.login_crypto_hello.diffie_hellman.server_keys_known = 1;
  clientHello.build_info.product = Product_PRODUCT_CLIENT;
  clientHello.build_info.platform = Platform2_PLATFORM_LINUX_X86;
  clientHello.build_info.version = SPOTIFY_VERSION;
  clientHello.feature_set.autoupdate2 = true;
  clientHello.cryptosuites_supported[0] = Cryptosuite_CRYPTO_SUITE_SHANNON;
  clientHello.padding[0] = 0x1E;

  clientHello.has_feature_set = true;
  clientHello.login_crypto_hello.has_diffie_hellman = true;
  clientHello.has_padding = true;

  auto nonce = crypto->generateVectorWithRandomData(16);
  std::copy(nonce.begin(), nonce.end(), clientHello.client_nonce);

  return pbEncode(ClientHello_fields, &clientHello);
}
