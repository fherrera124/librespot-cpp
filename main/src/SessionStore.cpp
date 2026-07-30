#include "SessionStore.h"

#include <fstream>
#include <iterator>

using namespace cspot;

SessionStore::SessionStore(std::string path) : path(std::move(path)) {}

void SessionStore::load(AuthInfo& authInfo) const {
  std::ifstream sessionFile(path, std::ios::binary);
  if (!sessionFile.is_open()) {
    return;
  }
  std::string sessionString((std::istreambuf_iterator<char>(sessionFile)),
                            std::istreambuf_iterator<char>());
  sessionFile.close();
  if (!sessionString.empty()) {
    authInfo.assignDataFromJson(sessionString);
  }
}

void SessionStore::save(const AuthInfo& authInfo) const {
  std::ofstream outFile(path, std::ios::binary);
  if (outFile.is_open()) {
    outFile << authInfo.toJson();
    outFile.close();
  }
}
