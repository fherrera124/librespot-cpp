#include "Login5Client.h"

#include <algorithm>  // for copy
#include <chrono>     // for steady_clock (hashcash timeout/duration)
#include <cstring>    // for memcpy
#include <exception>  // for exception

#include "BellLogger.h"    // for AbstractLogger
#include "BellUtils.h"     // for BELL_SLEEP_MS
#include "CSpotContext.h"  // for Context
#include "Crypto.h"        // for Crypto (sha1)
#include "HTTPClient.h"
#include "HttpRetry.h"  // for HttpRetry, PermanentHttpFailure
#include "Logger.h"        // for CSPOT_LOG
#include "NanoPBHelper.h"  // for pbEncode, pbDecode, pbPutString...
#include "TimeProvider.h"  // for TimeProvider
#include "pb_decode.h"     // for pb_release

#include "protobuf/clienttoken.pb.h"
#include "protobuf/login5.pb.h"

using namespace cspot;

namespace {
// Public client ID used by librespot/go-librespot for login5 with a stored
// credential (librespot core/src/config.rs KEYMASTER_CLIENT_ID; go-librespot
// client_id.go ClientId - same value in both, confirmed reading the source).
const char* LOGIN5_CLIENT_ID = "65b708073fc0480ea92a077233ca87bd";

const char* CLIENT_TOKEN_URL = "https://clienttoken.spotify.com/v1/clienttoken";
const char* LOGIN5_URL = "https://login5.spotify.com/v3/login";
const char* USER_AGENT = "cspot/1.0";

constexpr int MAX_LOGIN_TRIES = 3;  // same cap librespot uses
constexpr int HASHCASH_TIMEOUT_SECONDS = 5;  // ditto (challenge expires)

uint64_t bigEndian64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    v = (v << 8) | p[i];
  }
  return v;
}

void storeBigEndian64(uint64_t v, uint8_t* p) {
  for (int i = 7; i >= 0; i--) {
    p[i] = v & 0xFF;
    v >>= 8;
  }
}
}  // namespace

Login5Client::Login5Client(std::shared_ptr<cspot::Context> ctx) : ctx(ctx) {}

bool Login5Client::isExpired() {
  if (token.empty()) {
    return true;
  }

  return ctx->timeProvider->getSyncedTimestamp() > expiresAt;
}

std::string Login5Client::getToken() {
  if (!isExpired()) {
    return token;
  }

  updateToken();

  return token;
}

void Login5Client::updateToken() {
  if (tokenPending) {
    return;
  }

  tokenPending = true;

  int retryCount = 3;
  bool success = false;

  do {
    // Both requests can throw on TLS/socket failure (same F26/F63 gap
    // AccessKeyFetcher hardens against) - never let that escape the task.
    try {
      auto currentClientToken = fetchClientToken();
      if (!currentClientToken.empty()) {
        success = login(currentClientToken);
      }
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "login5 attempt failed: %s", e.what());
    }

    if (!success) {
      BELL_SLEEP_MS(3000);
    }

    retryCount--;
  } while (retryCount >= 0 && !success);

  tokenPending = false;
}

std::string Login5Client::getClientToken() {
  try {
    return fetchClientToken();
  } catch (const std::exception& e) {
    CSPOT_LOG(error, "clienttoken fetch failed: %s", e.what());
    return "";
  }
}

std::string Login5Client::fetchClientToken() {
  if (!clientToken.empty() &&
      ctx->timeProvider->getSyncedTimestamp() < clientTokenExpiresAt) {
    return clientToken;
  }

  clienttoken_ClientTokenRequest request =
      clienttoken_ClientTokenRequest_init_zero;
  request.has_request_type = true;
  request.request_type =
      clienttoken_ClientTokenRequestType_REQUEST_CLIENT_DATA_REQUEST;
  request.which_request = clienttoken_ClientTokenRequest_client_data_tag;

  auto& clientData = request.request.client_data;
  clientData.has_client_version = true;
  pbPutCharArray("0.0.0", clientData.client_version);
  clientData.has_client_id = true;
  pbPutCharArray(LOGIN5_CLIENT_ID, clientData.client_id);
  clientData.which_data =
      clienttoken_ClientDataRequest_connectivity_sdk_data_tag;

  auto& sdkData = clientData.data.connectivity_sdk_data;
  sdkData.has_device_id = true;
  pbPutString(ctx->config.deviceId, sdkData.device_id);
  sdkData.has_platform_specific_data = true;
  sdkData.platform_specific_data.which_data =
      clienttoken_PlatformSpecificData_desktop_linux_tag;

  // Same shape a headless go-librespot device (Raspberry Pi etc.) reports.
  auto& linuxData = sdkData.platform_specific_data.data.desktop_linux;
  linuxData.has_system_name = true;
  pbPutCharArray("Linux", linuxData.system_name);
  linuxData.has_system_release = true;
  pbPutCharArray("6.1.0", linuxData.system_release);
  linuxData.has_system_version = true;
  pbPutCharArray("#1 SMP", linuxData.system_version);
  linuxData.has_hardware = true;
  pbPutCharArray("armv7l", linuxData.hardware);

  auto body = pbEncode(clienttoken_ClientTokenRequest_fields, &request);

  // Bounded retry (3 attempts, 1s apart) - same policy as ContextResolver/
  // PlayerEngine's PUT: a transient failure here (dropped
  // connection, 5xx) used to burn one of updateToken()'s own outer
  // attempts (a full 3s sleep) instead of resolving in a couple seconds.
  // A 4xx is thrown as PermanentHttpFailure to skip the retry budget - not
  // transient, updateToken()'s outer loop is what should decide whether to
  // try the whole login5 flow again from scratch.
  size_t responseSize = 0;
  clienttoken_ClientTokenResponse tokenResponse =
      clienttoken_ClientTokenResponse_init_zero;
  try {
    HttpRetry(3, std::chrono::milliseconds(1000), "clienttoken fetch")
        .run([&]() {
          auto response = bell::HTTPClient::post(
              CLIENT_TOKEN_URL,
              {{"Accept", "application/x-protobuf"},
               {"User-Agent", USER_AGENT}},
              body);

          int status = response->statusCode();
          if (status != 200) {
            std::string reason = "status " + std::to_string(status);
            if (status >= 400 && status < 500) {
              throw PermanentHttpFailure(reason);
            }
            throw std::runtime_error(reason);
          }

          auto responseBytes = response->bytes();
          responseSize = responseBytes.size();
          pbDecode(tokenResponse, clienttoken_ClientTokenResponse_fields,
                   responseBytes);
        });
  } catch (const std::exception&) {
    return "";  // HttpRetry already logged the final giving-up message
  }

  std::string result;
  if (tokenResponse.which_response ==
          clienttoken_ClientTokenResponse_granted_token_tag &&
      tokenResponse.response.granted_token.token != nullptr) {
    auto& granted = tokenResponse.response.granted_token;
    result = std::string(granted.token);

    clientToken = result;
    clientTokenExpiresAt =
        ctx->timeProvider->getSyncedTimestamp() +
        (granted.has_expires_after_seconds
             ? granted.expires_after_seconds * 1000LL
             : 3600 * 1000LL);
    CSPOT_LOG(info, "clienttoken granted (expires after %ds)",
              granted.expires_after_seconds);
  } else if (tokenResponse.has_response_type &&
             tokenResponse.response_type ==
                 clienttoken_ClientTokenResponseType_RESPONSE_CHALLENGES_RESPONSE) {
    // go-librespot doesn't implement clienttoken challenges either and works
    // in production - only add this if it's ever actually seen. See
    // docs/dealer_websocket_migration.md, Fase 1b.
    CSPOT_LOG(error, "clienttoken answered with a challenge (unsupported)");
  } else {
    CSPOT_LOG(error, "unexpected clienttoken response (%d bytes)",
              (int)responseSize);
  }

  pb_release(clienttoken_ClientTokenResponse_fields, &tokenResponse);
  return result;
}

bool Login5Client::login(const std::string& currentClientToken) {
  LoginRequest request = LoginRequest_init_zero;
  pbPutCharArray(LOGIN5_CLIENT_ID, request.client_info.client_id);
  pbPutString(ctx->config.deviceId, request.client_info.device_id);

  request.which_login_method = LoginRequest_stored_credential_tag;
  auto& credential = request.login_method.stored_credential;
  pbPutString(ctx->config.username, credential.username);
  if (ctx->config.authData.size() > sizeof(credential.data.bytes)) {
    CSPOT_LOG(error, "authData too large for login5 stored credential");
    return false;
  }
  std::copy(ctx->config.authData.begin(), ctx->config.authData.end(),
            credential.data.bytes);
  credential.data.size = ctx->config.authData.size();

  for (int attempt = 0; attempt < MAX_LOGIN_TRIES; attempt++) {
    auto body = pbEncode(LoginRequest_fields, &request);

    auto response = bell::HTTPClient::post(
        LOGIN5_URL,
        {{"Accept", "application/x-protobuf"},
         {"User-Agent", USER_AGENT},
         {"Client-Token", currentClientToken}},
        body);

    auto responseBytes = response->bytes();
    LoginResponse loginResponse = LoginResponse_init_zero;
    pbDecode(loginResponse, LoginResponse_fields, responseBytes);

    if (loginResponse.which_response == LoginResponse_ok_tag &&
        loginResponse.response.ok.access_token != nullptr) {
      token = std::string(loginResponse.response.ok.access_token);
      int expiresIn = loginResponse.response.ok.has_access_token_expires_in
                          ? loginResponse.response.ok.access_token_expires_in
                          : 3600;
      expiresAt =
          ctx->timeProvider->getSyncedTimestamp() + (expiresIn * 1000LL);
      CSPOT_LOG(info, "login5 authenticated (token expires in %ds)",
                expiresIn);
      pb_release(LoginResponse_fields, &loginResponse);
      return true;
    }

    if (loginResponse.which_response == LoginResponse_challenges_tag) {
      CSPOT_LOG(info, "login5 answered with %d challenge(s), solving...",
                (int)loginResponse.response.challenges.challenges_count);

      // Echo the server's context and attach one solution per challenge,
      // then retry the same request (librespot login5.rs does exactly this).
      std::vector<uint8_t> loginContext(
          loginResponse.login_context.bytes,
          loginResponse.login_context.bytes +
              loginResponse.login_context.size);
      request.has_login_context = true;
      request.login_context.size = loginResponse.login_context.size;
      memcpy(request.login_context.bytes, loginResponse.login_context.bytes,
             loginResponse.login_context.size);

      request.has_challenge_solutions = true;
      auto& solutions = request.challenge_solutions;
      solutions.solutions_count = 0;

      bool solvedAll = true;
      auto& challenges = loginResponse.response.challenges;
      for (pb_size_t i = 0; i < challenges.challenges_count; i++) {
        if (challenges.challenges[i].which_challenge !=
            Challenge_hashcash_tag) {
          CSPOT_LOG(error, "login5 sent a non-hashcash challenge, giving up");
          solvedAll = false;
          break;
        }

        auto& hashcash = challenges.challenges[i].challenge.hashcash;
        auto& solution = solutions.solutions[solutions.solutions_count];
        solution.which_solution = ChallengeSolution_hashcash_tag;
        solution.solution.hashcash.has_suffix = true;
        solution.solution.hashcash.has_duration = true;
        solution.solution.hashcash.duration.has_seconds = true;
        solution.solution.hashcash.duration.has_nanos = true;

        if (!solveHashcash(loginContext, hashcash.prefix.bytes,
                           hashcash.prefix.size, hashcash.length,
                           solution.solution.hashcash.suffix,
                           solution.solution.hashcash.duration.seconds,
                           solution.solution.hashcash.duration.nanos)) {
          CSPOT_LOG(error, "login5 hashcash timed out (length %d)",
                    (int)hashcash.length);
          solvedAll = false;
          break;
        }
        solutions.solutions_count++;
      }

      pb_release(LoginResponse_fields, &loginResponse);
      if (!solvedAll) {
        return false;
      }
      continue;
    }

    if (loginResponse.which_response == LoginResponse_error_tag) {
      auto error = loginResponse.response.error;
      pb_release(LoginResponse_fields, &loginResponse);
      if (error == LoginError_TIMEOUT || error == LoginError_TOO_MANY_ATTEMPTS) {
        CSPOT_LOG(error, "login5 transient error %d, retrying...", (int)error);
        BELL_SLEEP_MS(2000);
        continue;
      }
      CSPOT_LOG(error, "login5 rejected the login, error %d (status %d)",
                (int)error, response->statusCode());
      return false;
    }

    pb_release(LoginResponse_fields, &loginResponse);
    CSPOT_LOG(error, "unparseable login5 response (status %d, %d bytes)",
              response->statusCode(), (int)responseBytes.size());
    return false;
  }

  CSPOT_LOG(error, "login5 failed after %d attempts", MAX_LOGIN_TRIES);
  return false;
}

// Port of librespot's util::solve_hash_cash (core/src/util.rs) - NOT of
// go-librespot's variant, whose accumulating (never-reset) hasher diverges
// from the canonical algorithm after the first iteration. Find a 16-byte
// suffix such that sha1(prefix || suffix) has >= `length` trailing zero bits
// (checked over the last 8 digest bytes as a big-endian u64).
bool Login5Client::solveHashcash(const std::vector<uint8_t>& loginContext,
                                 const uint8_t* prefix, size_t prefixLen,
                                 int32_t length, uint8_t suffixOut[16],
                                 int64_t& seconds, int32_t& nanos) {
  Crypto crypto;
  crypto.sha1Init();
  crypto.sha1Update(loginContext);
  auto contextSum = crypto.sha1FinalBytes();

  uint64_t target = bigEndian64(contextSum.data() + 12);

  // Single reused input buffer: prefix stays fixed, last 16 bytes (the
  // candidate suffix) are rewritten every iteration.
  std::vector<uint8_t> input(prefixLen + 16);
  memcpy(input.data(), prefix, prefixLen);

  auto start = std::chrono::steady_clock::now();
  for (uint64_t counter = 0;; counter++) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >=
        HASHCASH_TIMEOUT_SECONDS) {
      return false;
    }

    storeBigEndian64(target + counter, input.data() + prefixLen);
    storeBigEndian64(counter, input.data() + prefixLen + 8);

    crypto.sha1Init();
    crypto.sha1Update(input);
    auto digest = crypto.sha1FinalBytes();

    uint64_t tail = bigEndian64(digest.data() + 12);
    int trailingZeros = (tail == 0) ? 64 : __builtin_ctzll(tail);
    if (trailingZeros >= length) {
      memcpy(suffixOut, input.data() + prefixLen, 16);
      auto nanosTotal =
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
              .count();
      seconds = nanosTotal / 1000000000LL;
      nanos = (int32_t)(nanosTotal % 1000000000LL);
      return true;
    }
  }
}
