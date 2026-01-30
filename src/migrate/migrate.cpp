#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <getopt.h>
#include <filesystem>
#include <string>
#include <thread>

#include "../plugin.h"
#include "loguru.hpp"

CPluginContext gPluginContext;

// Migration functions
bool migrateV3(const std::filesystem::path &dbPath, unsigned threads, const std::string &statisticCutoff);

enum CmdLineOptsTy {
  clOptHelp = 1,
  clOptDbPath,
  clOptThreads,
  clOptKeepStatistic
};

static option cmdLineOpts[] = {
  {"help", no_argument, nullptr, clOptHelp},
  {"dbpath", required_argument, nullptr, clOptDbPath},
  {"threads", required_argument, nullptr, clOptThreads},
  {"keep-statistic", required_argument, nullptr, clOptKeepStatistic},
  {nullptr, 0, nullptr, 0}
};

void printHelpMessage()
{
  printf("migrate usage:\n");
  printf("  --dbpath <path>           Path to the database directory (required)\n");
  printf("  --threads <num>           Number of threads for parallel migration (default: %u)\n", std::max(1u, std::thread::hardware_concurrency() / 2));
  printf("  --keep-statistic <months> Keep statistic partitions for this many months (default: 36)\n");
  printf("  --help                    Show this help message\n");
}

int main(int argc, char **argv)
{
  loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
  loguru::g_preamble_thread = false;
  loguru::g_preamble_file = true;
  loguru::g_flush_interval_ms = 100;
  loguru::init(argc, argv);
  loguru::g_stderr_verbosity = 1;
  loguru::set_thread_name("main");

  const char *dbPath = nullptr;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2);
  unsigned keepStatisticMonths = 36;

  int res;
  int index = 0;
  while ((res = getopt_long(argc, argv, "", cmdLineOpts, &index)) != -1) {
    switch (res) {
      case clOptHelp:
        printHelpMessage();
        return 0;
      case clOptDbPath:
        dbPath = optarg;
        break;
      case clOptThreads:
        threads = std::max(1, atoi(optarg));
        break;
      case clOptKeepStatistic:
        keepStatisticMonths = std::max(1, atoi(optarg));
        break;
      case ':':
        fprintf(stderr, "Error: option %s missing argument\n", cmdLineOpts[index].name);
        break;
      case '?':
        exit(1);
      default:
        break;
    }
  }

  if (!dbPath) {
    fprintf(stderr, "Error: you must specify --dbpath\n");
    exit(1);
  }

  if (!std::filesystem::exists(dbPath)) {
    fprintf(stderr, "Error: database path %s does not exist\n", dbPath);
    exit(1);
  }

  loguru::add_file((std::filesystem::path(dbPath) / "migrate.log").generic_string().c_str(), loguru::Append, loguru::Verbosity_1);

  // Compute statistic cutoff partition name (yyyy.mm)
  std::string statisticCutoff;
  {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    int year = tm.tm_year + 1900;
    int month = tm.tm_mon + 1;
    month -= static_cast<int>(keepStatisticMonths);
    while (month <= 0) {
      month += 12;
      year--;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d.%02d", year, month);
    statisticCutoff = buf;
  }

  LOG_F(INFO, "Using %u threads for parallel migration", threads);
  LOG_F(INFO, "Statistic cutoff: %s (keep %u months)", statisticCutoff.c_str(), keepStatisticMonths);

  // Run migrations
  bool success = true;
  success = migrateV3(dbPath, threads, statisticCutoff) && success;

  if (!success) {
    LOG_F(ERROR, "Migration failed");
    return 1;
  }

  LOG_F(INFO, "All migrations completed successfully");
  return 0;
}
