#pragma once

#include <string>
#include <vector>
#include "rapidjson/document.h"

enum EErrorType {
  EOk = 0,
  ENotExists,
  ETypeMismatch
};

struct CInstanceConfig {
  std::string Name;
  std::string Type;
  std::string Protocol;
  std::vector<std::string> Backends;
  rapidjson::Value InstanceConfig;

  void load(rapidjson::Document &document, const rapidjson::Value &value, std::string &errorPlace, EErrorType *error);
};

struct CNodeConfig {
  std::string Type;
  std::string Address;
  std::string Login;
  std::string Password;
  bool LongPollEnabled;

  void load(const rapidjson::Value &value, const std::string &path, std::string &errorDescription, EErrorType *error);
};

struct CFeeConfig {
  std::string Address;
  float Percentage;

  void load(const rapidjson::Value &value, const std::string &path, std::string &errorDescription, EErrorType *error);
};

struct CMiningAddressConfig {
  std::string Address;
  uint32_t Weight;
};

struct CCoinConfig {
  std::string Name;
  std::vector<CNodeConfig> Nodes;
  std::vector<CFeeConfig> Fees;
  unsigned RequiredConfirmations;
  std::string DefaultPayoutThreshold;
  std::string MinimalAllowedPayout;
  unsigned KeepRoundTime;
  unsigned KeepStatsTime;
  unsigned ConfirmationsCheckInterval;
  unsigned PayoutInterval;
  unsigned BalanceCheckInterval;
  unsigned StatisticCheckInterval;
  unsigned ShareTarget;
  unsigned StratumWorkLifeTime;
  std::vector<CMiningAddressConfig> MiningAddresses;
  std::string CoinbaseMsg;

  void load(const rapidjson::Value &value, std::string &errorDescription, EErrorType *error);
};

struct CPoolFrontendConfig {
  bool IsMaster;
  unsigned HttpPort;
  unsigned WorkerThreadsNum;
  std::string AdminPasswordHash;
  std::string DbPath;
  std::string PoolName;
  std::string PoolHostAddress;
  std::string PoolActivateLinkPrefix;

  bool SmtpEnabled;
  std::string SmtpServer;
  std::string SmtpLogin;
  std::string SmtpPassword;
  std::string SmtpSenderAddress;
  bool SmtpUseSmtps;
  bool SmtpUseStartTls;

  std::vector<CCoinConfig> Coins;
  std::vector<CInstanceConfig> Instances;

  bool load(rapidjson::Document &document, std::string &errorDescription);
};
