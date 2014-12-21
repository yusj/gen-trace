#include "x64_target_client.h"
#include "code_manager_impl.h"
#include <assert.h>

char data[] = "\x53"
              "\x31"
              "\xd2"
              "\x48"
              "\x89"
              "\xfb"
              "\x48"
              "\x89"
              "\xf7"
              "\x31"
              "\xf6"
              "\xe8"
              "\x80"
              "\x45"
              "\x50"
              "\x00";

int
main ()
{
  code_manager_impl code_manager;
  x64_target_client x64_target_client;
  target_client *target_client = &x64_target_client;
  code_context *cc;
  assert (target_client->check_code (data, "test", &code_manager, &cc));
  assert (cc->code_point == &data[0]);
  assert (target_client->build_trampoline (&code_manager, cc));
  assert (cc->trampoline_code_start != 0);
  mem_modify_instr *instr
      = target_client->modify_code (cc, (void *)main, (void *)main);
  assert (instr);
}
