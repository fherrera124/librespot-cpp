#pragma once

#include "proto/NanoPBHelper.h"

// Protobuf includes
#include "keyexchange.pb.h"

namespace cspot_proto {
struct LoginCryptoDiffieHellmanHello {
  std::array<std::byte, 96> gc;
  uint32_t serverKeysKnown;

  static auto bindFields(LoginCryptoDiffieHellmanHello* self, bool isDecode) {
    _LoginCryptoDiffieHellmanHello rawProto =
        LoginCryptoDiffieHellmanHello_init_zero;
    nanopb_helper::bindField(rawProto.gc, self->gc, isDecode);
    nanopb_helper::bindField(rawProto.server_keys_known,
                                   self->serverKeysKnown, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::LoginCryptoDiffieHellmanHello,
              LoginCryptoDiffieHellmanHello_fields);

namespace cspot_proto {
struct LoginCryptoHelloUnion {
  nanopb_helper::Optional<LoginCryptoDiffieHellmanHello> diffieHellman;

  static auto bindFields(LoginCryptoHelloUnion* self, bool isDecode) {
    _LoginCryptoHelloUnion rawProto = LoginCryptoHelloUnion_init_zero;
    nanopb_helper::bindField(rawProto.diffie_hellman, self->diffieHellman,
                             isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::LoginCryptoHelloUnion, LoginCryptoHelloUnion_fields);

namespace cspot_proto {
struct LoginCryptoDiffieHellmanChallenge {
  std::array<std::byte, 96> gs;

  static auto bindFields(LoginCryptoDiffieHellmanChallenge* self,
                         bool isDecode) {
    _LoginCryptoDiffieHellmanChallenge rawProto =
        LoginCryptoDiffieHellmanChallenge_init_zero;
    nanopb_helper::bindField(rawProto.gs, self->gs, isDecode);
    return rawProto;
  }
};
};  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::LoginCryptoDiffieHellmanChallenge,
              LoginCryptoDiffieHellmanChallenge_fields);

namespace cspot_proto {
struct LoginCryptoChallengeUnion {
  nanopb_helper::Optional<LoginCryptoDiffieHellmanChallenge> diffieHellman;
  static auto bindFields(LoginCryptoChallengeUnion* self, bool isDecode) {
    _LoginCryptoChallengeUnion rawProto = LoginCryptoChallengeUnion_init_zero;
    nanopb_helper::bindField(rawProto.diffie_hellman, self->diffieHellman,
                             isDecode);
    return rawProto;
  };
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::LoginCryptoChallengeUnion,
              LoginCryptoChallengeUnion_fields);

namespace cspot_proto {
struct APChallenge {
  cspot_proto::LoginCryptoChallengeUnion loginCryptoChallenge;

  static auto bindFields(APChallenge* self, bool isDecode) {
    _APChallenge rawProto = APChallenge_init_zero;
    nanopb_helper::bindField(rawProto.login_crypto_challenge,
                             self->loginCryptoChallenge, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::APChallenge, APChallenge_fields);

namespace cspot_proto {
struct APResponseMessage {
  nanopb_helper::Optional<APChallenge> challenge;

  static auto bindFields(APResponseMessage* self, bool isDecode) {
    _APResponseMessage rawProto = APResponseMessage_init_zero;
    nanopb_helper::bindField(rawProto.challenge, self->challenge, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::APResponseMessage, APResponseMessage_fields);

namespace cspot_proto {
struct LoginCryptoDiffieHellmanResponse {
  std::array<std::byte, 20> hmac;

  static auto bindFields(LoginCryptoDiffieHellmanResponse* self,
                         bool isDecode) {
    _LoginCryptoDiffieHellmanResponse rawProto =
        LoginCryptoDiffieHellmanResponse_init_zero;
    nanopb_helper::bindField(rawProto.hmac, self->hmac, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::LoginCryptoDiffieHellmanResponse,
              LoginCryptoDiffieHellmanResponse_fields);

namespace cspot_proto {
struct LoginCryptoResponseUnion {
  nanopb_helper::Optional<LoginCryptoDiffieHellmanResponse> diffieHellman;

  static auto bindFields(LoginCryptoResponseUnion* self, bool isDecode) {
    _LoginCryptoResponseUnion rawProto = LoginCryptoResponseUnion_init_zero;
    nanopb_helper::bindField(rawProto.diffie_hellman, self->diffieHellman,
                             isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::LoginCryptoResponseUnion,
              LoginCryptoResponseUnion_fields);

namespace cspot_proto {
struct CryptoResponseUnion {
  static auto bindFields(CryptoResponseUnion* self, bool isDecode) {
    _CryptoResponseUnion rawProto = CryptoResponseUnion_init_zero;
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::CryptoResponseUnion, CryptoResponseUnion_fields);

namespace cspot_proto {
struct PoWResponseUnion {
  static auto bindFields(PoWResponseUnion* self, bool isDecode) {
    _PoWResponseUnion rawProto = PoWResponseUnion_init_zero;
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::PoWResponseUnion, PoWResponseUnion_fields);

namespace cspot_proto {
struct ClientResponsePlaintext {
  cspot_proto::LoginCryptoResponseUnion loginCryptoResponse;
  cspot_proto::PoWResponseUnion powResponse;
  cspot_proto::CryptoResponseUnion cryptoResponse;

  static auto bindFields(ClientResponsePlaintext* self, bool isDecode) {
    _ClientResponsePlaintext rawProto = ClientResponsePlaintext_init_zero;
    nanopb_helper::bindField(rawProto.login_crypto_response,
                             self->loginCryptoResponse, isDecode);
    nanopb_helper::bindField(rawProto.pow_response, self->powResponse,
                             isDecode);
    nanopb_helper::bindField(rawProto.crypto_response, self->cryptoResponse,
                             isDecode);
    return rawProto;
  }
};
};  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::ClientResponsePlaintext,
              ClientResponsePlaintext_fields);

namespace cspot_proto {
struct BuildInfo {
  Product product;
  Platform2 platform;
  uint64_t version;

  static auto bindFields(BuildInfo* self, bool isDecode) {
    _BuildInfo rawProto = BuildInfo_init_zero;
    nanopb_helper::bindField(rawProto.product, self->product, isDecode);
    nanopb_helper::bindField(rawProto.platform, self->platform, isDecode);
    nanopb_helper::bindField(rawProto.version, self->version, isDecode);
    return rawProto;
  }
};
};  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::BuildInfo, BuildInfo_fields);

namespace cspot_proto {
struct FeatureSet {
  bool autoupdate2;

  static auto bindFields(FeatureSet* self, bool isDecode) {
    _FeatureSet rawProto = FeatureSet_init_zero;
    nanopb_helper::bindField(rawProto.autoupdate2, self->autoupdate2, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::FeatureSet, FeatureSet_fields);

namespace cspot_proto {
struct ClientHello {
  cspot_proto::BuildInfo buildInfo;
  cspot_proto::LoginCryptoHelloUnion loginCryptoHello;
  std::vector<uint32_t> cryptosuitesSupported;
  std::vector<std::byte> padding;
  std::array<std::byte, 16> clientNonce;
  cspot_proto::FeatureSet featureSet;

  static auto bindFields(ClientHello* self, bool isDecode) {
    _ClientHello rawProto = ClientHello_init_zero;
    nanopb_helper::bindField(rawProto.build_info, self->buildInfo, isDecode);
    nanopb_helper::bindField(rawProto.login_crypto_hello,
                             self->loginCryptoHello, isDecode);
    nanopb_helper::bindField(rawProto.cryptosuites_supported,
                                       self->cryptosuitesSupported, isDecode);
    nanopb_helper::bindField(rawProto.padding, self->padding, isDecode);
    nanopb_helper::bindField(rawProto.feature_set, self->featureSet, isDecode);
    nanopb_helper::bindField(rawProto.client_nonce, self->clientNonce,
                             isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::ClientHello, ClientHello_fields);
