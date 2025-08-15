#pragma once

#include "NanoPBHelper.h"

// Protobuf includes
#include "authentication.pb.h"

namespace cspot_proto {
struct SystemInfo {
  CpuFamily cpuFamily;
  Os os;
  std::string systemInformationString;
  std::string deviceId;

  static auto bindFields(SystemInfo* self, bool isDecode) {
    _SystemInfo rawProto = SystemInfo_init_zero;
    nanopb_helper::bindVarintField(rawProto.cpu_family, self->cpuFamily,
                                   isDecode);
    nanopb_helper::bindVarintField(rawProto.os, self->os, isDecode);
    nanopb_helper::bindField(rawProto.system_information_string,
                             self->systemInformationString, isDecode);
    nanopb_helper::bindField(rawProto.device_id, self->deviceId, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::SystemInfo, SystemInfo_fields);

namespace cspot_proto {
struct LoginCredentials {
  AuthenticationType type;
  std::vector<uint8_t> authData;
  std::string username;

  static auto bindFields(LoginCredentials* self, bool isDecode) {
    _LoginCredentials rawProto = LoginCredentials_init_zero;
    nanopb_helper::bindVarintField(rawProto.typ, self->type, isDecode);
    nanopb_helper::bindField(rawProto.auth_data, self->authData, isDecode);
    nanopb_helper::bindField(rawProto.username, self->username, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::LoginCredentials, LoginCredentials_fields);

namespace cspot_proto {
struct ClientResponseEncrypted {
  cspot_proto::LoginCredentials loginCredentials;
  cspot_proto::SystemInfo systemInfo;
  std::string versionString;

  static auto bindFields(ClientResponseEncrypted* self, bool isDecode) {
    _ClientResponseEncrypted rawProto = ClientResponseEncrypted_init_zero;
    nanopb_helper::bindField(rawProto.login_credentials, self->loginCredentials,
                             isDecode);
    nanopb_helper::bindField(rawProto.system_info, self->systemInfo, isDecode);
    nanopb_helper::bindField(rawProto.version_string, self->versionString,
                             isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::ClientResponseEncrypted,
              ClientResponseEncrypted_fields);
