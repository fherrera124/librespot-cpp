#include "crypto/DiffieHellman.h"

#include <string>
#include "fmt/color.h"

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

using namespace cspot;

namespace {
// The prime and generator used by Spotify, group 1, 768-bit.
const std::array<uint8_t, 96> dhPrime = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc9, 0x0f, 0xda, 0xa2,
    0x21, 0x68, 0xc2, 0x34, 0xc4, 0xc6, 0x62, 0x8b, 0x80, 0xdc, 0x1c, 0xd1,
    0x29, 0x02, 0x4e, 0x08, 0x8a, 0x67, 0xcc, 0x74, 0x02, 0x0b, 0xbe, 0xa6,
    0x3b, 0x13, 0x9b, 0x22, 0x51, 0x4a, 0x08, 0x79, 0x8e, 0x34, 0x04, 0xdd,
    0xef, 0x95, 0x19, 0xb3, 0xcd, 0x3a, 0x43, 0x1b, 0x30, 0x2b, 0x0a, 0x6d,
    0xf2, 0x5f, 0x14, 0x37, 0x4f, 0xe1, 0x35, 0x6d, 0x6d, 0x51, 0xc2, 0x45,
    0xe4, 0x85, 0xb5, 0x76, 0x62, 0x5e, 0x7e, 0xc6, 0xf4, 0x4c, 0x42, 0xe9,
    0xa6, 0x3a, 0x36, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

// The generator value
const std::array<uint8_t, 1> dhGenerator = {0x02};
}  // namespace

DH::DH() {
  generatePrivateKey();

  mbedtls_mpi_init(&prime);
  mbedtls_mpi_init(&generator);
  mbedtls_mpi_init(&privateMpi);

  // Temporary mpi for public key
  mbedtls_mpi publicKeyMpi;
  mbedtls_mpi_init(&publicKeyMpi);

  // Read bin into big num mpi
  mbedtls_mpi_read_binary(&prime, dhPrime.data(), dhPrime.size());
  mbedtls_mpi_read_binary(&generator, dhGenerator.data(), dhGenerator.size());
  mbedtls_mpi_read_binary(&privateMpi, privateKey.data(), privateKey.size());

  // perform diffie hellman G^X mod P
  int calcRes = mbedtls_mpi_exp_mod(&publicKeyMpi, &generator, &privateMpi,
                                    &prime, nullptr);
  if (calcRes != 0) {
    // Free memory for public key mpi
    mbedtls_mpi_free(&publicKeyMpi);

    throw std::runtime_error("Failed to calculate DH public key");
  }

  // Write generated public key to vector
  mbedtls_mpi_write_binary(&publicKeyMpi, publicKey.data(), publicKey.size());

  // Free memory for public key mpi
  mbedtls_mpi_free(&publicKeyMpi);
}

void DH::computeSharedKey(const uint8_t* remotePublicKey, size_t keySize,
                          uint8_t* sharedKey) {
  // initialize big num for result and remote key
  mbedtls_mpi sharedKeyMpi;
  mbedtls_mpi remoteKeyMpi;
  mbedtls_mpi_init(&sharedKeyMpi);
  mbedtls_mpi_init(&remoteKeyMpi);

  // Read bin into big num mpi
  mbedtls_mpi_read_binary(&remoteKeyMpi, remotePublicKey, keySize);

  // perform diffie hellman (G^Y)^X mod P (for shared secret)
  int res = mbedtls_mpi_exp_mod(&sharedKeyMpi, &remoteKeyMpi, &privateMpi,
                                &prime, nullptr);

  if (res != 0) {
    mbedtls_mpi_free(&remoteKeyMpi);
    mbedtls_mpi_free(&sharedKeyMpi);

    throw std::runtime_error("Failed to calculate DH shared key");
  }

  mbedtls_mpi_write_binary(&sharedKeyMpi, sharedKey, dhKeySize);

  // Release memory
  mbedtls_mpi_free(&remoteKeyMpi);
  mbedtls_mpi_free(&sharedKeyMpi);
}

DH::~DH() {
  mbedtls_mpi_free(&prime);
  mbedtls_mpi_free(&generator);
  mbedtls_mpi_free(&privateMpi);
}

std::string DH::getPublicKeyBase64() {
  std::string publicKeyBase64;

  size_t outputSize = 0;
  int res = mbedtls_base64_encode(nullptr, 0, &outputSize, publicKey.data(),
                                  publicKey.size());
  if (outputSize == 0) {
    throw std::runtime_error(
        fmt::format("Failed to calculate base64 encoded public key size"));
  }

  publicKeyBase64.resize(outputSize);
  res = mbedtls_base64_encode(
      reinterpret_cast<uint8_t*>(publicKeyBase64.data()),
      publicKeyBase64.size(), &outputSize, publicKey.data(), publicKey.size());
  if (res != 0) {
    throw std::runtime_error("Failed to calculate base64 encoded public key");
  }

  // Convert public key to string
  return publicKeyBase64;
}

void DH::generatePrivateKey() {
  // Generate a random private key
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctrDrbg;

  // Personification string
  std::string pers = "cspotGen";

  // init entropy and random num generator
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctrDrbg);

  // Seed the generator
  mbedtls_ctr_drbg_seed(&ctrDrbg, mbedtls_entropy_func, &entropy,
                        reinterpret_cast<const uint8_t*>(pers.data()),
                        pers.size());

  // Generate random bytes
  mbedtls_ctr_drbg_random(&ctrDrbg, privateKey.data(), privateKey.size());

  // Release memory
  mbedtls_entropy_free(&entropy);
  mbedtls_ctr_drbg_free(&ctrDrbg);
}