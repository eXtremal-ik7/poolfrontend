#include "asyncio/asyncio.h"
#include "asyncio/socket.h"
#include "loguru.hpp"

int main(int argc, char *argv[])
{
  initializeSocketSubsystem();
  loguru::init(argc, argv);
  loguru::set_thread_name("main");
  
    
  return 0;
}
