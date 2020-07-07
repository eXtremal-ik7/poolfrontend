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

static int interrupted = 0;
static void sigIntHandler(int) { interrupted = 1; }

struct PoolConfig {
  bool IsMaster;
  std::filesystem::path DatabasePath;
  uint16_t HttpPort;

  std::unique_ptr<UserManager> UserMgr;
  std::vector<PoolBackend> Backends;
};

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
  PoolConfig poolConfig;

  try {
    cfg->parse(argv[1]);
    const char *frontendSection = "poolfrontend";

    poolConfig.IsMaster = cfg->lookupBoolean(frontendSection, "isMaster", true);
    poolConfig.DatabasePath = cfg->lookupString(frontendSection, "dbPath");
    poolConfig.HttpPort = cfg->lookupInt(frontendSection, "httpPort");

    // Initialize user manager
    poolConfig.UserMgr.reset(new UserManager(poolConfig.DatabasePath));

    // Initialize all backends
    config4cpp::StringVector coinsList;
    cfg->lookupList(frontendSection, "coins", coinsList);
    for (int i = 0, ie = coinsList.length(); i != ie; ++i) {
      std::string scopeName = std::string("coins.") + coinsList[i];
      const char *scope = scopeName.c_str();

      PoolBackendConfig backendConfig;
      backendConfig.CoinName = coinsList[i];

      // Inherited pool config parameters
      backendConfig.isMaster = poolConfig.IsMaster;
      backendConfig.dbPath = poolConfig.DatabasePath;

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
      poolConfig.Backends.emplace_back(std::move(backendConfig), *poolConfig.UserMgr);
    }

  } catch(const config4cpp::ConfigurationException& ex){
    LOG_F(ERROR, "%s", ex.c_str());
    exit(1);
  }

  initializeSocketSubsystem();
  asyncBase *base = createAsyncBase(amOSDefault);
    
  // Start backends for all coins
  for (auto &backend: poolConfig.Backends) {
    backend.start();
  }

  // Handle CTRL+C (SIGINT)
  signal(SIGINT, sigIntHandler);
  signal(SIGTERM, sigIntHandler);
  std::thread sigIntThread([&base, &poolConfig]() {
    while (!interrupted)
      std::this_thread::sleep_for(std::chrono::seconds(1));
    LOG_F(INFO, "Interrupted by user");
    for (auto &backend: poolConfig.Backends) {
      backend.stop();
    }
    postQuitOperation(base);
  });

  sigIntThread.detach();

  asyncLoop(base);
  LOG_F(INFO, "poolfrondend stopped\n");
  return 0;
}
