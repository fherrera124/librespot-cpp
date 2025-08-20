#pragma once

#include <cstdint>
#include <vector>
#include "metadata.pb.h"
#include "proto/NanoPBHelper.h"

namespace cspot_proto {
struct Image {
  std::vector<uint8_t> fileId;  // Unique identifier for the image file
  static auto bindFields(Image* self, bool isDecode) {
    _Image rawProto = Image_init_zero;
    nanopb_helper::bindField(rawProto.file_id, self->fileId, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::Image, Image_fields)

namespace cspot_proto {
struct ImageGroup {
  std::vector<cspot_proto::Image> images;  // List of images in the group
  static auto bindFields(ImageGroup* self, bool isDecode) {
    _ImageGroup rawProto = ImageGroup_init_zero;
    nanopb_helper::bindField(rawProto.image, self->images, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::ImageGroup, ImageGroup_fields)

namespace cspot_proto {
struct Album {
  std::vector<std::byte> gid;
  std::string name;
  nanopb_helper::Optional<cspot_proto::ImageGroup> coverGroup;

  static auto bindFields(Album* self, bool isDecode) {
    _Album rawProto = Album_init_zero;
    nanopb_helper::bindField(rawProto.gid, self->gid, isDecode);
    nanopb_helper::bindField(rawProto.name, self->name, isDecode);
    nanopb_helper::bindField(rawProto.cover_group, self->coverGroup, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::Album, Album_fields)

namespace cspot_proto {
struct Artist {
  std::vector<std::byte> gid;
  std::string name;

  static auto bindFields(Artist* self, bool isDecode) {
    _Artist rawProto = Artist_init_zero;
    nanopb_helper::bindField(rawProto.gid, self->gid, isDecode);
    nanopb_helper::bindField(rawProto.name, self->name, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::Artist, Artist_fields)

namespace cspot_proto {
struct AudioFile {
  std::vector<std::byte> fileId;
  AudioFormat format;

  static auto bindFields(AudioFile* self, bool isDecode) {
    _AudioFile rawProto = AudioFile_init_zero;
    nanopb_helper::bindField(rawProto.file_id, self->fileId, isDecode);
    nanopb_helper::bindField(rawProto.format, self->format, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::AudioFile, AudioFile_fields)

namespace cspot_proto {
struct Restriction {
  std::string countriesAllowed;
  std::string countriesForbidden;

  static auto bindFields(Restriction* self, bool isDecode) {
    _Restriction rawProto = Restriction_init_zero;
    nanopb_helper::bindField(rawProto.countries_allowed, self->countriesAllowed,
                             isDecode);
    nanopb_helper::bindField(rawProto.countries_forbidden,
                             self->countriesForbidden, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::Restriction, Restriction_fields)

namespace cspot_proto {
struct Track {
  std::vector<std::byte> gid;
  std::string name;
  int32_t durationMs;
  nanopb_helper::Optional<cspot_proto::Album> album;
  std::vector<cspot_proto::Artist> artists;
  std::vector<cspot_proto::Restriction> restrictions;
  std::vector<cspot_proto::AudioFile> audioFiles;
  std::vector<cspot_proto::Track> alternativeTracks;

  // Definition moved to cpp file to avoid circular dependency issues
  static _Track bindFields(Track* self, bool isDecode);
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::Track, Track_fields)

namespace cspot_proto {
struct Episode {
  std::vector<std::byte> gid;
  std::string name;
  int32_t durationMs;
  std::vector<cspot_proto::Restriction> restrictions;
  std::vector<cspot_proto::AudioFile> audioFiles;
  nanopb_helper::Optional<ImageGroup> coverGroup;

  static auto bindFields(Episode* self, bool isDecode) {
    _Episode rawProto = Episode_init_zero;
    nanopb_helper::bindField(rawProto.gid, self->gid, isDecode);
    nanopb_helper::bindField(rawProto.name, self->name, isDecode);
    nanopb_helper::bindField(rawProto.duration, self->durationMs, isDecode);
    nanopb_helper::bindField(rawProto.restriction, self->restrictions,
                             isDecode);
    nanopb_helper::bindField(rawProto.file, self->audioFiles, isDecode);
    nanopb_helper::bindField(rawProto.covers, self->coverGroup, isDecode);

    return rawProto;
  }
};
}  // namespace cspot_proto

NANOPB_STRUCT(cspot_proto::Episode, Episode_fields)
