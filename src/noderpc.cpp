#include <stdio.h>
#include "poolcommon/utils.h"
#include "poolcore/bitcoinRPCClient.h"
#include "poolcore/coinLibrary.h"
#include "p2putils/uriParse.h"
#include "loguru.hpp"
#include <inttypes.h>

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

void sendMoneyCoro(Context *context)
{
  if (context->ArgsNum != 2) {
    LOG_F(INFO, "Usage: sendMoney <address> <amount>");
    return;
  }

  const char *destinationAddress = context->Argv[0];
  const char *amount = context->Argv[1];

  if (!context->CoinInfo.checkAddress(destinationAddress, context->CoinInfo.PayoutAddressType)) {
    LOG_F(INFO, "Invalid %s address: %s", context->CoinInfo.Name.c_str(), destinationAddress);
    return;
  }

  int64_t value;
  if (!parseMoneyValue(amount, context->CoinInfo.RationalPartSize, &value)) {
    LOG_F(INFO, "Invalid amount: %s %s", amount, context->CoinInfo.Name.c_str());
    return;
  }

  CNetworkClient::SendMoneyResult result;
  if (!context->Client->ioSendMoney(context->Base, destinationAddress, value, result)) {
    LOG_F(ERROR, "ioSendMoney error %s", result.Error.c_str());
    return;
  }

  LOG_F(INFO, "txid: %s; fee: %s %s", result.TxId.c_str(), FormatMoney(result.Fee, context->CoinInfo.RationalPartSize).c_str(), context->CoinInfo.Name.c_str());
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

int main(int argc, char **argv)
{
  if (argc < 7) {
    fprintf(stderr, "Usage: %s <type> <coin> <address> <login> <password> <command> <argument1> ... <argumentN>\n", argv[0]);
    return 1;
  }

  loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
  loguru::g_preamble_thread = false;
  loguru::g_preamble_file = true;
  loguru::g_flush_interval_ms = 100;
  loguru::init(argc, argv);
  loguru::g_stderr_verbosity = 1;
  loguru::set_thread_name("main");

  Context context;
  initializeSocketSubsystem();
  context.Base = createAsyncBase(amOSDefault);
  context.Argv = argv + 7;
  context.ArgsNum = argc - 7;

  const std::string type = argv[1];
  const std::string coin = argv[2];
  const std::string address = argv[3];
  const std::string login = argv[4];
  const std::string password = argv[5];
  const std::string command = argv[6];

  context.CoinInfo = CCoinLibrary::get(coin.c_str());
  if (context.CoinInfo.Name.empty()) {
    LOG_F(ERROR, "Unknown coin: %s", coin.c_str());
    return 1;
  }

  // Create node
  if (type == "bitcoinrpc") {
    context.Client.reset(new CBitcoinRpcClient(context.Base, 1, context.CoinInfo, address.c_str(), login.c_str(), password.c_str(), true));
  } else {
    LOG_F(ERROR, "unknown client type: %s", type.c_str());
    return 1;
  }

  if (command == "getBalance") {
    coroutineCall(coroutineNew([](void *arg) {
      getBalanceCoro(static_cast<Context*>(arg));
      postQuitOperation(static_cast<Context*>(arg)->Base);
    }, &context, 0x10000));
  } else if (command == "sendMoney") {
    coroutineCall(coroutineNew([](void *arg) {
      sendMoneyCoro(static_cast<Context*>(arg));
      postQuitOperation(static_cast<Context*>(arg)->Base);
    }, &context, 0x10000));
  } else if (command == "getBlockConfirmation") {
    coroutineCall(coroutineNew([](void *arg) {
      getBlockConfirmationCoro(static_cast<Context*>(arg));
      postQuitOperation(static_cast<Context*>(arg)->Base);
    }, &context, 0x10000));
  } else {
    LOG_F(ERROR, "Unknown method: %s", command.c_str());
    return 1;
  }

  asyncLoop(context.Base);
  return 0;
}
