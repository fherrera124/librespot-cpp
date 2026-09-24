#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "proto/NanoPBHelper.h"

// Protobuf includes
#include "playplay.pb.h"

namespace cspot_proto {
struct PlayPlayLicenseRequest {
  int32_t version = 0;
  std::vector<std::byte> token;
  std::vector<std::byte> cacheId;
  Interactivity interactivity = Interactivity_UNKNOWN_INTERACTIVITY;
  ContentType contentType = ContentType_UNKNOWN_CONTENT_TYPE;
  int64_t timestamp = 0;

  static auto bindFields(PlayPlayLicenseRequest* self, bool isDecode) {
    _PlayPlayLicenseRequest rawProto = PlayPlayLicenseRequest_init_zero;

    nanopb_helper::bindField(rawProto.version, self->version, isDecode);
    nanopb_helper::bindField(rawProto.token, self->token, isDecode);
    nanopb_helper::bindField(rawProto.cache_id, self->cacheId, isDecode);
    nanopb_helper::bindField(rawProto.interactivity, self->interactivity,
                             isDecode);
    nanopb_helper::bindField(rawProto.content_type, self->contentType,
                             isDecode);
    nanopb_helper::bindField(rawProto.timestamp, self->timestamp, isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::PlayPlayLicenseRequest,
              PlayPlayLicenseRequest_fields)

namespace cspot_proto {
struct PlayPlayLicenseResponse {
  std::vector<std::byte> obfuscatedKey;
  std::vector<std::byte> b4Seq;

  static auto bindFields(PlayPlayLicenseResponse* self, bool isDecode) {
    _PlayPlayLicenseResponse rawProto = PlayPlayLicenseResponse_init_zero;

    nanopb_helper::bindField(rawProto.obfuscated_key, self->obfuscatedKey,
                             isDecode);
    nanopb_helper::bindField(rawProto.b4_seq, self->b4Seq, isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::PlayPlayLicenseResponse,
              PlayPlayLicenseResponse_fields)
