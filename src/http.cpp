#include "http.h"
#include "poolcommon/utils.h"
#include "poolcore/thread.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include "loguru.hpp"
#include "rapidjson/document.h"
#include "poolcommon/jsonSerializer.h"

std::unordered_map<std::string, std::pair<int, PoolHttpConnection::FunctionTy>> PoolHttpConnection::FunctionNameMap_ = {
  // User manager functions
  {"userAction", {hmPost, fnUserAction}},
  {"userCreate", {hmPost, fnUserCreate}},
  {"userResendEmail", {hmPost, fnUserResendEmail}},
  {"userLogin", {hmPost, fnUserLogin}},
  {"userLogout", {hmPost, fnUserLogout}},
  {"userChangeEmail", {hmPost, fnUserChangeEmail}},
  {"userChangePassword", {hmPost, fnUserChangePassword}},
  {"userGetCredentials", {hmPost, fnUserGetCredentials}},
  {"userGetSettings", {hmPost, fnUserGetSettings}},
  {"userUpdateCredentials", {hmPost, fnUserUpdateCredentials}},
  {"userUpdateSettings", {hmPost, fnUserUpdateSettings}},
  {"userEnumerateAll", {hmPost, fnUserEnumerateAll}},
  // Backend functions
  {"backendManualPayout", {hmPost, fnBackendManualPayout}},
  {"backendQueryFoundBlocks", {hmPost, fnBackendQueryFoundBlocks}},
  {"backendQueryPayouts", {hmPost, fnBackendQueryPayouts}},
  {"backendQueryPoolBalance", {hmPost, fnBackendQueryPoolBalance}},
  {"backendQueryPoolStats", {hmPost, fnBackendQueryPoolStats}},
  {"backendQueryPoolStatsHistory", {hmPost, fnBackendQueryPoolStatsHistory}},
  {"backendQueryUserBalance", {hmPost, fnBackendQueryUserBalance}},
  {"backendQueryUserStats", {hmPost, fnBackendQueryUserStats}},
  {"backendQueryUserStatsHistory", {hmPost, fnBackendQueryUserStatsHistory}},
  {"backendQueryWorkerStatsHistory", {hmPost, fnBackendQueryWorkerStatsHistory}}
};

static inline bool rawcmp(Raw data, const char *operand) {
  size_t opSize = strlen(operand);
  return data.size == opSize && memcmp(data.data, operand, opSize) == 0;
}

static inline void jsonSerializeNull(xmstream &stream, const char *name, bool lastField = false)
{
  stream.write("\"");
  stream.write(name);
  stream.write("\": null");
  if (!lastField)
    stream.write(',');
}

static inline void jsonSerializeBoolean(xmstream &stream, const char *name, bool value, bool lastField = false)
{
  stream.write("\"");
  stream.write(name);
  stream.write("\": ");
  stream.write(value ? "true" : "false");
  if (!lastField)
    stream.write(',');
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

template<typename T>
static inline void jsonSerializeInt(xmstream &stream, const char *name, T number, bool lastField = false)
{
  char N[32];
  xitoa(number, N);
  stream.write("\"");
  stream.write(name);
  stream.write("\": ");
  stream.write(static_cast<const char*>(N));
  if (!lastField)
    stream.write(',');
}

static inline void jsonParseString(rapidjson::Document &document, const char *name, std::string &out, bool *validAcc) {
  if (document.HasMember(name) && document[name].IsString())
    out = document[name].GetString();
  else
    *validAcc = false;
}

static inline void jsonParseString(rapidjson::Document &document, const char *name, std::string &out, const std::string &defaultValue, bool *validAcc) {
  if (document.HasMember(name) && document[name].IsString())
    out = document[name].GetString();
  else
    out = defaultValue;
}


static inline void jsonParseInt64(rapidjson::Document &document, const char *name, int64_t *out, int64_t defaultValue, bool *validAcc) {
  if (document.HasMember(name) && document[name].IsInt64())
    *out = document[name].GetInt64();
  else
    *out = defaultValue;
}


static inline void jsonParseUInt64(rapidjson::Document &document, const char *name, uint64_t *out, int64_t defaultValue, bool *validAcc) {
  if (document.HasMember(name) && document[name].IsUint64())
    *out = document[name].GetUint64();
  else
    *out = defaultValue;
}

static inline void jsonParseUInt(rapidjson::Document &document, const char *name, unsigned *out, unsigned defaultValue, bool *validAcc) {
  if (document.HasMember(name) && document[name].IsUint())
    *out = document[name].GetUint();
  else
    *out = defaultValue;
}

static inline void jsonParseBoolean(rapidjson::Document &document, const char *name, bool *out, bool *validAcc) {
  if (document.HasMember(name) && document[name].IsBool())
    *out = document[name].GetBool();
  else
    *validAcc = false;
}

static inline bool parseUserCredentials(const char *json, UserManager::Credentials &credentials)
{
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(json);
  if (document.HasParseError())
    return false;
  jsonParseString(document, "login", credentials.Login, "", &validAcc);
  jsonParseString(document, "password", credentials.Password, "", &validAcc);
  jsonParseString(document, "name", credentials.Name, "", &validAcc);
  jsonParseString(document, "email", credentials.EMail, "", &validAcc);
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
      case fnUserGetCredentials: onUserGetCredentials(); break;
      case fnUserGetSettings: onUserGetSettings(); break;
      case fnUserUpdateCredentials: onUserUpdateCredentials(); break;
      case fnUserUpdateSettings: onUserUpdateSettings(); break;
      case fnUserEnumerateAll: onUserEnumerateAll(); break;
      case fnBackendManualPayout: onBackendManualPayout(); break;
      case fnBackendQueryUserBalance: onBackendQueryUserBalance(); break;
      case fnBackendQueryUserStats: onBackendQueryUserStats(); break;
      case fnBackendQueryUserStatsHistory: onBackendQueryUserStatsHistory(); break;
      case fnBackendQueryWorkerStatsHistory: onBackendQueryWorkerStatsHistory(); break;
      case fnBackendQueryFoundBlocks: onBackendQueryFoundBlocks(); break;
      case fnBackendQueryPayouts: onBackendQueryPayouts(); break;
      case fnBackendQueryPoolBalance: onBackendQueryPoolBalance(); break;
      case fnBackendQueryPoolStats: onBackendQueryPoolStats(); break;
      case fnBackendQueryPoolStatsHistory : onBackendQueryPoolStatsHistory(); break;
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

void PoolHttpConnection::onRead(AsyncOpStatus status, size_t)
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
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  jsonParseString(document, "id", actionId, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userAction(actionId, [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserCreate()
{
  UserManager::Credentials credentials;
  if (!parseUserCredentials(Context.Request.c_str(), credentials)) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userCreate(std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserResendEmail()
{
  UserManager::Credentials credentials;
  if (!parseUserCredentials(Context.Request.c_str(), credentials)) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userResendEmail(std::move(credentials), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserLogin()
{
  UserManager::Credentials credentials;
  if (!parseUserCredentials(Context.Request.c_str(), credentials)) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userLogin(std::move(credentials), [this](const std::string &sessionId, const char *status) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    stream.write('{');
    jsonSerializeString(stream, "sessionid", sessionId.c_str());
    jsonSerializeString(stream, "status", status, true);
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
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  jsonParseString(document, "id", sessionId, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().userLogout(sessionId, [this](const char *status) {
    replyWithStatus(status);
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

void PoolHttpConnection::onUserGetCredentials()
{
  std::string sessionId;
  std::string targetLogin;
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write('{');

  std::string login;
  UserManager::Credentials credentials;
  if (Server_.userManager().validateSession(sessionId, targetLogin, login)) {
    if (Server_.userManager().getUserCredentials(login, credentials)) {
      jsonSerializeString(stream, "status", "ok");
      jsonSerializeString(stream, "name", credentials.Name.c_str());
      jsonSerializeString(stream, "email", credentials.EMail.c_str());
      jsonSerializeInt(stream, "registrationDate", credentials.RegistrationDate, true);
    } else {
      jsonSerializeString(stream, "status", "unknown_id", true);
    }
  } else {
    jsonSerializeString(stream, "status", "unknown_id", true);
  }

  stream.write("}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onUserGetSettings()
{
  std::string sessionId;
  std::string targetLogin;
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write('{');
  jsonSerializeString(stream, "status", "ok");
  stream.write("\"coins\": [");

  std::string login;
  if (Server_.userManager().validateSession(sessionId, targetLogin, login)) {
    bool firstIteration = true;
    for (const auto &coinInfo: Server_.userManager().coinInfo()) {
      if (!firstIteration)
        stream.write(',');
      stream.write('{');
      jsonSerializeString(stream, "name", coinInfo.Name.c_str());

      UserSettingsRecord settings;
      if (Server_.userManager().getUserCoinSettings(login, coinInfo.Name, settings)) {
        jsonSerializeString(stream, "address", settings.Address.c_str());
        jsonSerializeString(stream, "payoutThreshold", FormatMoney(settings.MinimalPayout, coinInfo.RationalPartSize).c_str());
        jsonSerializeBoolean(stream, "autoPayoutEnabled", settings.AutoPayout, true);
      } else {
        jsonSerializeNull(stream, "address");
        jsonSerializeNull(stream, "payoutThreshold");
        jsonSerializeBoolean(stream, "autoPayoutEnabled", false, true);
      }
      stream.write("}");
      firstIteration = false;
    }
  } else {
    jsonSerializeString(stream, "status", "unknown_id", true);
  }

  stream.write("]}\n");
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
  std::string sessionId;
  std::string targetLogin;
  UserSettingsRecord settings;

  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  std::string payoutThreshold;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", settings.Coin, &validAcc);
  jsonParseString(document, "address", settings.Address, &validAcc);
  jsonParseString(document, "payoutThreshold", payoutThreshold, &validAcc);
  jsonParseBoolean(document, "autoPayoutEnabled", &settings.AutoPayout, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  auto It = Server_.userManager().coinIdxMap().find(settings.Coin);
  if (It == Server_.userManager().coinIdxMap().end()) {
    replyWithStatus("invalid_coin");
    return;
  }

  CCoinInfo &coinInfo = Server_.userManager().coinInfo()[It->second];
  if (!parseMoneyValue(payoutThreshold.c_str(), coinInfo.RationalPartSize, &settings.MinimalPayout)) {
    replyWithStatus("request_format_error");
    return;
  }

  if (!coinInfo.checkAddress(settings.Address, coinInfo.PayoutAddressType)) {
    replyWithStatus("invalid_address");
    return;
  }

  if (!Server_.userManager().validateSession(sessionId, targetLogin, settings.Login)) {
    replyWithStatus("unknown_id");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().updateSettings(std::move(settings), [this](const char *status) {
    replyWithStatus(status);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onUserEnumerateAll()
{
  std::string sessionId;
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  jsonParseString(document, "id", sessionId, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  std::string login;
  if (!Server_.userManager().validateSession(sessionId, "", login) || login != "admin") {
    replyWithStatus("unknown_id");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  Server_.userManager().enumerateUsers([this](const std::vector<UserManager::Credentials> &allUsers) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object object(stream);
      object.addString("status", "ok");
      object.addField("users");
      {
        JSON::Array usersArray(stream);
        for (const auto &user: allUsers) {
          usersArray.addField();
          {
            JSON::Object userObject(stream);
            userObject.addString("login", user.Login);
            userObject.addString("name", user.Name);
            userObject.addString("email", user.EMail);
            userObject.addInt("registrationDate", user.RegistrationDate);
          }
        }
      }
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendManualPayout()
{
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  std::string login;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, login)) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->manualPayout(login, [this](bool result) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    stream.write('{');
    jsonSerializeString(stream, "status", "ok");
    jsonSerializeBoolean(stream, "result", result, true);
    stream.write('}');

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryUserBalance()
{
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  std::string login;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, login)) {
    replyWithStatus("unknown_id");
    return;
  }

  if (!coin.empty()) {
    PoolBackend *backend = Server_.backend(coin);
    if (!backend) {
      replyWithStatus("invalid_coin");
      return;
    }

    objectIncrementReference(aioObjectHandle(Socket_), 1);
    backend->queryUserBalance(login, [this, backend](const UserBalanceRecord &record) {
      xmstream stream;
      reply200(stream);
      size_t offset = startChunk(stream);
      const CCoinInfo &coinInfo = backend->getCoinInfo();
      {
        JSON::Object object(stream);
        object.addString("status", "ok");
        object.addField("balances");
        {
          JSON::Array allBalances(stream);
          allBalances.addField();
          {
            JSON::Object balance(stream);
            balance.addString("coin", coinInfo.Name);
            balance.addString("balance", FormatMoney(record.Balance, coinInfo.RationalPartSize));
            balance.addString("requested", FormatMoney(record.Requested, coinInfo.RationalPartSize));
            balance.addString("paid", FormatMoney(record.Paid, coinInfo.RationalPartSize));
          }
        }
      }

      finishChunk(stream, offset);
      aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  } else {
    // Ask all backends about balances
    objectIncrementReference(aioObjectHandle(Socket_), 1);
    PoolBackend::queryUserBalanceMulti(&Server_.backends()[0], Server_.backends().size(), login, [this](const UserBalanceRecord *balanceData, size_t backendsNum) {
      xmstream stream;
      reply200(stream);
      size_t offset = startChunk(stream);
      {
        JSON::Object object(stream);
        object.addString("status", "ok");
        object.addField("balances");
        {
          JSON::Array allBalances(stream);
          for (size_t i = 0; i < backendsNum; i++) {
            const CCoinInfo &coinInfo = Server_.backend(i)->getCoinInfo();
            allBalances.addField();
            {
              JSON::Object balance(stream);
              balance.addString("coin", coinInfo.Name);
              balance.addString("balance", FormatMoney(balanceData[i].Balance, coinInfo.RationalPartSize));
              balance.addString("requested", FormatMoney(balanceData[i].Requested, coinInfo.RationalPartSize));
              balance.addString("paid", FormatMoney(balanceData[i].Paid, coinInfo.RationalPartSize));
            }

          }
        }
      }

      finishChunk(stream, offset);
      aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  }
}

void PoolHttpConnection::onBackendQueryUserStats()
{
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  std::string login;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, login)) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->queryUserStats(login, [this, backend](const StatisticDb::CStats &aggregate, const std::vector<StatisticDb::CStats> &workers) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object object(stream);
      object.addString("status", "ok");
      object.addString("powerUnit", backend->getCoinInfo().getPowerUnitName());
      object.addInt("powerMultLog10", backend->getCoinInfo().PowerMultLog10);
      object.addField("total");
      {
        JSON::Object total(stream);
        total.addInt("clients", aggregate.ClientsNum);
        total.addInt("workers", aggregate.WorkersNum);
        total.addDouble("shareRate", aggregate.SharesPerSecond);
        total.addDouble("shareWork", aggregate.SharesWork);
        total.addInt("power", aggregate.AveragePower);
        total.addInt("lastShareTime", aggregate.LastShareTime);
      }

      object.addField("workers");
      {
        JSON::Array workersOutput(stream);
        for (size_t i = 0, ie = workers.size(); i != ie; ++i) {
          workersOutput.addField();
          {
            JSON::Object workerOutput(stream);
            workerOutput.addString("name", workers[i].WorkerId);
            workerOutput.addDouble("shareRate", workers[i].SharesPerSecond);
            workerOutput.addDouble("shareWork", workers[i].SharesWork);
            workerOutput.addInt("power", workers[i].AveragePower);
            workerOutput.addInt("lastShareTime", workers[i].LastShareTime);
          }
        }
      }
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::queryStatsHistory(PoolBackend *backend, const std::string &login, const std::string &worker, int64_t timeFrom, int64_t timeTo, int64_t groupByInterval)
{
  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->queryStatsHistory(login, worker, timeFrom, timeTo, groupByInterval, [this, backend](const std::vector<StatisticDb::CStats> &stats) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);

    {
      JSON::Object object(stream);
      object.addString("status", "ok");
      object.addString("powerUnit", backend->getCoinInfo().getPowerUnitName());
      object.addInt("powerMultLog10", backend->getCoinInfo().PowerMultLog10);
      object.addField("stats");
      {
        JSON::Array workersOutput(stream);
        for (size_t i = 0, ie = stats.size(); i != ie; ++i) {
          workersOutput.addField();
          {
            JSON::Object workerOutput(stream);
            workerOutput.addString("name", stats[i].WorkerId);
            workerOutput.addInt("time", stats[i].Time);
            workerOutput.addDouble("shareRate", stats[i].SharesPerSecond);
            workerOutput.addDouble("shareWork", stats[i].SharesWork);
            workerOutput.addInt("power", stats[i].AveragePower);
          }
        }
      }
    }

    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryUserStatsHistory()
{
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  int64_t currentTime = time(nullptr);
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  int64_t timeFrom;
  int64_t timeTo;
  int64_t groupByInterval;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, currentTime - 24*3600, &validAcc);
  jsonParseInt64(document, "timeTo", &timeTo, currentTime, &validAcc);
  jsonParseInt64(document, "groupByInterval", &groupByInterval, 3600, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  std::string login;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, login)) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  queryStatsHistory(backend, login, "", timeFrom, timeTo, groupByInterval);
}

void PoolHttpConnection::onBackendQueryWorkerStatsHistory()
{
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  int64_t currentTime = time(nullptr);
  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  std::string workerId;
  int64_t timeFrom;
  int64_t timeTo;
  int64_t groupByInterval;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseString(document, "workerId", workerId, &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, currentTime - 24*3600, &validAcc);
  jsonParseInt64(document, "timeTo", &timeTo, currentTime, &validAcc);
  jsonParseInt64(document, "groupByInterval", &groupByInterval, 3600, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  std::string login;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, login)) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  queryStatsHistory(backend, login, workerId, timeFrom, timeTo, groupByInterval);
}

void PoolHttpConnection::onBackendQueryFoundBlocks()
{
  bool validAcc = true;
  std::string coin;
  int64_t heightFrom;
  std::string hashFrom;
  uint32_t count;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  jsonParseString(document, "coin", coin, &validAcc);
  jsonParseInt64(document, "heightFrom", &heightFrom, -1, &validAcc);
  jsonParseString(document, "hashFrom", hashFrom, "", &validAcc);
  jsonParseUInt(document, "count", &count, 20, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  const CCoinInfo &coinInfo = backend->getCoinInfo();

  objectIncrementReference(aioObjectHandle(Socket_), 1);
  backend->queryFoundBlocks(heightFrom, hashFrom, count, [this, &coinInfo](const std::vector<FoundBlockRecord> &blocks, const std::vector<CNetworkClient::GetBlockConfirmationsQuery> &confirmations) {
    xmstream stream;
    reply200(stream);
    size_t offset = startChunk(stream);
    stream.write('{');
    jsonSerializeString(stream, "status", "ok");
    stream.write("\"blocks\": [");
    for (size_t i = 0, ie = blocks.size(); i != ie; ++i) {
      if (i != 0)
        stream.write(',');
      stream.write('{');
      jsonSerializeInt(stream, "height", blocks[i].Height);
      jsonSerializeString(stream, "hash", blocks[i].Hash.c_str());
      jsonSerializeInt(stream, "time", blocks[i].Time);
      jsonSerializeInt(stream, "confirmations", confirmations[i].Confirmations);
      jsonSerializeString(stream, "generatedCoins", FormatMoney(blocks[i].AvailableCoins, coinInfo.RationalPartSize).c_str());
      jsonSerializeString(stream, "foundBy", blocks[i].FoundBy.c_str(), true);
      stream.write('}');
    }
    stream.write("]}");
    finishChunk(stream, offset);
    aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
    objectDecrementReference(aioObjectHandle(Socket_), 1);
  });
}

void PoolHttpConnection::onBackendQueryPayouts()
{
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  std::string sessionId;
  std::string targetLogin;
  std::string coin;
  uint64_t timeFrom;
  unsigned count;
  jsonParseString(document, "id", sessionId, &validAcc);
  jsonParseString(document, "targetLogin", targetLogin, "", &validAcc);
  jsonParseString(document, "coin", coin, "", &validAcc);
  jsonParseUInt64(document, "timeFrom", &timeFrom, 0, &validAcc);
  jsonParseUInt(document, "count", &count, 20, &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  // id -> login
  std::string login;
  if (!Server_.userManager().validateSession(sessionId, targetLogin, login)) {
    replyWithStatus("unknown_id");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  std::vector<PayoutDbRecord> records;
  backend->queryPayouts(login, timeFrom, count, records);
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write('{');
  jsonSerializeString(stream, "status", "ok");
  stream.write("\"payouts\": [");
  for (size_t i = 0, ie = records.size(); i != ie; ++i) {
    if (i != 0)
      stream.write(',');
    stream.write('{');
    jsonSerializeInt(stream, "time", records[i].time);
    jsonSerializeString(stream, "txid", records[i].transactionId.c_str());
    jsonSerializeString(stream, "value", FormatMoney(records[i].value, backend->getCoinInfo().RationalPartSize).c_str(), true);
    stream.write('}');
  }
  stream.write("]}");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendQueryPoolBalance()
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write("{\"error\": \"not implemented\"}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void PoolHttpConnection::onBackendQueryPoolStats()
{
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  std::string coin;
  jsonParseString(document, "coin", coin, "", &validAcc);
  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  if (!coin.empty()) {
    PoolBackend *backend = Server_.backend(coin);
    if (!backend) {
      replyWithStatus("invalid_coin");
      return;
    }

    objectIncrementReference(aioObjectHandle(Socket_), 1);
    backend->queryPoolStats([this, backend](const StatisticDb::CStats &record) {
      xmstream stream;
      reply200(stream);
      size_t offset = startChunk(stream);
      const CCoinInfo &coinInfo = backend->getCoinInfo();

      {
        JSON::Object object(stream);
        object.addString("status", "ok");
        object.addField("stats");
        {
          JSON::Array statsArray(stream);
          statsArray.addField();
          {
            JSON::Object statsObject(stream);
            statsObject.addString("coin", coinInfo.Name);
            statsObject.addString("powerUnit", backend->getCoinInfo().getPowerUnitName());
            statsObject.addInt("powerMultLog10", backend->getCoinInfo().PowerMultLog10);
            statsObject.addInt("clients", record.ClientsNum);
            statsObject.addInt("workers", record.WorkersNum);
            statsObject.addDouble("shareRate", record.SharesPerSecond);
            statsObject.addDouble("shareWork", record.SharesWork);
            statsObject.addInt("power", record.AveragePower);
            statsObject.addInt("lastShareTime", record.LastShareTime);
          }
        }
      }

      finishChunk(stream, offset);
      aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  } else {
    // Ask all backends about stats
    objectIncrementReference(aioObjectHandle(Socket_), 1);
    PoolBackend::queryPoolStatsMulti(&Server_.backends()[0], Server_.backends().size(), [this](const StatisticDb::CStats *stats, size_t backendsNum) {
      xmstream stream;
      reply200(stream);
      size_t offset = startChunk(stream);

      {
        JSON::Object object(stream);
        object.addString("status", "ok");
        object.addField("stats");
        {
          JSON::Array statsArray(stream);
          for (size_t i = 0; i < backendsNum; i++) {
            const CCoinInfo &coinInfo = Server_.backend(i)->getCoinInfo();
            statsArray.addField();
            {
              JSON::Object statsObject(stream);
              statsObject.addString("coin", coinInfo.Name);
              statsObject.addString("powerUnit", coinInfo.getPowerUnitName());
              statsObject.addInt("powerMultLog10", coinInfo.PowerMultLog10);
              statsObject.addInt("clients", stats[i].ClientsNum);
              statsObject.addInt("workers", stats[i].WorkersNum);
              statsObject.addDouble("shareRate", stats[i].SharesPerSecond);
              statsObject.addDouble("shareWork", stats[i].SharesWork);
              statsObject.addInt("power", stats[i].AveragePower);
            }
          }
        }
      }

      finishChunk(stream, offset);
      aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
      objectDecrementReference(aioObjectHandle(Socket_), 1);
    });
  }
}

void PoolHttpConnection::onBackendQueryPoolStatsHistory()
{
  bool validAcc = true;
  rapidjson::Document document;
  document.Parse(Context.Request.c_str());
  if (document.HasParseError()) {
    replyWithStatus("json_format_error");
    return;
  }

  int64_t currentTime = time(nullptr);
  std::string coin;
  int64_t timeFrom;
  int64_t timeTo;
  int64_t groupByInterval;
  jsonParseString(document, "coin", coin, "", &validAcc);
  jsonParseInt64(document, "timeFrom", &timeFrom, currentTime - 24*3600, &validAcc);
  jsonParseInt64(document, "timeTo", &timeTo, currentTime, &validAcc);
  jsonParseInt64(document, "groupByInterval", &groupByInterval, 3600, &validAcc);

  if (!validAcc) {
    replyWithStatus("json_format_error");
    return;
  }

  PoolBackend *backend = Server_.backend(coin);
  if (!backend) {
    replyWithStatus("invalid_coin");
    return;
  }

  queryStatsHistory(backend, "", "", timeFrom, timeTo, groupByInterval);
}


PoolHttpServer::PoolHttpServer(uint16_t port,
                               UserManager &userMgr,
                               std::vector<std::unique_ptr<PoolBackend>> &backends,
                               std::unordered_map<std::string, size_t> &coinIdxMap) :
  Port_(port),
  UserMgr_(userMgr),
  CoinIdxMap_(coinIdxMap)
{
  Base_ = createAsyncBase(amOSDefault);
  Backends_.resize(backends.size());
  for (size_t i = 0, ie = backends.size(); i != ie; ++i)
    Backends_[i] = backends[i].get();
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
  Thread_ = std::thread([](PoolHttpServer *server) {
    loguru::set_thread_name("http");
    InitializeWorkerThread();
    LOG_F(INFO, "http server started tid=%u", GetGlobalThreadId());
    asyncLoop(server->Base_);
  }, this);
  return true;
}

void PoolHttpServer::stop()
{
  postQuitOperation(Base_);
  Thread_.join();
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

void PoolHttpConnection::replyWithStatus(const char *status)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  stream.write('{');
  jsonSerializeString(stream, "status", status, true);
  stream.write("}\n");
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}
