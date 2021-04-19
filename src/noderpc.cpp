#include <stdio.h>
#include "poolcommon/utils.h"
#include "poolcore/backendData.h"
#include "poolcore/bitcoinRPCClient.h"
#include "poolcore/coinLibrary.h"
#include "p2putils/uriParse.h"
#include "loguru.hpp"
#include <inttypes.h>
#include <getopt.h>

enum CmdLineOptsTy {
  clOptHelp = 1,
  clOptNode,
  clOptCoin,
  clOptAddress,
  clOptUser,
  clOptPassword,
  clOptMethod,
  clOptMiningAddresses
};

static option cmdLineOpts[] = {
  {"help", no_argument, nullptr, clOptHelp},
  {"node", required_argument, nullptr, clOptNode},
  {"coin", required_argument, nullptr, clOptCoin},
  {"address", required_argument, nullptr, clOptAddress},
  {"user", required_argument, nullptr, clOptUser},
  {"password", required_argument, nullptr, clOptPassword},
  {"method", required_argument, nullptr, clOptMethod},
  {"mining-addresses", required_argument, nullptr, clOptMiningAddresses},
  {nullptr, 0, nullptr, 0}
};

struct Context {
  asyncBase *Base;
  std::unique_ptr<CNetworkClient> Client;
  CCoinInfo CoinInfo;
  char **Argv;
  size_t ArgsNum;
};

void getBalanceCoro(Context *context)
{
  CNetworkClient::GetBalanceResult result;
  if (context->Client->ioGetBalance(context->Base, result)) {
    LOG_F(INFO, "balance: %s immature: %s\n", FormatMoney(result.Balance, context->CoinInfo.RationalPartSize).c_str(), FormatMoney(result.Immatured, context->CoinInfo.RationalPartSize).c_str());
  } else {
    LOG_F(ERROR, "ioGetBalance failed");
  }
}

void buildTransactionCoro(Context *context)
{
  if (context->ArgsNum != 3) {
    LOG_F(INFO, "Usage: buildTransaction <address> <change_address> <amount>");
    return;
  }

  const char *destinationAddress = context->Argv[0];
  const char *changeAddress = context->Argv[1];
  const char *amount = context->Argv[2];

  if (!context->CoinInfo.checkAddress(destinationAddress, context->CoinInfo.PayoutAddressType)) {
    LOG_F(INFO, "Invalid %s address: %s", context->CoinInfo.Name.c_str(), destinationAddress);
    return;
  }

  int64_t value;
  if (!parseMoneyValue(amount, context->CoinInfo.RationalPartSize, &value)) {
    LOG_F(INFO, "Invalid amount: %s %s", amount, context->CoinInfo.Name.c_str());
    return;
  }

  CNetworkClient::BuildTransactionResult transaction;
  CNetworkClient::EOperationStatus status =
    context->Client->ioBuildTransaction(context->Base, destinationAddress, changeAddress, value, transaction);
  if (status == CNetworkClient::EStatusOk) {
    // Nothing to do
  } else if (status == CNetworkClient::EStatusInsufficientFunds) {
    LOG_F(INFO, "No money left to pay");
    return;
  } else {
    LOG_F(ERROR, "Payment %s to %s failed with error \"%s\"", FormatMoney(value, context->CoinInfo.RationalPartSize).c_str(), destinationAddress, transaction.Error.c_str());
    return;
  }

  LOG_F(INFO, "txData: %s", transaction.TxData.c_str());
  LOG_F(INFO, "txId: %s", transaction.TxId.c_str());
  LOG_F(INFO, "real value: %s", FormatMoney(transaction.Value, context->CoinInfo.RationalPartSize).c_str());
  LOG_F(INFO, "fee: %s", FormatMoney(transaction.Fee, context->CoinInfo.RationalPartSize).c_str());
}

void sendTransactionCoro(Context *context)
{
  if (context->ArgsNum != 1) {
    LOG_F(INFO, "Usage: sendTransaction <txdata>");
    return;
  }

  const char *txData = context->Argv[0];
  std::string error;
  CNetworkClient::EOperationStatus status =
    context->Client->ioSendTransaction(context->Base, txData, error);
  if (status == CNetworkClient::EStatusOk) {
    // Nothing to do
  } else if (status == CNetworkClient::EStatusVerifyRejected) {
    LOG_F(ERROR, "Transaction %s rejected", txData);
    return;
  } else {
    LOG_F(WARNING, "Sending transaction %s error \"%s\", will try send later...", txData, error.c_str());
    return;
  }

  LOG_F(INFO, "sending ok");
}

void getTxConfirmationsCoro(Context *context)
{
  if (context->ArgsNum != 1) {
    LOG_F(INFO, "Usage: getTxConfirmations <txid>");
    return;
  }

  const char *txId = context->Argv[0];
  std::string error;
  int64_t confirmations = 0;
  CNetworkClient::EOperationStatus status = context->Client->ioGetTxConfirmations(context->Base, txId, &confirmations, error);
  if (status == CNetworkClient::EStatusOk) {
    // Nothing to do
  } else if (status == CNetworkClient::EStatusInvalidAddressOrKey) {
    LOG_F(ERROR, "Transaction %s lost, need resend", txId);
    return;
  } else {
    LOG_F(WARNING, "Transaction %s checkong error \"%s\", will try later...", txId, error.c_str());
    return;
  }

  LOG_F(INFO, "Confirmations: %" PRIi64 "", confirmations);
}

void getBlockConfirmationCoro(Context *context)
{
  if (context->ArgsNum != 2) {
    LOG_F(INFO, "Usage: getBlockConfirmation <hash> <height>");
    return;
  }

  const char *blockHash = context->Argv[0];
  const char *blockHeight = context->Argv[1];

  std::vector<CNetworkClient::GetBlockConfirmationsQuery> query;
  CNetworkClient::GetBlockConfirmationsQuery &queryElement = query.emplace_back();
  queryElement.Hash = blockHash;
  queryElement.Height = xatoi<uint64_t>(blockHeight);
  if (context->Client->ioGetBlockConfirmations(context->Base, query)) {
    LOG_F(INFO, "confirmations: %" PRId64 "", queryElement.Confirmations);
  } else {
    LOG_F(ERROR, "can't get confirmations for %s", blockHash);
  }
}

void printHelpMessage()
{
  printf("noderpc usage:\n");
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

  std::string type;
  std::string coin;
  const char *address = nullptr;
  const char *user = "";
  const char *password = "";
  std::string method;
  std::vector<std::string> miningAddresses;

  // Parsing command line
  int res;
  int index = 0;
  while ((res = getopt_long(argc, argv, "", cmdLineOpts, &index)) != -1) {
    switch (res) {
      case clOptHelp :
        printHelpMessage();
        return 0;
      case clOptNode:
        type = optarg;
        break;
      case clOptCoin:
        coin = optarg;
        break;
      case clOptAddress:
        address = optarg;
        break;
      case clOptUser:
        user = optarg;
        break;
      case clOptPassword:
        password = optarg;
        break;
      case clOptMethod:
        method = optarg;
        break;
      case clOptMiningAddresses: {
        const char *p = optarg;
        while (p) {
          const char *commaPtr = strchr(p, ',');
          miningAddresses.emplace_back(p, commaPtr ? commaPtr-p : strlen(p));
          p = commaPtr ? commaPtr+1 : nullptr;
        }
        break;
      }
      case ':' :
        fprintf(stderr, "Error: option %s missing argument\n", cmdLineOpts[index].name);
        break;
      case '?' :
        exit(1);
      default :
        break;
    }
  }

  if (type.empty() || coin.empty() || !address || method.empty()) {
    fprintf(stderr, "Error: you must specify --node, --coin, --address, --method\n");
    exit(1);
  }

  Context context;
  initializeSocketSubsystem();
  context.Base = createAsyncBase(amOSDefault);
  context.Argv = argv + optind;
  context.ArgsNum = argc - optind;

  context.CoinInfo = CCoinLibrary::get(coin.c_str());
  if (context.CoinInfo.Name.empty()) {
    LOG_F(ERROR, "Unknown coin: %s", coin.c_str());
    return 1;
  }

  // Create node
  if (type == "bitcoinrpc") {
    if (!user || !password) {
      fprintf(stderr, "Error: you must specify --user and --password\n");
      exit(1);
    }
    context.Client.reset(new CBitcoinRpcClient(context.Base, 1, context.CoinInfo, address, user, password, true));
  }

  if (method == "getBalance") {
    coroutineCall(coroutineNew([](void *arg) {
      getBalanceCoro(static_cast<Context*>(arg));
      postQuitOperation(static_cast<Context*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "buildTransaction") {
    coroutineCall(coroutineNew([](void *arg) {
      buildTransactionCoro(static_cast<Context*>(arg));
      postQuitOperation(static_cast<Context*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "sendTransaction") {
    coroutineCall(coroutineNew([](void *arg) {
      sendTransactionCoro(static_cast<Context*>(arg));
      postQuitOperation(static_cast<Context*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "getTxConfirmations") {
    coroutineCall(coroutineNew([](void *arg) {
      getTxConfirmationsCoro(static_cast<Context*>(arg));
      postQuitOperation(static_cast<Context*>(arg)->Base);
    }, &context, 0x10000));
  } else if (method == "getBlockConfirmation") {
    coroutineCall(coroutineNew([](void *arg) {
      getBlockConfirmationCoro(static_cast<Context*>(arg));
      postQuitOperation(static_cast<Context*>(arg)->Base);
    }, &context, 0x10000));
  } else {
    LOG_F(ERROR, "Unknown method: %s", method.c_str());
    return 1;
  }

  asyncLoop(context.Base);
  return 0;
}
