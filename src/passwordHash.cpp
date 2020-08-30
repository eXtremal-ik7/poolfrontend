#include "poolcore/usermgr.h"
#include "poolcommon/uint256.h"

int main(int argc, char **argv)
{
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <login> <password>\n", argv[0]);
    return 1;
  }

  printf("%s\n", UserManager::generateHash(argv[1], argv[2]).ToString().c_str());
  return 0;
}
