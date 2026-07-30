#include <doctest/doctest.h>
#include <filesystem>

#include "AuthInfo.h"
#include "SessionStore.h"

TEST_CASE("SessionStore tests") {
  auto path = (std::filesystem::temp_directory_path() /
              "cspot_session_store_test.json")
                 .string();
  std::filesystem::remove(path);

  SUBCASE("load() is a no-op when the file doesn't exist") {
    cspot::SessionStore store(path);
    cspot::AuthInfo authInfo("Cspot player");
    auto deviceIdBefore = authInfo.deviceId;

    store.load(authInfo);

    CHECK(authInfo.deviceId == deviceIdBefore);
    CHECK_FALSE(authInfo.deviceIdWasPersisted);
  }

  SUBCASE("save() then load() round-trips the device id and credentials") {
    cspot::SessionStore store(path);

    cspot::AuthInfo saved("Cspot player");
    cspot_proto::LoginCredentials credentials;
    credentials.username = "someuser";
    credentials.type = AuthenticationType_AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS;
    credentials.authData = {std::byte{1}, std::byte{2}, std::byte{3}};
    saved.loginCredentials = credentials;
    store.save(saved);

    cspot::AuthInfo loaded;
    store.load(loaded);

    CHECK(loaded.deviceId == saved.deviceId);
    CHECK(loaded.deviceIdWasPersisted);
    REQUIRE(loaded.loginCredentials.has_value());
    CHECK(loaded.loginCredentials->username == "someuser");
    CHECK(loaded.loginCredentials->authData == credentials.authData);
  }

  std::filesystem::remove(path);
}
