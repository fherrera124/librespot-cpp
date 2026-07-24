#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "extended_metadata.pb.h"
#include "proto/MetadataPb.h"
#include "proto/NanoPBHelper.h"

namespace cspot_proto {
struct ExtensionQuery {
  ExtensionKind extensionKind;

  static auto bindFields(ExtensionQuery* self, bool isDecode) {
    _ExtensionQuery rawProto = ExtensionQuery_init_zero;
    nanopb_helper::bindField(rawProto.extension_kind, self->extensionKind,
                             isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::ExtensionQuery, ExtensionQuery_fields)

namespace cspot_proto {
struct EntityRequest {
  std::string entityUri;
  std::vector<cspot_proto::ExtensionQuery> query;

  static auto bindFields(EntityRequest* self, bool isDecode) {
    _EntityRequest rawProto = EntityRequest_init_zero;
    nanopb_helper::bindField(rawProto.entity_uri, self->entityUri, isDecode);
    nanopb_helper::bindField(rawProto.query, self->query, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::EntityRequest, EntityRequest_fields)

namespace cspot_proto {
struct BatchedEntityRequest {
  std::vector<cspot_proto::EntityRequest> entityRequest;

  static auto bindFields(BatchedEntityRequest* self, bool isDecode) {
    _BatchedEntityRequest rawProto = BatchedEntityRequest_init_zero;
    nanopb_helper::bindField(rawProto.entity_request, self->entityRequest,
                             isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::BatchedEntityRequest, BatchedEntityRequest_fields)

namespace cspot_proto {
struct AnyMessage {
  std::vector<std::byte> value;

  static auto bindFields(AnyMessage* self, bool isDecode) {
    _AnyMessage rawProto = AnyMessage_init_zero;
    nanopb_helper::bindField(rawProto.value, self->value, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::AnyMessage, AnyMessage_fields)

namespace cspot_proto {
struct EntityExtensionDataHeader {
  // uint32_t, not int32_t: for FT_CALLBACK messages nanopb generates no
  // type-aware (de)serialization at all - bindField() alone decides
  // zigzag vs plain-varint decode, purely from the C++ field's
  // signedness (see NanoPBHelper.h), regardless of what the .proto
  // declares (int32 vs sint32 - that distinction is a no-op here). The
  // real server sends this as a plain (non-zigzag) varint - confirmed by
  // hand-decoding a real response where a real 200 came back as 100
  // (200 >> 1, the exact zigzag-of-a-plain-varint signature) with the
  // signed version. Status codes are never negative anyway.
  uint32_t statusCode = 0;

  static auto bindFields(EntityExtensionDataHeader* self, bool isDecode) {
    _EntityExtensionDataHeader rawProto = EntityExtensionDataHeader_init_zero;
    nanopb_helper::bindField(rawProto.status_code, self->statusCode,
                             isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::EntityExtensionDataHeader,
              EntityExtensionDataHeader_fields)

namespace cspot_proto {
struct EntityExtensionData {
  nanopb_helper::Optional<cspot_proto::EntityExtensionDataHeader> header;
  std::string entityUri;
  nanopb_helper::Optional<cspot_proto::AnyMessage> extensionData;

  static auto bindFields(EntityExtensionData* self, bool isDecode) {
    _EntityExtensionData rawProto = EntityExtensionData_init_zero;
    nanopb_helper::bindField(rawProto.header, self->header, isDecode);
    nanopb_helper::bindField(rawProto.entity_uri, self->entityUri, isDecode);
    nanopb_helper::bindField(rawProto.extension_data, self->extensionData,
                             isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::EntityExtensionData, EntityExtensionData_fields)

namespace cspot_proto {
struct EntityExtensionDataArray {
  ExtensionKind extensionKind;
  std::vector<cspot_proto::EntityExtensionData> extensionData;

  static auto bindFields(EntityExtensionDataArray* self, bool isDecode) {
    _EntityExtensionDataArray rawProto = EntityExtensionDataArray_init_zero;
    nanopb_helper::bindField(rawProto.extension_kind, self->extensionKind,
                             isDecode);
    nanopb_helper::bindField(rawProto.extension_data, self->extensionData,
                             isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::EntityExtensionDataArray,
              EntityExtensionDataArray_fields)

namespace cspot_proto {
struct BatchedExtensionResponse {
  std::vector<cspot_proto::EntityExtensionDataArray> extendedMetadata;

  static auto bindFields(BatchedExtensionResponse* self, bool isDecode) {
    _BatchedExtensionResponse rawProto = BatchedExtensionResponse_init_zero;
    nanopb_helper::bindField(rawProto.extended_metadata,
                             self->extendedMetadata, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::BatchedExtensionResponse,
              BatchedExtensionResponse_fields)

namespace cspot_proto {
struct ExtendedAudioFile {
  nanopb_helper::Optional<cspot_proto::AudioFile> file;

  static auto bindFields(ExtendedAudioFile* self, bool isDecode) {
    _ExtendedAudioFile rawProto = ExtendedAudioFile_init_zero;
    nanopb_helper::bindField(rawProto.file, self->file, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::ExtendedAudioFile, ExtendedAudioFile_fields)

namespace cspot_proto {
struct AudioFilesExtensionResponse {
  std::vector<cspot_proto::ExtendedAudioFile> files;

  static auto bindFields(AudioFilesExtensionResponse* self, bool isDecode) {
    _AudioFilesExtensionResponse rawProto =
        AudioFilesExtensionResponse_init_zero;
    nanopb_helper::bindField(rawProto.files, self->files, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::AudioFilesExtensionResponse,
              AudioFilesExtensionResponse_fields)
