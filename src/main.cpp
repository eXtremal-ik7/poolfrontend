#include "http.h"
#include "poolcore/backend.h"

#include "config4cpp/Configuration.h"

#include "asyncio/asyncio.h"
#include "asyncio/socket.h"
#include "loguru.hpp"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <thread>

static std::atomic<unsigned> threadCounter = 0;
static __tls unsigned threadId;

static int interrupted = 0;
static void sigIntHandler(int) { interrupted = 1; }

struct PoolContext {
  bool IsMaster;
  std::filesystem::path DatabasePath;
  uint16_t HttpPort;

  std::unique_ptr<PoolHttpServer> HttpServer;
  std::unique_ptr<UserManager> UserMgr;
  std::vector<PoolBackend> Backends;
  std::unordered_map<std::string, size_t> CoinIdxMap;
};

void InitializeWorkerThread()
{
  threadId = threadCounter.fetch_add(1);
}

unsigned GetWorkerThreadId()
{
  return threadId;
}

int main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <configuration file>\n", argv[0]);
    return 1;
  }

  char logFileName[64];
  {
    auto t = time(nullptr);
    auto now = localtime(&t);
    snprintf(logFileName, sizeof(logFileName), "poolfrontend-%04u-%02u-%02u.log", now->tm_year + 1900, now->tm_mon + 1, now->tm_mday);
  }

  loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
  loguru::g_preamble_thread = true;
  loguru::g_preamble_file = false;
  loguru::g_flush_interval_ms = 100;
  loguru::init(argc, argv);
  loguru::add_file(logFileName, loguru::Append, loguru::Verbosity_INFO);
  loguru::g_stderr_verbosity = 1;
  loguru::set_thread_name("main");

  PoolBackendConfig backendConfig;
  config4cpp::Configuration *cfg = config4cpp::Configuration::create();
  PoolContext poolContext;
  unsigned workerThreadsNum = 0;

  try {
    cfg->parse(argv[1]);
    const char *frontendSection = "poolfrontend";

    poolContext.IsMaster = cfg->lookupBoolean(frontendSection, "isMaster", true);
    poolContext.DatabasePath = cfg->lookupString(frontendSection, "dbPath");
    poolContext.HttpPort = cfg->lookupInt(frontendSection, "httpPort");
    workerThreadsNum = cfg->lookupInt(frontendSection, "workerThreadsNum", 0);

    // Initialize user manager
    poolContext.UserMgr.reset(new UserManager(poolContext.DatabasePath));

    // Initialize all backends
    config4cpp::StringVector coinsList;
    cfg->lookupList(frontendSection, "coins", coinsList);
    for (int i = 0, ie = coinsList.length(); i != ie; ++i) {
      poolContext.CoinIdxMap[coinsList[i]] = i;

      std::string scopeName = std::string("coins.") + coinsList[i];
      const char *scope = scopeName.c_str();

      PoolBackendConfig backendConfig;
      backendConfig.CoinName = coinsList[i];

      // Inherited pool config parameters
      backendConfig.isMaster = poolContext.IsMaster;
      backendConfig.dbPath = poolContext.DatabasePath;

      // Backend parameters
      backendConfig.RequiredConfirmations = cfg->lookupInt(scope, "requiredConfirmations", 10);
      backendConfig.DefaultMinimalPayout = static_cast<int64_t>(cfg->lookupFloat(scope, "defaultMinimalPayout", 0.01) * COIN);
      backendConfig.MinimalPayout = static_cast<int64_t>(cfg->lookupFloat(scope, "minimalPayout", 0.001) * COIN);
      backendConfig.KeepRoundTime = cfg->lookupInt(scope, "keepRoundTime", 3) * 24*3600;
      backendConfig.KeepStatsTime = cfg->lookupInt(scope, "keepStatsTime", 2) * 60;
      backendConfig.ConfirmationsCheckInterval = cfg->lookupInt(scope, "confirmationsCheckInterval", 10) * 60 * 1000000;
      backendConfig.PayoutInterval = cfg->lookupInt(scope, "payoutInterval", 60) * 60 * 1000000;
      backendConfig.BalanceCheckInterval = cfg->lookupInt(scope, "balanceCheckInterval", 3) * 60 * 1000000;
      backendConfig.StatisticCheckInterval = cfg->lookupInt(scope, "statisticCheckInterval", 1) * 60 * 1000000;
      // ZEC specific
      backendConfig.poolZAddr = cfg->lookupString(scope, "pool_zaddr", "");
      backendConfig.poolTAddr = cfg->lookupString(scope, "pool_taddr", "");

      // Initialize backend
      poolContext.Backends.emplace_back(std::move(backendConfig), *poolContext.UserMgr);
    }

  } catch(const config4cpp::ConfigurationException& ex){
    LOG_F(ERROR, "%s", ex.c_str());
    exit(1);
  }

  initializeSocketSubsystem();
  asyncBase *base = createAsyncBase(amOSDefault);
    
  // Start user manager
  poolContext.UserMgr->start();

  // Start backends for all coins
  for (auto &backend: poolContext.Backends) {
    backend.start();
  }

  poolContext.HttpServer.reset(new PoolHttpServer(base, poolContext.HttpPort, *poolContext.UserMgr.get(), poolContext.Backends, poolContext.CoinIdxMap));
  poolContext.HttpServer->start();

  if (workerThreadsNum == 0)
    workerThreadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;

  std::unique_ptr<std::thread[]> workerThreads(new std::thread[workerThreadsNum]);
  for (unsigned i = 0; i < workerThreadsNum; i++) {
    workerThreads[i] = std::thread([](asyncBase *base, unsigned) {
      char threadName[16];
      InitializeWorkerThread();
      snprintf(threadName, sizeof(threadName), "worker%u", GetWorkerThreadId());
      loguru::set_thread_name(threadName);
      asyncLoop(base);
    }, base, i);
  }

  // Handle CTRL+C (SIGINT)
  signal(SIGINT, sigIntHandler);
  signal(SIGTERM, sigIntHandler);
  std::thread sigIntThread([&base, &poolContext]() {
    while (!interrupted)
      std::this_thread::sleep_for(std::chrono::seconds(1));
    LOG_F(INFO, "Interrupted by user");
    for (auto &backend: poolContext.Backends)
      backend.stop();
    poolContext.UserMgr->stop();
    postQuitOperation(base);
  });

  sigIntThread.detach();

  for (unsigned i = 0; i < workerThreadsNum; i++)
    workerThreads[i].join();

  LOG_F(INFO, "poolfrondend stopped\n");
  return 0;
}
