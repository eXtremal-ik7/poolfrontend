#include "http.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include "loguru.hpp"

std::unordered_map<std::string, std::pair<int, PoolHttpConnection::FunctionTy>> PoolHttpConnection::FunctionNameMap_ = {
  {"userCreate", {hmGet, fnUserCreate}}
};

static inline bool rawcmp(Raw data, const char *operand) {
  size_t opSize = strlen(operand);
  return data.size == opSize && memcmp(data.data, operand, opSize) == 0;
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
    Context.argumentsNum = 0;
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
  }
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
