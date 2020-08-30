#include "config.h"
#include "http.h"
#include "poolcore/backend.h"
#include "poolcore/bitcoinRPCClient.h"
#include "poolcore/coinLibrary.h"
#include "poolcore/clientDispatcher.h"
#include "poolcore/thread.h"
#include "poolcommon/utils.h"
#include "poolinstances/fabric.h"

#include "asyncio/asyncio.h"
#include "asyncio/socket.h"
#include "loguru.hpp"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <thread>
#if !defined(OS_WINDOWS)
#include <netdb.h>
#endif

static int interrupted = 0;
static void sigIntHandler(int) { interrupted = 1; }

struct PoolContext {
  bool IsMaster;
  std::filesystem::path DatabasePath;
  uint16_t HttpPort;

  std::vector<CCoinInfo> CoinList;
  std::vector<std::unique_ptr<CNetworkClientDispatcher>> ClientsDispatcher;
  std::vector<std::unique_ptr<PoolBackend>> Backends;
  std::unordered_map<std::string, size_t> CoinIdxMap;

  std::unique_ptr<CThreadPool> ThreadPool;
  std::vector<std::unique_ptr<CPoolInstance>> Instances;

  std::unique_ptr<UserManager> UserMgr;
  std::unique_ptr<PoolHttpServer> HttpServer;
};

int main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <configuration file>\n", argv[0]);
    return 1;
  }

  loguru::g_preamble_thread = true;
  loguru::g_preamble_file = true;
  loguru::g_flush_interval_ms = 100;
  loguru::g_stderr_verbosity = loguru::Verbosity_1;
  loguru::init(argc, argv);
  loguru::set_thread_name("main");

  PoolBackendConfig backendConfig;
  PoolContext poolContext;
  unsigned totalThreadsNum = 0;
  unsigned workerThreadsNum = 0;

  initializeSocketSubsystem();
  asyncBase *base = createAsyncBase(amOSDefault);

  // Parse config
  FileDescriptor configFd;
  if (!configFd.open(argv[1])) {
    LOG_F(ERROR, "Can't open config file %s", argv[1]);
    return 1;
  }

  std::string configData;
  configData.resize(configFd.size());
  configFd.read(configData.data(), 0, configData.size());
  configFd.close();
  rapidjson::Document document;
  document.Parse<rapidjson::kParseCommentsFlag>(configData.c_str());
  if (document.HasParseError()) {
    LOG_F(ERROR, "Config file %s is not valid JSON", argv[1]);
    return 1;
  }

  CPoolFrontendConfig config;
  {
    std::string error;
    if (!config.load(document, error)) {
      LOG_F(ERROR, "Config file %s contains error", argv[1]);
      LOG_F(ERROR, "%s", error.c_str());
      return 1;
    }
  }

  {
    poolContext.DatabasePath = config.DbPath;

    {
      char logFileName[64];
      auto t = time(nullptr);
      auto now = localtime(&t);
      snprintf(logFileName, sizeof(logFileName), "poolfrontend-%04u-%02u-%02u.log", now->tm_year + 1900, now->tm_mon + 1, now->tm_mday);
      std::filesystem::path logFilePath(poolContext.DatabasePath);
      loguru::add_file((poolContext.DatabasePath / logFileName).u8string().c_str(), loguru::Append, loguru::Verbosity_1);
    }

    // Analyze config
    poolContext.IsMaster = config.IsMaster;
    poolContext.HttpPort = config.HttpPort;
    workerThreadsNum = config.WorkerThreadsNum;
    if (workerThreadsNum == 0)
      workerThreadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() / 4 : 2;

    // Calculate total threads num
    unsigned backendsNum = static_cast<unsigned>(config.Coins.size());
    totalThreadsNum =
      1 +                   // Monitor (listeners and clients polling)
      workerThreadsNum +    // Share checkers
      backendsNum +         // Backends
      1;                    // HTTP server
    LOG_F(INFO, "Worker threads: %u; total pool threads: %u", workerThreadsNum, totalThreadsNum);

    // Initialize user manager
    poolContext.UserMgr.reset(new UserManager(poolContext.DatabasePath));

    // Base config
    poolContext.UserMgr->setBaseCfg(config.PoolName, config.PoolHostAddress, config.PoolActivateLinkPrefix);
    // Admin password
    if (!config.AdminPasswordHash.empty())
      poolContext.UserMgr->setAdminPassword(config.AdminPasswordHash);

    // SMTP config
    if (config.SmtpEnabled) {
      // Build HostAddress for server
      HostAddress smtpAddress;
      char *colonPos = (char*)strchr(config.SmtpServer.c_str(), ':');
      if (colonPos == nullptr) {
        LOG_F(ERROR, "Invalid server %s\nIt must have address:port format", config.SmtpServer.c_str());
        return 1;
      }

      *colonPos = 0;
      hostent *host = gethostbyname(config.SmtpServer.c_str());
      if (!host) {
        LOG_F(ERROR, "Cannot retrieve address of %s (gethostbyname failed)", config.SmtpServer.c_str());
      }

      u_long addr = host->h_addr ? *reinterpret_cast<u_long*>(host->h_addr) : 0;
      if (!addr) {
        LOG_F(ERROR, "Cannot retrieve address of %s (gethostbyname returns 0)", config.SmtpServer.c_str());
        return 1;
      }

      smtpAddress.family = AF_INET;
      smtpAddress.ipv4 = static_cast<uint32_t>(addr);
      smtpAddress.port = htons(atoi(colonPos + 1));

      // Enable SMTP
      poolContext.UserMgr->enableSMTP(smtpAddress, config.SmtpLogin, config.SmtpPassword, config.SmtpSenderAddress, config.SmtpUseSmtps, config.SmtpUseStartTls);
    }

    // Initialize all backends
    for (size_t coinIdx = 0, coinIdxE = config.Coins.size(); coinIdx != coinIdxE; ++coinIdx) {
      PoolBackendConfig backendConfig;
      const CCoinConfig &coinConfig = config.Coins[coinIdx];
      const char *coinName = coinConfig.Name.c_str();
      CCoinInfo coinInfo = CCoinLibrary::get(coinName);
      if (coinInfo.Name.empty()) {
        LOG_F(ERROR, "Unknown coin: %s", coinName);
        return 1;
      }

      // Inherited pool config parameters
      backendConfig.isMaster = poolContext.IsMaster;
      backendConfig.dbPath = poolContext.DatabasePath / coinInfo.Name;

      // Backend parameters
      if (!parseMoneyValue(coinConfig.DefaultPayoutThreshold.c_str(), coinInfo.RationalPartSize, &backendConfig.DefaultPayoutThreshold)) {
        LOG_F(ERROR, "Can't load 'defaultPayoutThreshold' from %s coin config", coinName);
        return 1;
      }

      if (!parseMoneyValue(coinConfig.MinimalAllowedPayout.c_str(), coinInfo.RationalPartSize, &backendConfig.MinimalAllowedPayout)) {
        LOG_F(ERROR, "Can't load 'minimalPayout' from %s coin config", coinName);
        return 1;
      }

      backendConfig.RequiredConfirmations = coinConfig.RequiredConfirmations;
      backendConfig.KeepRoundTime = coinConfig.KeepRoundTime * 24*3600;
      backendConfig.KeepStatsTime = coinConfig.KeepStatsTime * 60;
      backendConfig.ConfirmationsCheckInterval = coinConfig.ConfirmationsCheckInterval * 60 * 1000000;
      backendConfig.PayoutInterval = coinConfig.PayoutInterval * 60 * 1000000;
      backendConfig.BalanceCheckInterval = coinConfig.BalanceCheckInterval * 60 * 1000000;

      for (const auto &addr: coinConfig.MiningAddresses) {
        if (!coinInfo.checkAddress(addr.Address, coinInfo.PayoutAddressType)) {
          LOG_F(ERROR, "Invalid mining address: %s", addr.Address.c_str());
          return 1;
        }

        backendConfig.MiningAddresses.add(addr.Address, addr.Weight);
      }

      backendConfig.CoinBaseMsg = coinConfig.CoinbaseMsg;
      if (backendConfig.CoinBaseMsg.empty())
        backendConfig.CoinBaseMsg = config.PoolName;

      backendConfig.PoolFee.resize(coinConfig.Fees.size());
      for (size_t feeIdx = 0, feeIdxE = coinConfig.Fees.size(); feeIdx != feeIdxE; ++feeIdx) {
        PoolFeeEntry &entry = backendConfig.PoolFee[feeIdx];
        entry.User = coinConfig.Fees[feeIdx].Address;
        entry.Percentage = coinConfig.Fees[feeIdx].Percentage;

        UserManager::Credentials credentials;
        if (!poolContext.UserMgr->getUserCredentials(entry.User, credentials)) {
          LOG_F(ERROR, "User %s not found", entry.User.c_str());
          LOG_F(ERROR, "You need create user before adding to pool fee receivers");
          return 1;
        }

        if (entry.Percentage > 100.0f) {
          LOG_F(ERROR, "Invalid pool fee: %.3f", entry.Percentage);
          return 1;
        }
      }

      // ZEC specific
      // backendConfig.poolZAddr;
      // backendConfig.poolTAddr;

      // Nodes
      std::unique_ptr<CNetworkClientDispatcher> dispatcher(new CNetworkClientDispatcher(base, coinInfo, totalThreadsNum));
      for (size_t nodeIdx = 0, nodeIdxE = coinConfig.Nodes.size(); nodeIdx != nodeIdxE; ++nodeIdx) {
        CNetworkClient *client;
        const CNodeConfig &node = coinConfig.Nodes[nodeIdx];
        if (node.Type == "bitcoinrpc") {
          client = new CBitcoinRpcClient(base, totalThreadsNum, coinInfo, node.Address.c_str(), node.Login.c_str(), node.Password.c_str(), node.LongPollEnabled);
        } else {
          LOG_F(ERROR, "Unknown node type: %s", node.Type.c_str());
          return 1;
        }

        dispatcher->addClient(client);
      }

      // Initialize backend
      PoolBackend *backend = new PoolBackend(std::move(backendConfig), coinInfo, *poolContext.UserMgr, *dispatcher);
      poolContext.Backends.emplace_back(backend);
      poolContext.ClientsDispatcher.emplace_back(dispatcher.release());
      poolContext.UserMgr->configAddCoin(coinInfo, backendConfig.DefaultPayoutThreshold);
      poolContext.CoinList.push_back(coinInfo);
      poolContext.CoinIdxMap[coinName] = coinIdx;
    }

    // Initialize workers
    poolContext.ThreadPool.reset(new CThreadPool(workerThreadsNum));

    // Initialize instances
    poolContext.Instances.resize(config.Instances.size());
    for (size_t instIdx = 0, instIdxE = config.Instances.size(); instIdx != instIdxE; ++instIdx) {
      CInstanceConfig &instanceConfig = config.Instances[instIdx];
      CPoolInstance *instance = PoolInstanceFabric::get(base, *poolContext.UserMgr, *poolContext.ThreadPool, instanceConfig.Type, instanceConfig.Protocol, instanceConfig.InstanceConfig);
      if (!instance) {
        LOG_F(ERROR, "Can't create instance with type '%s' and prorotol '%s'", instanceConfig.Type.c_str(), instanceConfig.Protocol.c_str());
        return 1;
      }

      for (const auto &linkedCoinName: instanceConfig.Backends) {
        auto It = poolContext.CoinIdxMap.find(linkedCoinName);
        if (It == poolContext.CoinIdxMap.end()) {
          LOG_F(ERROR, "Instance %s linked with non-existent coin %s", instanceConfig.Name.c_str(), linkedCoinName.c_str());
          return 1;
        }

        poolContext.ClientsDispatcher[It->second]->connectWith(instance);
      }
    }
  }
    
  // Start workers
  poolContext.ThreadPool->start();

  // Start user manager
  poolContext.UserMgr->start();

  // Start backends for all coins
  for (auto &backend: poolContext.Backends) {
    backend->start();
  }

  // Start clients polling
  for (auto &dispatcher: poolContext.ClientsDispatcher) {
    dispatcher->poll();
  }

  poolContext.HttpServer.reset(new PoolHttpServer(poolContext.HttpPort, *poolContext.UserMgr.get(), poolContext.Backends, poolContext.CoinIdxMap));
  poolContext.HttpServer->start();

  // Start monitor thread
  std::thread monitorThread([](asyncBase *base) {
    InitializeWorkerThread();
    loguru::set_thread_name("monitor");
    LOG_F(INFO, "monitor started tid=%u", GetGlobalThreadId());
    asyncLoop(base);
  }, base);

  // Handle CTRL+C (SIGINT)
  signal(SIGINT, sigIntHandler);
  signal(SIGTERM, sigIntHandler);

  std::thread sigIntThread([&base, &poolContext]() {
    loguru::set_thread_name("sigint_monitor");
    while (!interrupted)
      std::this_thread::sleep_for(std::chrono::seconds(1));
    LOG_F(INFO, "Interrupted by user");
    // Stop HTTP server
    poolContext.HttpServer->stop();
    // Stop workers
    poolContext.ThreadPool->stop();
    // Stop backends
    for (auto &backend: poolContext.Backends)
      backend->stop();
    // Stop user manager
    poolContext.UserMgr->stop();

    // Stop monitor thread
    postQuitOperation(base);
  });

  sigIntThread.detach();
  monitorThread.join();
  LOG_F(INFO, "poolfrondend stopped\n");
  return 0;
}
