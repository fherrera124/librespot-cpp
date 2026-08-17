#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "playlist4.pb.h"
#include "proto/NanoPBHelper.h"

namespace cspot_proto {
struct PlaylistItem {
  std::string uri;

  static auto bindFields(PlaylistItem* self, bool isDecode) {
    _Item rawProto = Item_init_zero;
    nanopb_helper::bindField(rawProto.uri, self->uri, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::PlaylistItem, Item_fields)

namespace cspot_proto {
struct PlaylistItems {
  bool truncated = false;
  std::vector<cspot_proto::PlaylistItem> items;
  std::string continuationToken;

  static auto bindFields(PlaylistItems* self, bool isDecode) {
    _ListItems rawProto = ListItems_init_zero;
    nanopb_helper::bindField(rawProto.truncated, self->truncated, isDecode);
    nanopb_helper::bindField(rawProto.items, self->items, isDecode);
    nanopb_helper::bindField(rawProto.continuation_token,
                             self->continuationToken, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::PlaylistItems, ListItems_fields)

namespace cspot_proto {
struct SelectedListContent {
  int32_t length = 0;
  nanopb_helper::Optional<cspot_proto::PlaylistItems> contents;

  static auto bindFields(SelectedListContent* self, bool isDecode) {
    _SelectedListContent rawProto = SelectedListContent_init_zero;
    nanopb_helper::bindField(rawProto.length, self->length, isDecode);
    nanopb_helper::bindField(rawProto.contents, self->contents, isDecode);
    return rawProto;
  }
};
}  // namespace cspot_proto
NANOPB_STRUCT(cspot_proto::SelectedListContent, SelectedListContent_fields)
