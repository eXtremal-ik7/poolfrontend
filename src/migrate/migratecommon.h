#pragma once

#include "rocksdb/db.h"
#include "p2putils/xmstream.h"
#include <filesystem>
#include <functional>
#include <string>

static constexpr unsigned MigrateBatchSize = 1u << 14;

using MigrateCallback = std::function<bool(rocksdb::Iterator *it, rocksdb::WriteBatch &batch)>;
using MigrateFileCallback = std::function<bool(xmstream &input, xmstream &output)>;
using PartitionFilter = std::function<bool(const std::string &partitionName)>;

bool migrateDatabase(const std::filesystem::path &dbPath, const char *baseName, const char *newName, MigrateCallback callback);
bool migrateDatabaseMt(const std::filesystem::path &dbPath, const char *baseName, const char *newName, MigrateCallback callback, unsigned threads, PartitionFilter filter = nullptr);
bool migrateFile(const std::filesystem::path &dirPath, const char *baseName, const char *newName, MigrateFileCallback callback);
bool migrationTargetExists(const std::filesystem::path &dbPath, const char *newName);
bool migrateDirectory(const std::filesystem::path &dbPath, const char *baseName, const char *newName, MigrateFileCallback callback, bool skipTargetCheck = false);
bool migrateDirectoryMt(const std::filesystem::path &dbPath, const char *baseName, const char *newName, MigrateFileCallback callback, unsigned threads, bool skipTargetCheck = false);

std::string formatSI(const std::string &decimal);
