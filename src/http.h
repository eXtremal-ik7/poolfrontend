#pragma once

#include "poolcore/backend.h"
#include <p2putils/HttpRequestParse.h>

class PoolHttpServer;

class PoolHttpConnection {
public:
  PoolHttpConnection(PoolHttpServer &server, HostAddress address, aioObject *socket) : Server_(server), Address_(address), Socket_(socket) {
    httpRequestParserInit(&ParserState);
    objectSetDestructorCb(aioObjectHandle(Socket_), [](aioObjectRoot*, void *arg) {
      delete static_cast<PoolHttpConnection*>(arg);
    }, this);
  }
  void run();

private:
  static void readCb(AsyncOpStatus status, aioObject*, size_t size, void *arg) { static_cast<PoolHttpConnection*>(arg)->onRead(status, size); }
  static void writeCb(AsyncOpStatus, aioObject*, size_t, void *arg) { static_cast<PoolHttpConnection*>(arg)->onWrite(); }

  void onWrite();
  void onRead(AsyncOpStatus status, size_t size);
  int onParse(HttpRequestComponent *component);
  void close();

  void reply200(xmstream &stream);
  void reply404();
  size_t startChunk(xmstream &stream);
  void finishChunk(xmstream &stream, size_t offset);

  void onUserAction();
  void onUserCreate();
  void onUserResendEmail();
  void onUserLogin();
  void onUserLogout();
  void onUserChangeEmail();
  void onUserChangePassword();
  void onUserRecoveryPassword();
  void onUserGetCredentials();
  void onUserGetSettings();
  void onUserUpdateCredentials();
  void onUserUpdateSettings();
  void onBackendManualPayout();

private:
  enum FunctionTy {
    fnUnknown = 0,
    fnApi,
    // User manager functions
    fnUserAction,
    fnUserCreate,
    fnUserResendEmail,
    fnUserLogin,
    fnUserLogout,
    fnUserChangeEmail,
    fnUserChangePassword,
    fnUserRecoveryPassword,
    fnUserGetCredentials,
    fnUserGetSettings,
    fnUserUpdateCredentials,
    fnUserUpdateSettings,
    // Backend functions
    fnBackendManualPayout
    // Statistic functions
  };

  static std::unordered_map<std::string, std::pair<int, PoolHttpConnection::FunctionTy>> FunctionNameMap_;

  PoolHttpServer &Server_;
  HostAddress Address_;
  aioObject *Socket_;

  char buffer[65536];
  HttpRequestParserState ParserState;
  size_t oldDataSize = 0;
  std::atomic<unsigned> Deleted_ = 0;

  struct {
    int method = hmUnknown;
    FunctionTy function = fnUnknown;
    std::string Request;
  } Context;
};

class PoolHttpServer {
public:
  PoolHttpServer(asyncBase *base,
                 uint16_t port,
                 UserManager &userMgr,
                 std::vector<PoolBackend> &backends,
                 std::unordered_map<std::string, size_t> &coinIdxMap);

  bool start();

  UserManager &userManager() { return UserMgr_; }

private:
  static void acceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socketFd, void *arg);

  void onAccept(AsyncOpStatus status, aioObject *object);

private:
  asyncBase *Base_;
  uint16_t Port_;
  UserManager &UserMgr_;
  std::vector<PoolBackend> &Backends_;
  std::unordered_map<std::string, size_t> &CoinIdxMap_;

  aioObject *ListenerSocket_;
};
