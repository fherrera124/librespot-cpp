#pragma once

#include "proto/NanoPBHelper.h"

// Protobuf includes
#include "login5.pb.h"

namespace cspot_proto {
struct StoredCredential {
  std::string username;
  std::vector<std::byte> data;

  static auto bindFields(StoredCredential* self, bool isDecode) {
    _StoredCredential rawProto = StoredCredential_init_zero;

    nanopb_helper::bindField(rawProto.username, self->username, isDecode);
    nanopb_helper::bindField(rawProto.data, self->data, isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::StoredCredential, StoredCredential_fields)

namespace cspot_proto {
struct ClientInfo {
  std::string clientId;
  std::string deviceId;

  static auto bindFields(ClientInfo* self, bool isDecode) {
    _ClientInfo rawProto = ClientInfo_init_zero;

    nanopb_helper::bindField(rawProto.client_id, self->clientId, isDecode);
    nanopb_helper::bindField(rawProto.device_id, self->deviceId, isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::ClientInfo, ClientInfo_fields)

namespace cspot_proto {
struct LoginRequest {
  cspot_proto::ClientInfo clientInfo;
  cspot_proto::StoredCredential storedCredential;

  static auto bindFields(LoginRequest* self, bool isDecode) {
    _LoginRequest rawProto = LoginRequest_init_zero;

    nanopb_helper::bindField(rawProto.client_info, self->clientInfo, isDecode);
    nanopb_helper::bindField(rawProto.stored_credential, self->storedCredential,
                             isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::LoginRequest, LoginRequest_fields)

namespace cspot_proto {
struct LoginOk {
  std::string accessToken;
  int32_t accessTokenExpiresIn;

  static auto bindFields(LoginOk* self, bool isDecode) {
    _LoginOk rawProto = LoginOk_init_zero;

    nanopb_helper::bindField(rawProto.access_token, self->accessToken,
                             isDecode);
    nanopb_helper::bindField(rawProto.access_token_expires_in,
                             self->accessTokenExpiresIn, isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::LoginOk, LoginOk_fields);

namespace cspot_proto {
struct LoginResponse {
  nanopb_helper::Optional<cspot_proto::LoginOk> loginOk;
  nanopb_helper::Optional<LoginError> loginError;

  static auto bindFields(LoginResponse* self, bool isDecode) {
    _LoginResponse rawProto = LoginResponse_init_zero;

    nanopb_helper::bindField(rawProto.ok, self->loginOk, isDecode);
    // TODO: FIXUP!
    // nanopb_helper::bindField(rawProto.error, self->loginError, isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::LoginResponse, LoginResponse_fields);
