#include "migratecommon.h"
#include "struct2.h"
#include "poolcore/backendData.h"
#include "poolcore/coinLibrary.h"
#include "poolcore/poolCore.h"
#include "poolcore/accounting.h"
#include "poolcore/shareLog.h"
#include "poolcore/statistics.h"
#include "poolcommon/utils.h"
#include "../plugin.h"
#include "loguru.hpp"
#include <unordered_map>

extern CPluginContext gPluginContext;

// Unconditional correction for rounding errors after conversion.
// Old data integrity is verified separately (strict checks on raw old values).
// Here we only distribute the rounding remainder evenly across all elements.
template<typename ValueType, typename Container, typename FieldAccessor>
static bool correctSum(Container &items, const ValueType &expected, uint64_t height, const char *fieldName, FieldAccessor accessor)
{
  ValueType sum = ValueType::zero();
  for (auto &item : items)
    sum += accessor(item);

  if (sum == expected)
    return true;

  bool needSubtract = sum > expected;
  ValueType diff = needSubtract ? sum - expected : expected - sum;

  ValueType div;
  uint64_t mod = diff.divmod64(items.size(), &div);
  uint64_t i = 0;
  for (auto &item : items) {
    if (needSubtract) {
      accessor(item) -= div;
      if (i < mod) accessor(item) -= 1u;
    } else {
      accessor(item) += div;
      if (i < mod) accessor(item) += 1u;
    }
    i++;
  }

  // Re-verify
  sum = ValueType::zero();
  for (auto &item : items)
    sum += accessor(item);
  if (sum != expected) {
    LOG_F(ERROR, "Round height=%llu: %s correction failed: sum=%s expected=%s",
          static_cast<unsigned long long>(height), fieldName, sum.getDecimal().c_str(), expected.getDecimal().c_str());
    return false;
  }

  LOG_F(WARNING, "Round height=%llu: adjusted %s diff=%s across %zu items",
        static_cast<unsigned long long>(height), fieldName, diff.getDecimal().c_str(), items.size());
  return true;
}

static bool isNonCoinDirectory(const std::string &name)
{
  static const char *skip[] = {"useractions", "userfeeplan", "users", "usersessions", "usersettings"};
  for (const char *prefix : skip) {
    if (name.starts_with(prefix))
      return true;
  }
  return false;
}

static bool loadCoinMap(const std::filesystem::path &dbPath,
                        std::unordered_map<std::string, CCoinInfo> &coinMap,
                        std::unordered_map<std::string, CCoinInfoOld2> &old2Map)
{
  for (std::filesystem::directory_iterator I(dbPath), IE; I != IE; ++I) {
    std::string name = I->path().filename().generic_string();

    if (!is_directory(I->status())) {
      if (name.starts_with("poolfrontend") && name.ends_with(".log"))
        continue;
      if (name == "migrate.log")
        continue;
      LOG_F(WARNING, "Unexpected file in database directory: %s", name.c_str());
      continue;
    }

    if (name.starts_with("__"))
      continue;
    if (isNonCoinDirectory(name))
      continue;

    CCoinInfo info = CCoinLibrary::get(name.c_str());
    if (info.Name.empty()) {
      for (const auto &proc : gPluginContext.AddExtraCoinProcs) {
        if (proc(name.c_str(), info))
          break;
      }
    }

    if (info.Name.empty()) {
      LOG_F(WARNING, "Unknown directory in database path: %s", name.c_str());
      continue;
    }

    if (coinMap.count(info.Name))
      continue;

    CCoinInfoOld2 old2 = CCoinLibraryOld2::get(name.c_str());
    for (const auto &proc : gPluginContext.AddExtraCoinOld2Procs) {
      if (proc(name.c_str(), old2))
        break;
    }

    if (info.IsAlgorithm)
      LOG_F(INFO, "Found algorithm: %s", info.Name.c_str());
    else
      LOG_F(INFO, "Found coin: %s", info.Name.c_str());

    old2Map.emplace(info.Name, old2);
    coinMap.emplace(info.Name, std::move(info));
  }

  return true;
}

static bool migrateBalance(const std::filesystem::path &coinPath, const CCoinInfo &coinInfo, const CCoinInfoOld2 &old2)
{
  return migrateDatabase(coinPath, "balance", "balance.2", [&coinInfo, &old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    UserBalanceRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize balance record, database corrupted");
      return false;
    }

    UserBalanceRecord newRecord;
    newRecord.Login = oldRecord.Login;
    newRecord.Balance = fromRational(static_cast<uint64_t>(oldRecord.BalanceWithFractional));
    newRecord.Balance /= static_cast<uint64_t>(old2.ExtraMultiplier);
    newRecord.Requested = fromRational(static_cast<uint64_t>(oldRecord.Requested));
    newRecord.Paid = fromRational(static_cast<uint64_t>(oldRecord.Paid));

    LOG_F(INFO, "  %s balance=%s requested=%s paid=%s",
          oldRecord.Login.c_str(),
          FormatMoney(newRecord.Balance, coinInfo.FractionalPartSize).c_str(),
          FormatMoney(newRecord.Requested, coinInfo.FractionalPartSize).c_str(),
          FormatMoney(newRecord.Paid, coinInfo.FractionalPartSize).c_str());

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  });
}

static bool migrateStats(const std::filesystem::path &coinPath, const CCoinInfoOld2 &old2, const char *baseName, const char *newName, unsigned threads, const std::string &cutoff)
{
  return migrateDatabaseMt(coinPath, baseName, newName, [&old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    StatsRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize stats record, database corrupted");
      return false;
    }

    StatsRecord newRecord;
    newRecord.Login = oldRecord.Login;
    newRecord.WorkerId = oldRecord.WorkerId;
    newRecord.Time = oldRecord.Time;
    newRecord.ShareCount = oldRecord.ShareCount;
    newRecord.ShareWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.ShareWork.mulfp(oldRecord.ShareWork);
    newRecord.PrimePOWTarget = oldRecord.PrimePOWTarget;
    newRecord.PrimePOWShareCount = oldRecord.PrimePOWShareCount;

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads, [&cutoff](const std::string &partition) {
    return partition >= cutoff;
  });
}

static bool migratePPLNSPayouts(const std::filesystem::path &coinPath, const CCoinInfoOld2 &old2, unsigned threads)
{
  return migrateDatabaseMt(coinPath, "pplns.payouts", "pplns.payouts.2", [&old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    CPPLNSPayout2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize pplns.payouts record, database corrupted");
      return false;
    }

    CPPLNSPayout newRecord;
    newRecord.Login = oldRecord.Login;
    newRecord.RoundStartTime = oldRecord.RoundStartTime;
    newRecord.BlockHash = oldRecord.BlockHash;
    newRecord.BlockHeight = oldRecord.BlockHeight;
    newRecord.RoundEndTime = oldRecord.RoundEndTime;
    newRecord.PayoutValue = fromRational(static_cast<uint64_t>(oldRecord.PayoutValue));
    newRecord.PayoutValueWithoutFee = fromRational(static_cast<uint64_t>(oldRecord.PayoutValueWithoutFee));
    newRecord.AcceptedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.AcceptedWork.mulfp(oldRecord.AcceptedWork);
    newRecord.PrimePOWTarget = oldRecord.PrimePOWTarget;
    newRecord.RateToBTC = oldRecord.RateToBTC;
    newRecord.RateBTCToUSD = oldRecord.RateBTCToUSD;

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads);
}

static bool migratePayoutsRaw(const std::filesystem::path &coinPath, const CCoinInfo &coinInfo)
{
  return migrateFile(coinPath, "payouts.raw", "payoutQueue.raw", [&coinInfo](xmstream &input, xmstream &output) -> bool {
    PayoutDbRecord2 oldRecord;
    if (!oldRecord.deserializeValue(input)) {
      LOG_F(ERROR, "Can't deserialize payouts.raw record, file corrupted");
      return false;
    }

    PayoutDbRecord newRecord;
    newRecord.UserId = oldRecord.UserId;
    newRecord.Time = oldRecord.Time;
    newRecord.Value = fromRational(static_cast<uint64_t>(oldRecord.Value));
    newRecord.TransactionId = oldRecord.TransactionId;
    newRecord.TransactionData = oldRecord.TransactionData;
    newRecord.Status = oldRecord.Status;
    newRecord.TxFee = fromRational(static_cast<uint64_t>(oldRecord.TxFee));

    LOG_F(INFO, "  %s value=%s txId=%s status=%u",
          newRecord.UserId.c_str(),
          FormatMoney(newRecord.Value, coinInfo.FractionalPartSize).c_str(),
          newRecord.TransactionId.c_str(), newRecord.Status);

    newRecord.serializeValue(output);
    return true;
  });
}

static bool migratePayouts(const std::filesystem::path &coinPath, unsigned threads)
{
  return migrateDatabaseMt(coinPath, "payouts", "payouts.2", [](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    PayoutDbRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize payouts record, database corrupted");
      return false;
    }

    PayoutDbRecord newRecord;
    newRecord.UserId = oldRecord.UserId;
    newRecord.Time = oldRecord.Time;
    newRecord.Value = fromRational(static_cast<uint64_t>(oldRecord.Value));
    newRecord.TransactionId = oldRecord.TransactionId;
    newRecord.TransactionData = oldRecord.TransactionData;
    newRecord.Status = oldRecord.Status;
    newRecord.TxFee = fromRational(static_cast<uint64_t>(oldRecord.TxFee));

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads);
}

static bool migratePoolBalance(const std::filesystem::path &coinPath, const CCoinInfoOld2 &old2, unsigned threads)
{
  return migrateDatabaseMt(coinPath, "poolBalance", "poolBalance.2", [&old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    PoolBalanceRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize poolBalance record, database corrupted");
      return false;
    }

    PoolBalanceRecord newRecord;
    newRecord.Time = oldRecord.Time;
    newRecord.Balance = fromRational(static_cast<uint64_t>(oldRecord.BalanceWithFractional));
    newRecord.Balance /= static_cast<uint64_t>(old2.ExtraMultiplier);
    newRecord.Immature = fromRational(static_cast<uint64_t>(oldRecord.Immature));
    newRecord.Users = fromRational(static_cast<uint64_t>(oldRecord.Users));
    newRecord.Queued = fromRational(static_cast<uint64_t>(oldRecord.Queued));
    newRecord.ConfirmationWait = fromRational(static_cast<uint64_t>(oldRecord.ConfirmationWait));
    newRecord.Net = fromRational(static_cast<uint64_t>(oldRecord.Net));

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads);
}

static bool migrateFoundBlocks(const std::filesystem::path &coinPath, const CCoinInfo &coinInfo, const CCoinInfoOld2 &old2, unsigned threads)
{
  std::atomic<unsigned> debugCount{0};
  return migrateDatabaseMt(coinPath, "foundBlocks", "foundBlocks.2", [&coinInfo, &old2, &debugCount](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    FoundBlockRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize foundBlocks record, database corrupted");
      return false;
    }

    FoundBlockRecord newRecord;
    newRecord.Height = oldRecord.Height;
    newRecord.Hash = oldRecord.Hash;
    newRecord.Time = oldRecord.Time;
    newRecord.AvailableCoins = fromRational(static_cast<uint64_t>(oldRecord.AvailableCoins));
    newRecord.FoundBy = oldRecord.FoundBy;
    newRecord.ExpectedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.ExpectedWork.mulfp(oldRecord.ExpectedWork);
    newRecord.AccumulatedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.AccumulatedWork.mulfp(oldRecord.AccumulatedWork);
    newRecord.PublicHash = oldRecord.PublicHash;

    unsigned n = debugCount.fetch_add(1);
    if (n < 10) {
      LOG_F(INFO, "  height=%llu availableCoins=%s foundBy=%s",
            static_cast<unsigned long long>(newRecord.Height),
            FormatMoney(newRecord.AvailableCoins, coinInfo.FractionalPartSize).c_str(),
            newRecord.FoundBy.c_str());
    } else if (n == 10) {
      LOG_F(INFO, "  ... and more");
    }

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  }, threads);
}

static bool migrateRounds(const std::filesystem::path &coinPath, const CCoinInfo &coinInfo, const CCoinInfoOld2 &old2)
{
  return migrateDatabase(coinPath, "rounds.v2", "rounds.3", [&coinInfo, &old2](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    MiningRound2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize rounds record, database corrupted");
      return false;
    }

    MiningRound newRecord;
    newRecord.Height = oldRecord.Height;
    newRecord.BlockHash = oldRecord.BlockHash;
    newRecord.EndTime = oldRecord.EndTime;
    newRecord.StartTime = oldRecord.StartTime;
    newRecord.TotalShareValue = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.TotalShareValue.mulfp(oldRecord.TotalShareValue);
    newRecord.AvailableCoins = fromRational(static_cast<uint64_t>(oldRecord.AvailableCoins));
    newRecord.AvailableCoins /= static_cast<uint64_t>(old2.ExtraMultiplier);
    newRecord.FoundBy = oldRecord.FoundBy;
    newRecord.ExpectedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.ExpectedWork.mulfp(oldRecord.ExpectedWork);
    newRecord.AccumulatedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
    newRecord.AccumulatedWork.mulfp(oldRecord.AccumulatedWork);
    newRecord.TxFee = fromRational(static_cast<uint64_t>(oldRecord.TxFee));
    newRecord.PrimePOWTarget = oldRecord.PrimePOWTarget;

    for (const auto &s : oldRecord.UserShares) {
      UInt<256> shareValue = UInt<256>::fromDouble(old2.WorkMultiplier);
      shareValue.mulfp(s.ShareValue);
      UInt<256> incomingWork = UInt<256>::fromDouble(old2.WorkMultiplier);
      incomingWork.mulfp(s.IncomingWork);
      newRecord.UserShares.emplace_back(s.UserId, shareValue, incomingWork);
    }

    for (const auto &p : oldRecord.Payouts) {
      UInt<384> value = fromRational(static_cast<uint64_t>(p.Value));
      value /= static_cast<uint64_t>(old2.ExtraMultiplier);
      UInt<384> valueWithoutFee = fromRational(static_cast<uint64_t>(p.ValueWithoutFee));
      valueWithoutFee /= static_cast<uint64_t>(old2.ExtraMultiplier);
      UInt<256> acceptedWork = UInt<256>::fromDouble(old2.WorkMultiplier);
      acceptedWork.mulfp(p.AcceptedWork);
      newRecord.Payouts.emplace_back(p.UserId, value, valueWithoutFee, acceptedWork);
    }

    LOG_F(INFO, "  height=%llu hash=%s endTime=%lld startTime=%lld totalShareValue=%s availableCoins=%s",
          static_cast<unsigned long long>(newRecord.Height),
          newRecord.BlockHash.c_str(),
          static_cast<long long>(newRecord.EndTime),
          static_cast<long long>(newRecord.StartTime),
          formatSI(newRecord.TotalShareValue.getDecimal()).c_str(),
          FormatMoney(newRecord.AvailableCoins, coinInfo.FractionalPartSize).c_str());
    LOG_F(INFO, "    foundBy=%s expectedWork=%s accumulatedWork=%s txFee=%s primePOWTarget=%u shares=%zu payouts=%zu",
          newRecord.FoundBy.c_str(),
          formatSI(newRecord.ExpectedWork.getDecimal()).c_str(),
          formatSI(newRecord.AccumulatedWork.getDecimal()).c_str(),
          FormatMoney(newRecord.TxFee, coinInfo.FractionalPartSize).c_str(),
          newRecord.PrimePOWTarget,
          newRecord.UserShares.size(),
          newRecord.Payouts.size());

    for (const auto &s : newRecord.UserShares) {
      LOG_F(INFO, "    * share %s shareValue=%s incomingWork=%s",
            s.UserId.c_str(),
            formatSI(s.ShareValue.getDecimal()).c_str(),
            formatSI(s.IncomingWork.getDecimal()).c_str());
    }

    for (const auto &p : newRecord.Payouts) {
      LOG_F(INFO, "    * payout %s value=%s valueWithoutFee=%s acceptedWork=%s",
            p.UserId.c_str(),
            FormatMoney(p.Value, coinInfo.FractionalPartSize).c_str(),
            FormatMoney(p.ValueWithoutFee, coinInfo.FractionalPartSize).c_str(),
            formatSI(p.AcceptedWork.getDecimal()).c_str());
    }

    // Strict checks on old data (before conversion)
    bool hasPerUserIncomingWork = false;
    if (!oldRecord.UserShares.empty()) {
      double oldShareSum = 0;
      double oldWorkSum = 0;
      for (const auto &s : oldRecord.UserShares) {
        oldShareSum += s.ShareValue;
        oldWorkSum += s.IncomingWork;
      }
      if (oldShareSum != oldRecord.TotalShareValue) {
        LOG_F(ERROR, "Round height=%llu: old data: sum of ShareValue (%.*g) != TotalShareValue (%.*g)",
              static_cast<unsigned long long>(oldRecord.Height), 17, oldShareSum, 17, oldRecord.TotalShareValue);
        return false;
      }

      hasPerUserIncomingWork = oldWorkSum != 0;
      // v1 rounds don't have per-user IncomingWork (all zeros), skip check
      if (hasPerUserIncomingWork && oldWorkSum != oldRecord.AccumulatedWork) {
        LOG_F(ERROR, "Round height=%llu: old data: sum of IncomingWork (%.*g) != AccumulatedWork (%.*g)",
              static_cast<unsigned long long>(oldRecord.Height), 17, oldWorkSum, 17, oldRecord.AccumulatedWork);
        return false;
      }
    }

    if (!oldRecord.Payouts.empty()) {
      int64_t oldPayoutSum = 0;
      for (const auto &p : oldRecord.Payouts)
        oldPayoutSum += p.Value;
      if (oldPayoutSum != oldRecord.AvailableCoins) {
        LOG_F(ERROR, "Round height=%llu: old data: sum of payout Value (%lld) != AvailableCoins (%lld)",
              static_cast<unsigned long long>(oldRecord.Height),
              static_cast<long long>(oldPayoutSum), static_cast<long long>(oldRecord.AvailableCoins));
        return false;
      }
    }

    // Correct rounding errors after conversion
    if (!newRecord.UserShares.empty()) {
      if (!correctSum(newRecord.UserShares, newRecord.TotalShareValue, newRecord.Height, "ShareValue",
            [](auto &s) -> UInt<256>& { return s.ShareValue; }))
        return false;
      // v1 rounds don't have per-user IncomingWork (all zeros), skip correction
      if (hasPerUserIncomingWork) {
        if (!correctSum(newRecord.UserShares, newRecord.AccumulatedWork, newRecord.Height, "IncomingWork",
              [](auto &s) -> UInt<256>& { return s.IncomingWork; }))
          return false;
      }
    }

    if (!newRecord.Payouts.empty()) {
      if (!correctSum(newRecord.Payouts, newRecord.AvailableCoins, newRecord.Height, "PayoutValue",
            [](auto &p) -> UInt<384>& { return p.Value; }))
        return false;
    }

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  });
}

static void serializeAccountingFileData(xmstream &output, const AccountingDb::CAccountingFileData &fileData)
{
  DbIo<uint32_t>::serialize(output, AccountingDb::CAccountingFileData::CurrentRecordVersion);
  DbIo<decltype(fileData.LastShareId)>::serialize(output, fileData.LastShareId);
  DbIo<decltype(fileData.LastBlockTime)>::serialize(output, fileData.LastBlockTime);
  DbIo<decltype(fileData.Recent)>::serialize(output, fileData.Recent);
  DbIo<decltype(fileData.CurrentScores)>::serialize(output, fileData.CurrentScores);
}

// accounting.storage (version 1, manual format with double) -> accounting.storage.3
static bool migrateAccountingStorage(const std::filesystem::path &coinPath, const CCoinInfoOld2 &old2)
{
  return migrateDirectory(coinPath, "accounting.storage", "accounting.storage.3", [&old2](xmstream &input, xmstream &output) -> bool {
    // Version 1: manual parsing (no version header)
    AccountingDb::CAccountingFileData fileData;

    DbIo<decltype(fileData.LastShareId)>::unserialize(input, fileData.LastShareId);
    DbIo<decltype(fileData.LastBlockTime)>::unserialize(input, fileData.LastBlockTime);

    // Recent stats: vector<{UserId, vector<{sharesNum(unused), SharesWork(double), TimeLabel}>}>
    {
      uint64_t recentSize;
      DbIo<uint64_t>::unserialize(input, recentSize);
      fileData.Recent.resize(recentSize);
      for (uint64_t i = 0; i < recentSize; i++) {
        DbIo<std::string>::unserialize(input, fileData.Recent[i].UserId);
        uint64_t entrySize;
        DbIo<uint64_t>::unserialize(input, entrySize);
        fileData.Recent[i].Recent.resize(entrySize);
        for (uint64_t j = 0; j < entrySize; j++) {
          uint32_t sharesNum;
          DbIo<uint32_t>::unserialize(input, sharesNum);
          double sharesWork;
          DbIo<double>::unserialize(input, sharesWork);
          DbIo<int64_t>::unserialize(input, fileData.Recent[i].Recent[j].TimeLabel);
          fileData.Recent[i].Recent[j].SharesWork = UInt<256>::fromDouble(old2.WorkMultiplier);
          fileData.Recent[i].Recent[j].SharesWork.mulfp(sharesWork);
        }
      }
    }

    // CurrentScores: map<string, double>
    {
      uint64_t size;
      DbIo<uint64_t>::unserialize(input, size);
      for (uint64_t i = 0; i < size; i++) {
        std::string userId;
        double score;
        DbIo<std::string>::unserialize(input, userId);
        DbIo<double>::unserialize(input, score);
        auto &v = fileData.CurrentScores.emplace(userId, UInt<256>::fromDouble(old2.WorkMultiplier)).first->second;
        v.mulfp(score);
      }
    }

    if (input.eof())
      return false;

    LOG_F(INFO, "  lastShareId=%llu lastBlockTime=%lld recentUsers=%zu scores=%zu",
          static_cast<unsigned long long>(fileData.LastShareId),
          static_cast<long long>(fileData.LastBlockTime),
          fileData.Recent.size(), fileData.CurrentScores.size());
    for (const auto &user : fileData.Recent) {
      LOG_F(INFO, "    recent %s: %zu entries", user.UserId.c_str(), user.Recent.size());
      for (const auto &entry : user.Recent)
        LOG_F(INFO, "        time=%lld work=%s", static_cast<long long>(entry.TimeLabel), formatSI(entry.SharesWork.getDecimal()).c_str());
    }
    for (const auto &score : fileData.CurrentScores)
      LOG_F(INFO, "    score %s: %s", score.first.c_str(), formatSI(score.second.getDecimal()).c_str());

    serializeAccountingFileData(output, fileData);
    return true;
  }, true);
}

// accounting.storage.2 (version 2, CAccountingFileDataOld2 with double) -> accounting.storage.3
static bool migrateAccountingStorageV2(const std::filesystem::path &coinPath, const CCoinInfoOld2 &old2)
{
  return migrateDirectory(coinPath, "accounting.storage.2", "accounting.storage.3", [&old2](xmstream &input, xmstream &output) -> bool {
    CAccountingFileDataOld2 oldData;
    DbIo<CAccountingFileDataOld2>::unserialize(input, oldData);
    if (input.eof())
      return false;

    AccountingDb::CAccountingFileData fileData;
    fileData.LastShareId = oldData.LastShareId;
    fileData.LastBlockTime = oldData.LastBlockTime;

    // Convert Recent: CStatsExportDataOld2 -> CStatsExportData
    fileData.Recent.resize(oldData.Recent.size());
    for (size_t i = 0; i < oldData.Recent.size(); i++) {
      fileData.Recent[i].UserId = std::move(oldData.Recent[i].UserId);
      fileData.Recent[i].Recent.resize(oldData.Recent[i].Recent.size());
      for (size_t j = 0; j < oldData.Recent[i].Recent.size(); j++) {
        fileData.Recent[i].Recent[j].SharesWork = UInt<256>::fromDouble(old2.WorkMultiplier);
        fileData.Recent[i].Recent[j].SharesWork.mulfp(oldData.Recent[i].Recent[j].SharesWork);
        fileData.Recent[i].Recent[j].TimeLabel = oldData.Recent[i].Recent[j].TimeLabel;
      }
    }

    // Convert CurrentScores: double -> UInt<256>
    for (const auto &score : oldData.CurrentScores) {
      auto &v = fileData.CurrentScores.emplace(score.first, UInt<256>::fromDouble(old2.WorkMultiplier)).first->second;
      v.mulfp(score.second);
    }

    LOG_F(INFO, "  lastShareId=%llu lastBlockTime=%lld recentUsers=%zu scores=%zu",
          static_cast<unsigned long long>(fileData.LastShareId),
          static_cast<long long>(fileData.LastBlockTime),
          fileData.Recent.size(), fileData.CurrentScores.size());
    for (const auto &user : fileData.Recent) {
      LOG_F(INFO, "    recent %s: %zu entries", user.UserId.c_str(), user.Recent.size());
      for (const auto &entry : user.Recent)
        LOG_F(INFO, "        time=%lld work=%s", static_cast<long long>(entry.TimeLabel), formatSI(entry.SharesWork.getDecimal()).c_str());
    }
    for (const auto &score : fileData.CurrentScores)
      LOG_F(INFO, "    score %s: %s", score.first.c_str(), formatSI(score.second.getDecimal()).c_str());

    serializeAccountingFileData(output, fileData);
    return true;
  }, true);
}

// shares.log (version 1, old format with double work) -> shares.log.3
static bool migrateShareLog(const std::filesystem::path &coinPath, const CCoinInfoOld2 &old2, unsigned threads)
{
  return migrateDirectoryMt(coinPath, "shares.log", "shares.log.3", [&old2](xmstream &input, xmstream &output) -> bool {
    // Version 1: manual parsing (no version header)
    CShare share;
    share.UniqueShareId = input.readle<uint64_t>();
    DbIo<std::string>::unserialize(input, share.userId);
    DbIo<std::string>::unserialize(input, share.workerId);
    double workValue = input.read<double>();
    DbIo<int64_t>::unserialize(input, share.Time);
    if (input.eof())
      return false;

    share.WorkValue = UInt<256>::fromDouble(old2.WorkMultiplier);
    share.WorkValue.mulfp(workValue);

    ShareLogIo<CShare>::serialize(output, share);
    return true;
  }, threads, true);
}

// shares.log.v1 (version 2, CShareV1 with double work) -> shares.log.3
static bool migrateShareLogV1(const std::filesystem::path &coinPath, const CCoinInfoOld2 &old2, unsigned threads)
{
  return migrateDirectoryMt(coinPath, "shares.log.v1", "shares.log.3", [&old2](xmstream &input, xmstream &output) -> bool {
    CShareV1 shareV1;
    ShareLogIo<CShareV1>::unserialize(input, shareV1);
    if (input.eof())
      return false;

    CShare share;
    share.UniqueShareId = shareV1.UniqueShareId;
    share.userId = std::move(shareV1.userId);
    share.workerId = std::move(shareV1.workerId);
    share.WorkValue = UInt<256>::fromDouble(old2.WorkMultiplier);
    share.WorkValue.mulfp(shareV1.WorkValue);
    share.Time = shareV1.Time;
    share.ChainLength = shareV1.ChainLength;
    share.PrimePOWTarget = shareV1.PrimePOWTarget;

    ShareLogIo<CShare>::serialize(output, share);
    return true;
  }, threads, true);
}

static void serializeStatsFileData(xmstream &output, const StatisticDb::CStatsFileData &fileData)
{
  DbIo<uint32_t>::serialize(output, StatisticDb::CStatsFileData::CurrentRecordVersion);
  DbIo<decltype(fileData.LastShareId)>::serialize(output, fileData.LastShareId);
  for (const auto &record : fileData.Records)
    DbIo<StatisticDb::CStatsFileRecord>::serialize(output, record);
}

// stats cache v1 (manual format, no version header, no PrimePOW fields) -> v3
static bool migrateStatsCache(const std::filesystem::path &coinPath, const char *baseName, const char *newName, const CCoinInfoOld2 &old2, unsigned threads)
{
  return migrateDirectoryMt(coinPath, baseName, newName, [&old2](xmstream &input, xmstream &output) -> bool {
    StatisticDb::CStatsFileData fileData;
    DbIo<decltype(fileData.LastShareId)>::unserialize(input, fileData.LastShareId);
    while (input.remaining()) {
      StatisticDb::CStatsFileRecord &record = fileData.Records.emplace_back();
      uint32_t version;
      DbIo<decltype(version)>::unserialize(input, version);
      DbIo<decltype(record.Login)>::unserialize(input, record.Login);
      DbIo<decltype(record.WorkerId)>::unserialize(input, record.WorkerId);
      DbIo<decltype(record.Time)>::unserialize(input, record.Time);
      DbIo<decltype(record.ShareCount)>::unserialize(input, record.ShareCount);
      double shareWork;
      DbIo<double>::unserialize(input, shareWork);
      record.ShareWork = UInt<256>::fromDouble(old2.WorkMultiplier);
      record.ShareWork.mulfp(shareWork);
    }
    if (input.eof())
      return false;

    serializeStatsFileData(output, fileData);
    return true;
  }, threads, true);
}

// stats cache v2 (CStatsFileDataOld2 with double ShareWork) -> v3
static bool migrateStatsCacheV2(const std::filesystem::path &coinPath, const char *baseName, const char *newName, const CCoinInfoOld2 &old2, unsigned threads)
{
  return migrateDirectoryMt(coinPath, baseName, newName, [&old2](xmstream &input, xmstream &output) -> bool {
    CStatsFileDataOld2 oldData;
    DbIo<CStatsFileDataOld2>::unserialize(input, oldData);
    if (input.eof())
      return false;

    StatisticDb::CStatsFileData fileData;
    fileData.LastShareId = oldData.LastShareId;
    for (const auto &oldRecord : oldData.Records) {
      StatisticDb::CStatsFileRecord &record = fileData.Records.emplace_back();
      record.Login = oldRecord.Login;
      record.WorkerId = oldRecord.WorkerId;
      record.Time = oldRecord.Time;
      record.ShareCount = oldRecord.ShareCount;
      record.ShareWork = UInt<256>::fromDouble(old2.WorkMultiplier);
      record.ShareWork.mulfp(oldRecord.ShareWork);
      record.PrimePOWTarget = oldRecord.PrimePOWTarget;
      record.PrimePOWShareCount = oldRecord.PrimePOWShareCount;
    }

    serializeStatsFileData(output, fileData);
    return true;
  }, threads, true);
}

static bool migrateCoin(const std::filesystem::path &dbPath, const CCoinInfo &coinInfo, const CCoinInfoOld2 &old2, unsigned threads, const std::string &statisticCutoff)
{
  std::filesystem::path coinPath = dbPath / coinInfo.Name;
  if (!std::filesystem::exists(coinPath))
    return true;

  LOG_F(INFO, "Migrating %s: %s", coinInfo.IsAlgorithm ? "algorithm" : "coin", coinInfo.Name.c_str());

  if (!coinInfo.IsAlgorithm) {
    if (!migrateBalance(coinPath, coinInfo, old2))
      return false;
  }

  if (!migratePoolBalance(coinPath, old2, threads))
    return false;
  if (!migratePPLNSPayouts(coinPath, old2, threads))
    return false;
  if (!migratePayoutsRaw(coinPath, coinInfo))
    return false;
  if (!migratePayouts(coinPath, threads))
    return false;
  if (!migrateFoundBlocks(coinPath, coinInfo, old2, threads))
    return false;
  if (!migrateStats(coinPath, old2, "workerStats", "workerStats.2", threads, statisticCutoff))
    return false;
  if (!migrateStats(coinPath, old2, "poolstats", "poolstats.2", threads, statisticCutoff))
    return false;
  if (!migrateRounds(coinPath, coinInfo, old2))
    return false;
  if (!migrationTargetExists(coinPath, "shares.log.3")) {
    if (!migrateShareLog(coinPath, old2, threads))
      return false;
    if (!migrateShareLogV1(coinPath, old2, threads))
      return false;
  }
  if (!migrationTargetExists(coinPath, "accounting.storage.3")) {
    if (!migrateAccountingStorage(coinPath, old2))
      return false;
    if (!migrateAccountingStorageV2(coinPath, old2))
      return false;
  }
  if (!migrationTargetExists(coinPath, "stats.pool.cache.3")) {
    if (!migrateStatsCache(coinPath, "stats.pool.cache", "stats.pool.cache.3", old2, threads))
      return false;
    if (!migrateStatsCacheV2(coinPath, "stats.pool.cache.2", "stats.pool.cache.3", old2, threads))
      return false;
  }
  if (!migrationTargetExists(coinPath, "stats.workers.cache.3")) {
    if (!migrateStatsCache(coinPath, "stats.workers.cache", "stats.workers.cache.3", old2, threads))
      return false;
    if (!migrateStatsCacheV2(coinPath, "stats.workers.cache.2", "stats.workers.cache.3", old2, threads))
      return false;
  }

  return true;
}

static bool migrateUserSettings(const std::filesystem::path &dbPath, const std::unordered_map<std::string, CCoinInfo> &coinMap)
{
  return migrateDatabase(dbPath, "usersettings", "usersettings.2", [&coinMap](rocksdb::Iterator *it, rocksdb::WriteBatch &batch) -> bool {
    UserSettingsRecord2 oldRecord;
    if (!oldRecord.deserializeValue(it->value().data(), it->value().size())) {
      LOG_F(ERROR, "Can't deserialize user settings record, database corrupted");
      return false;
    }

    UserSettingsRecord newRecord;
    newRecord.Login = oldRecord.Login;
    newRecord.Coin = oldRecord.Coin;
    newRecord.Address = oldRecord.Address;
    newRecord.AutoPayout = oldRecord.AutoPayout;
    newRecord.MinimalPayout = fromRational(static_cast<uint64_t>(oldRecord.MinimalPayout));

    auto coinIt = coinMap.find(oldRecord.Coin);
    if (coinIt == coinMap.end()) {
      LOG_F(ERROR, "Unknown coin: %s", oldRecord.Coin.c_str());
      return false;
    }

    const CCoinInfo &coinInfo = coinIt->second;

    LOG_F(INFO, "  %s/%s address=%s minimalPayout=%s autoPayout=%u",
          oldRecord.Login.c_str(), oldRecord.Coin.c_str(), oldRecord.Address.c_str(),
          FormatMoney(newRecord.MinimalPayout, coinInfo.FractionalPartSize).c_str(), static_cast<unsigned>(oldRecord.AutoPayout));

    xmstream stream;
    newRecord.serializeValue(stream);
    batch.Put(it->key(), rocksdb::Slice(stream.data<const char>(), stream.sizeOf()));
    return true;
  });
}

bool migrateV3(const std::filesystem::path &dbPath, unsigned threads, const std::string &statisticCutoff)
{
  std::unordered_map<std::string, CCoinInfo> coinMap;
  std::unordered_map<std::string, CCoinInfoOld2> old2Map;
  if (!loadCoinMap(dbPath, coinMap, old2Map))
    return false;

  if (!migrateUserSettings(dbPath, coinMap))
    return false;

  for (const auto &[name, coinInfo] : coinMap) {
    if (!migrateCoin(dbPath, coinInfo, old2Map[name], threads, statisticCutoff))
      return false;
  }

  return true;
}
