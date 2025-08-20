#include "crypto/Base62.h"

#include <algorithm>
#include <vector>

namespace cspot {

namespace {
// The Base62 alphabet
const char* const BASE62_ALPHABET =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
const int BASE62_BASE = 62;

// Helper function to convert a byte array to a large integer (represented as a vector of digits)
std::vector<int> bytesToBigInt(const std::byte* data, size_t size) {
  std::vector<int> bigInt(size);
  for (size_t i = 0; i < size; ++i) {
    bigInt[i] = static_cast<int>(data[i]);
  }
  return bigInt;
}

// Helper function to perform division on a large integer
std::vector<int> divMod(std::vector<int>& number, int divisor, int& remainder) {
  remainder = 0;
  std::vector<int> result;
  for (int digit : number) {
    long long current =
        ((long long)remainder * 256) + digit;  // Assuming 256 as base for bytes
    result.push_back(static_cast<int>(current / divisor));
    remainder = static_cast<int>(current % divisor);
  }

  // Remove leading zeros
  auto firstNonZero =
      std::find_if(result.begin(), result.end(), [](int d) { return d != 0; });
  if (firstNonZero == result.end()) {
    return {0};  // Handle case where result is all zeros
  }
  return {firstNonZero, result.end()};
}

}  // namespace

std::string base62Encode(const std::byte* data, size_t size) {
  if (size == 0) {
    return "";
  }

  std::vector<int> bytes = bytesToBigInt(data, size);
  std::string encodedStr;

  // Handle leading zeros in the input byte array
  size_t leadingZeros = 0;
  while (leadingZeros < size && data[leadingZeros] == std::byte{0}) {
    encodedStr += BASE62_ALPHABET[0];  // Append '0' for each leading zero byte
    leadingZeros++;
  }

  if (bytes.empty() || (bytes.size() == 1 && bytes[0] == 0)) {
    return "0";  // Special case for input {0}
  }

  std::vector<int> currentNumber = bytes;
  while (currentNumber.size() != 1 || currentNumber[0] != 0) {
    int remainder;
    currentNumber = divMod(currentNumber, BASE62_BASE, remainder);
    encodedStr += BASE62_ALPHABET[remainder];
  }

  std::reverse(encodedStr.begin(), encodedStr.end());
  return encodedStr;
}

bool base62Decode(std::string_view encodedStr, std::byte* outData,
                  size_t& outSize) {
  if (encodedStr.empty()) {
    outSize = 0;
    return true;
  }

  // Initialize the decoded number as a vector of bytes (base 256)
  std::vector<std::byte> decodedBytes;
  decodedBytes.push_back(std::byte{0});  // Start with 0

  // Handle leading '0's in the encoded string
  size_t leadingZeros = 0;
  while (leadingZeros < encodedStr.length() &&
         encodedStr[leadingZeros] == BASE62_ALPHABET[0]) {
    if (decodedBytes.size() <
        outSize) {  // Ensure we don't write beyond outData capacity
      decodedBytes.insert(decodedBytes.begin(), std::byte{0});
    }
    leadingZeros++;
  }

  for (char c : encodedStr) {
    size_t value = std::string_view(BASE62_ALPHABET).find(c);
    if (value == std::string_view::npos) {
      // Invalid character in Base62 string
      return false;
    }

    // Multiply by BASE62_BASE and add the current digit
    int carry = static_cast<int>(value);
    for (size_t i = 0; i < decodedBytes.size(); ++i) {
      long long temp = ((long long)decodedBytes[i] * BASE62_BASE) + carry;
      decodedBytes[i] = static_cast<std::byte>(temp % 256);
      carry = static_cast<int>(temp / 256);
    }
    while (carry > 0) {
      decodedBytes.push_back(static_cast<std::byte>(carry % 256));
      carry /= 256;
    }
  }

  // Reverse the decoded bytes as they were accumulated in reverse order
  std::reverse(decodedBytes.begin(), decodedBytes.end());

  // Copy to outData, handling potential size mismatch
  if (decodedBytes.size() > outSize) {
    // Output buffer is too small
    outSize = decodedBytes.size();  // Inform the caller about the required size
    return false;
  }

  std::copy(decodedBytes.begin(), decodedBytes.end(), outData);
  outSize = decodedBytes.size();

  return true;
}

}  // namespace cspot
