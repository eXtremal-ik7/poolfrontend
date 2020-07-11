#include "http.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include "loguru.hpp"
#include "rapidjson/document.h"


std::unordered_map<std::string, std::pair<int, PoolHttpConnection::FunctionTy>> PoolHttpConnection::FunctionNameMap_ = {
  // User manager functions
  {"useraction", {hmPost, fnUserAction}},
  {"usercreate", {hmPost, fnUserCreate}},
  {"userresendemail", {hmPost, fnUserResendEmail}},
  {"userlogin", {hmPost, fnUserLogin}},
  {"userlogout", {hmPost, fnUserLogout}},
  {"userchangeemail", {hmPost, fnUserChangeEmail}},
  {"userchangepassword", {hmPost, fnUserChangePassword}},
  {"userrecoverypassword", {hmPost, fnUserRecoveryPassword}},
  {"usergetcredentials", {hmGet, fnUserGetCredentials}},
  {"usergetsettings", {hmGet, fnUserGetSettings}},
  {"userupdatecredentials", {hmPost, fnUserUpdateCredentials}},
  {"userupdatesettings", {hmPost, fnUserUpdateSettings}},
  // Backend functions
  {"backendmanualpayout", {hmPost, fnBackendManualPayout}}
};

static inline bool rawcmp(Raw data, const char *operand) {
  size_t opSize = strlen(operand);
  return data.size == opSize && memcmp(data.data, operand, opSize) == 0;
}

static inline void jsonSerializeString(xmstream &stream, const char *name, const char *value, bool lastField = false)
{
  stream.write("\"");
  stream.write(name);
  stream.write("\": \"");
  stream.write(value);
  stream.write("\"");
  if (!lastField)
    stream.write(',');
}

static inline void jsonParseString(rapidjson::Document &document, const char *name, std::string &out, bool *validAcc) {
  if (document.HasMember(name)) {
    if (document[name].IsString())
      out = document[name].GetString();
    else
      *validAcc = false;
  }
}

static inline bool parseUserCredentials(const char *json, UserManager::Credentials &credentials)
{
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(json);
  jsonParseString(document, "login", credentials.Login, &validAcc);
  jsonParseString(document, "password", credentials.Password, &validAcc);
  jsonParseString(document, "name", credentials.Name, &validAcc);
  jsonParseString(document, "email", credentials.EMail, &validAcc);
  return validAcc;
}

void PoolHttpConnection::run()
{
  aioRead(Socket_, buffer, sizeof(buffer), afNone, 0, readCb, this);
}

int PoolHttpConnection::onParse(HttpRequestComponent *component)
{
  if (component->type == httpRequestDtMethod) {
    Context.method = component->method;
    Context.function = fnUnknown;
    return 1;
  }

  if (component->type == httpRequestDtUriPathElement) {
    // Wait 'api'
    if (Context.function == fnUnknown && rawcmp(component->data, "api")) {
      Context.function = fnApi;
    } else if (Context.function == fnApi) {
      std::string functionName(component->data.data, component->data.data + component->data.size);
      auto It = FunctionNameMap_.find(functionName);
      if (It == FunctionNameMap_.end() || It->second.first != Context.method) {
        reply404();
        return 0;
      }

      Context.function = It->second.second;
      return 1;
    } else {
      reply404();
      return 0;
    }
  } else if (component->type == httpRequestDtData) {
    Context.Request.append(component->data.data, component->data.data + component->data.size);
    return 1;
  } else if (component->type == httpRequestDtDataLast) {
    Context.Request.append(component->data.data, component->data.data + component->data.size);
    switch (Context.function) {
      case fnUserAction: onUserAction(); break;
      case fnUserCreate: onUserCreate(); break;
      case fnUserResendEmail: onUserResendEmail(); break;
      case fnUserLogin: onUserLogin(); break;
      case fnUserLogout: onUserLogout(); break;
      case fnUserChangeEmail: onUserChangeEmail(); break;
      case fnUserChangePassword: onUserChangePassword(); break;
      case fnUserRecoveryPassword: onUserRecoveryPassword(); break;
      case fnUserGetCredentials: onUserGetCredentials(); break;
      case fnUserGetSettings: onUserGetSettings(); break;
      case fnUserUpdateCredentials: onUserUpdateCredentials(); break;
      case fnUserUpdateSettings: onUserUpdateSettings(); break;
      case fnBackendManualPayout: onBackendManualPayout(); break;
      default:
        reply404();
        return 0;
    }
  }

  return 1;
}

void PoolHttpConnection::onWrite()
{
  // TODO: check keep alive
  socketShutdown(aioObjectSocket(Socket_), SOCKET_SHUTDOWN_READWRITE);
  aioRead(Socket_, buffer, sizeof(buffer), afNone, 0, readCb, this);
}

void PoolHttpConnection::onRead(AsyncOpStatus status, size_t size)
{
  if (status != aosSuccess) {
    close();
    return;
  }

  httpRequestSetBuffer(&ParserState, buffer + oldDataSize, sizeof(buffer) - oldDataSize);

  switch (httpRequestParse(&ParserState, [](HttpRequestComponent *component, void *arg) -> int { return static_cast<PoolHttpConnection*>(arg)->onParse(component); }, this)) {
    case ParserResultOk : {
      // TODO: check keep-alive
      break;
    }

    case ParserResultNeedMoreData : {
      // copy 'tail' to begin of buffer
      oldDataSize = httpRequestDataRemaining(&ParserState);
      if (oldDataSize)
        memcpy(buffer, httpRequestDataPtr(&ParserState), oldDataSize);
      aioRead(Socket_, buffer+oldDataSize, sizeof(buffer)-oldDataSize, afNone, 0, readCb, this);
      break;
    }

    case ParserResultError : {
      close();
      break;
    }

    case ParserResultCancelled : {
      close();
      break;
    }
  }
}

void PoolHttpConnection::reply200(xmstream &stream)
{
  const char reply200[] = "HTTP/1.1 200 OK\r\nServer: bcnode\r\nTransfer-Encoding: chunked\r\n\r\n";
  stream.write(reply200, sizeof(reply200)-1);
}

void PoolHttpConnection::reply404()
{
  const char reply404[] = "HTTP/1.1 404 Not Found\r\nServer: bcnode\r\nTransfer-Encoding: chunked\r\n\r\n";
  const char html[] = "<html><head><title>Not Found</title></head><body><h1>404 Not Found</h1></body></html>";

  char buffer[4096];
  xmstream stream(buffer, sizeof(buffer));
  stream.write(reply404, sizeof(reply404)-1);

  size_t offset = startChunk(stream);
  stream.write(html);
  finishChunk(stream, offset);

  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

size_t PoolHttpConnection::startChunk(xmstream &stream)
{
  size_t offset = stream.offsetOf();
  stream.write("00000000\r\n", 10);
  return offset;
}

void PoolHttpConnection::finishChunk(xmstream &stream, size_t offset)
{
  char hex[16];
  char finishData[] = "\r\n0\r\n\r\n";
  sprintf(hex, "%08x", static_cast<unsigned>(stream.offsetOf() - offset - 10));
  memcpy(stream.data<uint8_t>() + offset, hex, 8);
  stream.write(finishData, sizeof(finishData));
}

void PoolHttpConnection::close()
{
  if (Deleted_++ == 0)
    deleteAioObject(Socket_);
}

void PoolHttpConnection::onUserAction()
{
  std::string actionId;
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  jsonParseString(document, "id", actionId, &validAcc);
  if (!validAcc) {
    // TODO: use different error code
    reply404();
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userAction(actionId, [this](bool success, const std::string &status) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    stream.write('{');
    jsonSerializeString(stream, "status", status.c_str(), true);
    stream.write("}\n");

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserCreate()
{
  UserManager::Credentials credentials;
  if (!parseUserCredentials(Context.Request.c_str(), credentials)) {
    // TODO: use different error code
    reply404();
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userCreate(std::move(credentials), [this](bool success, const std::string &status) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    stream.write('{');
    jsonSerializeString(stream, "status", status.c_str(), true);
    stream.write("}\n");

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserResendEmail()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserLogin()
{
  UserManager::Credentials credentials;
  if (!parseUserCredentials(Context.Request.c_str(), credentials)) {
    // TODO: use different error code
    reply404();
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userLogin(std::move(credentials), [this](const std::string &sessionId, const std::string &status) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    stream.write('{');
    jsonSerializeString(stream, "sessionId", sessionId.c_str());
    jsonSerializeString(stream, "status", status.c_str(), true);
    stream.write("}\n");

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserLogout()
{
  std::string sessionId;
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  jsonParseString(document, "id", sessionId, &validAcc);
  if (!validAcc) {
    // TODO: use different error code
    reply404();
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userLogout(sessionId, [this](bool success, const std::string &status) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    stream.write('{');
    jsonSerializeString(stream, "status", status.c_str(), true);
    stream.write("}\n");

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserChangeEmail()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserChangePassword()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserRecoveryPassword()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserGetCredentials()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserGetSettings()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserUpdateCredentials()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserUpdateSettings()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendManualPayout()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

PoolHttpServer::PoolHttpServer(asyncBase *base,
                               uint16_t port,
                               UserManager &userMgr,
                               std::vector<PoolBackend> &backends,
                               std::unordered_map<std::string, size_t> &coinIdxMap) :
  Base_(base),
  Port_(port),
  UserMgr_(userMgr),
  Backends_(backends),
  CoinIdxMap_(coinIdxMap)
{
}

bool PoolHttpServer::start()
{
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(Port_);
  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(hSocket);

  if (socketBind(hSocket, &address) != 0) {
    LOG_F(ERROR, "PoolHttpServer: can't bind port %u\n", static_cast<unsigned>(Port_));
    return false;
  }

  if (socketListen(hSocket) != 0) {
    LOG_F(ERROR, "PoolHttpServer: can't listen port %u\n", static_cast<unsigned>(Port_));
    return false;
  }

  ListenerSocket_ = newSocketIo(Base_, hSocket);
  aioAccept(ListenerSocket_, 0, acceptCb, this);
  return true;
}


void PoolHttpServer::acceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socketFd, void *arg)
{
  if (status == aosSuccess) {
    aioObject *connectionSocket = newSocketIo(aioGetBase(object), socketFd);
    PoolHttpConnection *connection = new PoolHttpConnection(*static_cast<PoolHttpServer*>(arg), address, connectionSocket);
    connection->run();
  } else {
    LOG_F(ERROR, "HTTP api accept connection failed");
  }

  aioAccept(object, 0, acceptCb, arg);
}
