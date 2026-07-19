#include "TrackReference.h"

#include <algorithm>    // for reverse
#include <string_view>  // for string_view

#include "Utils.h"

using namespace cspot;

static constexpr auto base62Alphabet =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

TrackReference::TrackReference() : type(Type::TRACK) {}

void TrackReference::decodeURI() {
  if (gid.size() == 0) {
    // Episode GID is being fetched via base62 encoded URI
    auto idString = uri.substr(uri.find_last_of(":") + 1, uri.size());
    gid = {0};

    std::string_view alphabet(base62Alphabet);
    for (int x = 0; x < idString.size(); x++) {
      size_t d = alphabet.find(idString[x]);
      gid = bigNumMultiply(gid, 62);
      gid = bigNumAdd(gid, d);
    }

    if (uri.find("episode:") != std::string::npos) {
      type = Type::EPISODE;
    }
  }
}

bool TrackReference::operator==(const TrackReference& other) const {
  return other.gid == gid && other.uri == uri;
}

std::string TrackReference::encodeURI(const std::vector<uint8_t>& gid,
                                      bool isEpisode) {
  // Big-number base conversion, inverse of decodeURI() above: repeatedly
  // divide by 62, collecting remainders least-significant-digit first.
  // bigNumDivide()/bigNumMultiply() (Utils.h) only expose the quotient, not
  // the remainder this needs, so this tracks it directly instead of
  // reusing them.
  std::vector<uint8_t> num = gid;
  std::string digits;
  bool allZero;
  do {
    int remainder = 0;
    allZero = true;
    for (size_t i = 0; i < num.size(); i++) {
      int cur = remainder * 256 + num[i];
      num[i] = cur / 62;
      remainder = cur % 62;
      if (num[i] != 0) {
        allZero = false;
      }
    }
    digits.push_back(base62Alphabet[remainder]);
  } while (!allZero);

  // Spotify IDs are conventionally 22 base62 characters (a 16-byte GID
  // needs at most that many digits) - left-pad with the alphabet's zero
  // digit ('0') to match, same as real clients.
  while (digits.size() < 22) {
    digits.push_back('0');
  }
  std::reverse(digits.begin(), digits.end());

  return std::string(isEpisode ? "spotify:episode:" : "spotify:track:") +
         digits;
}
