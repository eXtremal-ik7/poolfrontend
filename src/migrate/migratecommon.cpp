#include "migratecommon.h"
#include "poolcommon/path.h"
#include "loguru.hpp"
#include <algorithm>
#include <cstdio>
#include <future>
#include <memory>
#include <vector>

static bool migratePartition(const std::filesystem::path &oldPartPath, const std::filesystem::path &newPartPath,
                              const std::string &partitionName, const MigrateCallback &callback)
{
  rocksdb::Options oldOptions;
  oldOptions.create_if_missing = false;
  rocksdb::DB *oldDbRaw = nullptr;
  rocksdb::Status status = rocksdb::DB::Open(oldOptions, path_to_utf8(oldPartPath), &oldDbRaw);
  if (!status.ok()) {
    LOG_F(ERROR, "Can't open partition %s: %s", partitionName.c_str(), status.ToString().c_str());
    return false;
  }
  std::unique_ptr<rocksdb::DB> oldDb(oldDbRaw);

  rocksdb::Options newOptions;
  newOptions.create_if_missing = true;
  newOptions.compression = rocksdb::kZSTD;
  rocksdb::DB *newDbRaw = nullptr;
  status = rocksdb::DB::Open(newOptions, path_to_utf8(newPartPath), &newDbRaw);
  if (!status.ok()) {
    LOG_F(ERROR, "Can't create partition %s: %s", partitionName.c_str(), status.ToString().c_str());
    return false;
  }
  std::unique_ptr<rocksdb::DB> newDb(newDbRaw);

  unsigned count = 0;
  unsigned batchCount = 0;
  rocksdb::WriteBatch batch;

  rocksdb::ReadOptions readOptions;
  std::unique_ptr<rocksdb::Iterator> it(oldDb->NewIterator(readOptions));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (!callback(it.get(), batch))
      return false;

    count++;
    batchCount++;

    if (batchCount >= MigrateBatchSize) {
      rocksdb::WriteOptions writeOptions;
      newDb->Write(writeOptions, &batch);
      batch.Clear();
      batchCount = 0;
    }
  }

  if (batchCount > 0) {
    rocksdb::WriteOptions writeOptions;
    newDb->Write(writeOptions, &batch);
  }

  LOG_F(INFO, "  %s: migrated %u records, compacting...", partitionName.c_str(), count);
  rocksdb::CompactRangeOptions compactOptions;
  newDb->CompactRange(compactOptions, nullptr, nullptr);
  return true;
}

bool migrationTargetExists(const std::filesystem::path &dbPath, const char *newName)
{
  std::filesystem::path targetPath = dbPath / newName;
  if (std::filesystem::exists(targetPath)) {
    LOG_F(INFO, "%s already exists, skipping migration", newName);
    return true;
  }
  return false;
}

static bool renameOldDb(const std::filesystem::path &oldDbPath)
{
  std::string oldName = oldDbPath.filename().generic_string();
  std::filesystem::path bakPath = oldDbPath.parent_path() / ("__" + oldName + ".bak");
  std::error_code ec;
  std::filesystem::rename(oldDbPath, bakPath, ec);
  if (ec) {
    LOG_F(ERROR, "Can't rename %s to %s: %s", oldName.c_str(), bakPath.filename().generic_string().c_str(), ec.message().c_str());
    return false;
  }

  LOG_F(INFO, "Renamed %s -> %s", oldName.c_str(), bakPath.filename().generic_string().c_str());
  return true;
}

bool migrateDatabase(const std::filesystem::path &dbPath, const char *baseName, const char *newName, MigrateCallback callback)
{
  std::filesystem::path newDbPath = dbPath / newName;
  if (std::filesystem::exists(newDbPath)) {
    LOG_F(INFO, "%s already exists, skipping migration", newName);
    return true;
  }

  std::filesystem::path oldDbPath = dbPath / baseName;
  if (!std::filesystem::exists(oldDbPath) || !std::filesystem::is_directory(oldDbPath)) {
    LOG_F(INFO, "No previous %s database found, skipping", baseName);
    return true;
  }

  LOG_F(INFO, "Migrating %s -> %s ...", baseName, newName);

  std::filesystem::create_directories(newDbPath);

  for (std::filesystem::directory_iterator I(oldDbPath), IE; I != IE; ++I) {
    if (!is_directory(I->status()))
      continue;
    std::string partitionName = I->path().filename().generic_string();
    if (!migratePartition(I->path(), newDbPath / partitionName, partitionName, callback))
      return false;
  }

  return renameOldDb(oldDbPath);
}

bool migrateDatabaseMt(const std::filesystem::path &dbPath, const char *baseName, const char *newName, MigrateCallback callback, unsigned threads, PartitionFilter filter)
{
  std::filesystem::path newDbPath = dbPath / newName;
  if (std::filesystem::exists(newDbPath)) {
    LOG_F(INFO, "%s already exists, skipping migration", newName);
    return true;
  }

  std::filesystem::path oldDbPath = dbPath / baseName;
  if (!std::filesystem::exists(oldDbPath) || !std::filesystem::is_directory(oldDbPath)) {
    LOG_F(INFO, "No previous %s database found, skipping", baseName);
    return true;
  }

  // Collect partition directories
  std::vector<std::string> partitions;
  for (std::filesystem::directory_iterator I(oldDbPath), IE; I != IE; ++I) {
    if (!is_directory(I->status()))
      continue;
    std::string name = I->path().filename().generic_string();
    if (filter && !filter(name)) {
      LOG_F(INFO, "  skipping partition %s (filtered)", name.c_str());
      continue;
    }
    partitions.push_back(std::move(name));
  }
  std::sort(partitions.begin(), partitions.end());

  LOG_F(INFO, "Migrating %s -> %s (%zu partitions, %u threads) ...", baseName, newName, partitions.size(), threads);

  std::filesystem::create_directories(newDbPath);

  // Process partitions in groups
  for (size_t i = 0; i < partitions.size(); i += threads) {
    size_t end = std::min(i + static_cast<size_t>(threads), partitions.size());
    std::vector<std::future<bool>> futures;
    for (size_t j = i; j < end; j++) {
      futures.push_back(std::async(std::launch::async, [&oldDbPath, &newDbPath, &partitions, &callback, j]() {
        return migratePartition(oldDbPath / partitions[j], newDbPath / partitions[j], partitions[j], callback);
      }));
    }

    for (auto &f : futures) {
      if (!f.get())
        return false;
    }
  }

  return renameOldDb(oldDbPath);
}

bool migrateFile(const std::filesystem::path &dirPath, const char *baseName, const char *newName, MigrateFileCallback callback)
{
  std::filesystem::path newFilePath = dirPath / newName;
  if (std::filesystem::exists(newFilePath)) {
    LOG_F(INFO, "%s already exists, skipping migration", newName);
    return true;
  }

  std::filesystem::path oldFilePath = dirPath / baseName;
  if (!std::filesystem::exists(oldFilePath) || !std::filesystem::is_regular_file(oldFilePath)) {
    LOG_F(INFO, "No previous %s file found, skipping", baseName);
    return true;
  }

  LOG_F(INFO, "Migrating file %s -> %s ...", baseName, newName);

  // Read entire old file
  auto fileSize = std::filesystem::file_size(oldFilePath);
  xmstream input;
  if (fileSize > 0) {
    FILE *f = fopen(path_to_utf8(oldFilePath).c_str(), "rb");
    if (!f) {
      LOG_F(ERROR, "Can't open %s: %s", baseName, strerror(errno));
      return false;
    }
    fread(input.reserve(fileSize), 1, fileSize, f);
    fclose(f);
    input.seekSet(0);
  }

  // Convert records
  xmstream output;
  unsigned count = 0;
  while (input.remaining()) {
    if (!callback(input, output))
      return false;
    count++;
  }

  // Write new file
  FILE *f = fopen(path_to_utf8(newFilePath).c_str(), "wb");
  if (!f) {
    LOG_F(ERROR, "Can't create %s: %s", newName, strerror(errno));
    return false;
  }
  if (output.sizeOf() > 0)
    fwrite(output.data(), 1, output.sizeOf(), f);
  fclose(f);

  LOG_F(INFO, "  migrated %u records", count);
  return renameOldDb(oldFilePath);
}

static bool migrateOneFile(const std::filesystem::path &oldFilePath, const std::filesystem::path &newFilePath,
                            const std::string &fileName, const MigrateFileCallback &callback)
{
  auto fileSize = std::filesystem::file_size(oldFilePath);
  xmstream input;
  if (fileSize > 0) {
    FILE *f = fopen(path_to_utf8(oldFilePath).c_str(), "rb");
    if (!f) {
      LOG_F(ERROR, "Can't open %s: %s", fileName.c_str(), strerror(errno));
      return false;
    }
    fread(input.reserve(fileSize), 1, fileSize, f);
    fclose(f);
    input.seekSet(0);
  }

  xmstream output;
  unsigned count = 0;
  while (input.remaining()) {
    if (!callback(input, output))
      return false;
    count++;
  }

  FILE *f = fopen(path_to_utf8(newFilePath).c_str(), "wb");
  if (!f) {
    LOG_F(ERROR, "Can't create %s: %s", fileName.c_str(), strerror(errno));
    return false;
  }
  if (output.sizeOf() > 0)
    fwrite(output.data(), 1, output.sizeOf(), f);
  fclose(f);

  LOG_F(INFO, "  %s: migrated %u records", fileName.c_str(), count);
  return true;
}

bool migrateDirectory(const std::filesystem::path &dbPath, const char *baseName, const char *newName, MigrateFileCallback callback, bool skipTargetCheck)
{
  std::filesystem::path newDirPath = dbPath / newName;
  if (!skipTargetCheck && std::filesystem::exists(newDirPath)) {
    LOG_F(INFO, "%s already exists, skipping migration", newName);
    return true;
  }

  std::filesystem::path oldDirPath = dbPath / baseName;
  if (!std::filesystem::exists(oldDirPath) || !std::filesystem::is_directory(oldDirPath)) {
    LOG_F(INFO, "No previous %s directory found, skipping", baseName);
    return true;
  }

  // Collect files
  std::vector<std::string> files;
  for (std::filesystem::directory_iterator I(oldDirPath), IE; I != IE; ++I) {
    if (!is_regular_file(I->status()))
      continue;
    files.push_back(I->path().filename().generic_string());
  }
  std::sort(files.begin(), files.end());

  LOG_F(INFO, "Migrating directory %s -> %s (%zu files) ...", baseName, newName, files.size());

  std::filesystem::create_directories(newDirPath);

  for (const auto &file : files) {
    if (!migrateOneFile(oldDirPath / file, newDirPath / file, file, callback))
      return false;
  }

  return renameOldDb(oldDirPath);
}

bool migrateDirectoryMt(const std::filesystem::path &dbPath, const char *baseName, const char *newName, MigrateFileCallback callback, unsigned threads, bool skipTargetCheck)
{
  std::filesystem::path newDirPath = dbPath / newName;
  if (!skipTargetCheck && std::filesystem::exists(newDirPath)) {
    LOG_F(INFO, "%s already exists, skipping migration", newName);
    return true;
  }

  std::filesystem::path oldDirPath = dbPath / baseName;
  if (!std::filesystem::exists(oldDirPath) || !std::filesystem::is_directory(oldDirPath)) {
    LOG_F(INFO, "No previous %s directory found, skipping", baseName);
    return true;
  }

  // Collect files
  std::vector<std::string> files;
  for (std::filesystem::directory_iterator I(oldDirPath), IE; I != IE; ++I) {
    if (!is_regular_file(I->status()))
      continue;
    files.push_back(I->path().filename().generic_string());
  }
  std::sort(files.begin(), files.end());

  LOG_F(INFO, "Migrating directory %s -> %s (%zu files, %u threads) ...", baseName, newName, files.size(), threads);

  std::filesystem::create_directories(newDirPath);

  // Process files in groups
  for (size_t i = 0; i < files.size(); i += threads) {
    size_t end = std::min(i + static_cast<size_t>(threads), files.size());
    std::vector<std::future<bool>> futures;
    for (size_t j = i; j < end; j++) {
      futures.push_back(std::async(std::launch::async, [&oldDirPath, &newDirPath, &files, &callback, j]() {
        return migrateOneFile(oldDirPath / files[j], newDirPath / files[j], files[j], callback);
      }));
    }

    for (auto &f : futures) {
      if (!f.get())
        return false;
    }
  }

  return renameOldDb(oldDirPath);
}

std::string formatSI(const std::string &decimal)
{
  static const char suffixes[] = {0, 'K', 'M', 'G', 'T', 'P', 'E', 'Z', 'Y'};

  size_t len = decimal.size();
  if (len <= 3)
    return decimal;

  unsigned level = (len - 1) / 3;
  if (level > 8)
    level = 8;

  size_t intPart = len - level * 3;
  std::string result = decimal.substr(0, intPart);
  result += '.';
  result += decimal.substr(intPart, 3);
  result += suffixes[level];
  return result;
}
