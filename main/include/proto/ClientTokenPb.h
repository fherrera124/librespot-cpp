#pragma once

#include "proto/NanoPBHelper.h"

// Protobuf includes
#include "connectivity.pb.h"
#include "clienttoken.pb.h"

namespace cspot_proto {
struct NativeDesktopLinuxData {
  std::string systemName;     //  uname -s
  std::string systemRelease;  //  -r
  std::string systemVersion;  //  -v
  std::string hardware;       //  -i

  static auto bindFields(NativeDesktopLinuxData* self, bool isDecode) {
    _NativeDesktopLinuxData rawProto = NativeDesktopLinuxData_init_zero;

    nanopb_helper::bindField(rawProto.system_name, self->systemName, isDecode);
    nanopb_helper::bindField(rawProto.system_release, self->systemRelease,
                             isDecode);
    nanopb_helper::bindField(rawProto.system_version, self->systemVersion,
                             isDecode);
    nanopb_helper::bindField(rawProto.hardware, self->hardware, isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::NativeDesktopLinuxData,
              NativeDesktopLinuxData_fields)

namespace cspot_proto {
struct PlatformSpecificData {
  cspot_proto::NativeDesktopLinuxData desktopLinux;
  static auto bindFields(PlatformSpecificData* self, bool isDecode) {
    _PlatformSpecificData rawProto = PlatformSpecificData_init_zero;

    nanopb_helper::bindField(rawProto.desktop_linux, self->desktopLinux,
                             isDecode);

    return rawProto;
  }
};
};  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::PlatformSpecificData, PlatformSpecificData_fields)

namespace cspot_proto {
struct ConnectivitySdkData {
  cspot_proto::PlatformSpecificData platformSpecificData;
  std::string deviceId;

  static auto bindFields(ConnectivitySdkData* self, bool isDecode) {
    _ConnectivitySdkData rawProto = ConnectivitySdkData_init_zero;

    nanopb_helper::bindField(rawProto.platform_specific_data,
                             self->platformSpecificData, isDecode);
    nanopb_helper::bindField(rawProto.device_id, self->deviceId, isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::ConnectivitySdkData, ConnectivitySdkData_fields)

namespace cspot_proto {
struct ClientDataRequest {
  std::string clientVersion;
  std::string clientId;
  cspot_proto::ConnectivitySdkData connectivitySdkData;

  static auto bindFields(ClientDataRequest* self, bool isDecode) {
    _ClientDataRequest rawProto = ClientDataRequest_init_zero;

    nanopb_helper::bindField(rawProto.client_version, self->clientVersion,
                             isDecode);
    nanopb_helper::bindField(rawProto.client_id, self->clientId, isDecode);
    nanopb_helper::bindField(rawProto.connectivity_sdk_data,
                             self->connectivitySdkData, isDecode);

    return rawProto;
  }
};
};  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::ClientDataRequest, ClientDataRequest_fields);

namespace cspot_proto {
struct ClientTokenRequest {
  ClientTokenRequestType requestType =
      ClientTokenRequestType_REQUEST_CLIENT_DATA_REQUEST;
  cspot_proto::ClientDataRequest clientData;

  static auto bindFields(ClientTokenRequest* self, bool isDecode) {
    _ClientTokenRequest rawProto = ClientTokenRequest_init_zero;

    nanopb_helper::bindField(rawProto.request_type, self->requestType,
                                   isDecode);
    nanopb_helper::bindField(rawProto.client_data, self->clientData, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::ClientTokenRequest, ClientTokenRequest_fields);

namespace cspot_proto {
struct GrantedTokenResponse {
  std::string token;
  int32_t expiresAfterSeconds;
  int32_t refreshAfterSeconds;
  static auto bindFields(GrantedTokenResponse* self, bool isDecode) {
    _GrantedTokenResponse rawProto = GrantedTokenResponse_init_zero;

    nanopb_helper::bindField(rawProto.token, self->token, isDecode);
    nanopb_helper::bindField(rawProto.expires_after_seconds,
                             self->expiresAfterSeconds, isDecode);
    nanopb_helper::bindField(rawProto.refresh_after_seconds,
                             self->refreshAfterSeconds, isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::GrantedTokenResponse, GrantedTokenResponse_fields)

namespace cspot_proto {
struct ClientTokenResponse {
  ClientTokenResponseType responseType;
  cspot_proto::GrantedTokenResponse grantedToken;

  static auto bindFields(ClientTokenResponse* self, bool isDecode) {
    _ClientTokenResponse rawProto = ClientTokenResponse_init_zero;

    nanopb_helper::bindField(rawProto.response_type, self->responseType,
                                   isDecode);
    nanopb_helper::bindField(rawProto.granted_token, self->grantedToken,
                             isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::ClientTokenResponse, ClientTokenResponse_fields)
