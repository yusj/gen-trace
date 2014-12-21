#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "x64_target_client.h"
#include "dis.h"
#include "dis_client.h"
#include "mem_modify.h"

// jmp *xxxx;
const static int byte_needed_to_modify = 6;
const static int max_tempoline_insert_space = 16;
const static int max_positive_jump = 0x7fffffff;
const static int max_negative_jump = 0x80000000;
const static int nop = 0x90;
// 0000000000000000 <foo()>:
//    0:	55                   	push   %rbp
//    1:	48 89 e5             	mov    %rsp,%rbp
//    4:	ff 25 a0 aa aa 0a    	jmpq   *0xaaaaaa0(%rip)        #
//    aaaaaaa
//    a:	5d                   	pop    %rbp
//    b:	c3                   	retq

x64_target_client::x64_target_client () {}
x64_target_client::~x64_target_client () {}

class x64_dis_client : public dis_client
{
public:
  x64_dis_client () : is_accept_ (true) {}
  inline bool
  is_accept () const
  {
    return is_accept_;
  }

private:
  virtual void on_instr (const char *);
  bool is_accept_;
};

void
x64_dis_client::on_instr (const char *dis_str)
{
  // REMOVE PREFIX
  if (strncmp (dis_str, "REX.W ", 6) == 0)
    {
      dis_str += 6;
    }
  bool check_pass = false;
  // check the instr.
  struct
  {
    const char *instr_name;
    int size;
  } check_list[] = { { "mov", 3 },
                     { "add", 3 },
                     { "sub", 3 },
                     { "div", 3 },
                     { "push", 4 },
                     { "pop", 3 },
                     { "mul", 3 },
                     { "div", 3 },
                     { "xor", 3 },
                     { "or", 2 },
                     { "and", 3 },
                     { "test", 4 } };
  for (size_t i = 0; i < sizeof (check_list) / sizeof (check_list[0]); ++i)
    {
      if (strncmp (dis_str, check_list[i].instr_name, check_list[i].size) == 0)
        {
          check_pass = true;
          break;
        }
    }
  if (!check_pass)
    {
      is_accept_ = false;
      return;
    }
  // check if rip position independent code is here.
  if (strstr (dis_str, "rip"))
    {
      is_accept_ = false;
    }
}

bool
x64_target_client::check_code (void *code_point, const char *name,
                               code_manager *m, code_context **ppcontext)
{
  x64_dis_client dis_client;
  disasm::Disassembler dis (&dis_client);
  char *start = static_cast<char *> (code_point);
  int current = 0;
  while (current < byte_needed_to_modify && dis_client.is_accept ())
    {
      int len = dis.InstructionDecode (start);
      current += len;
      start += len;
    }
  if (dis_client.is_accept () == false)
    {
      return false;
    }
  if (current > max_tempoline_insert_space)
    {
      return false;
    }
  code_context *context;
  context = m->new_context (name);
  if (context == NULL)
    return false;
  context->code_point = code_point;
  context->machine_defined = reinterpret_cast<void *> (current);
  *ppcontext = context;
  return true;
}

extern "C" {
extern void template_for_hook (void);
extern void template_for_hook2 (void);
extern void template_for_hook_end (void);
}

// the layout is in the hook_template.S
bool
x64_target_client::build_trampoline (code_manager *m, code_context *context)
{
  static const int copy_size = (char *)template_for_hook_end
                               - (char *)template_for_hook;
  static const int trampoline_size = copy_size + 8 * 4;
  void *code_mem = m->new_code_mem (trampoline_size);
  if (!code_mem)
    {
      return false;
    }
  // check if we can jump to our code.
  intptr_t code_mem_int = reinterpret_cast<intptr_t> (code_mem);
  intptr_t code_start = code_mem_int + 8 * 4;
  intptr_t target_code_point
      = reinterpret_cast<intptr_t> (context->code_point);
  intptr_t jump_dist = code_start
                       - (target_code_point + byte_needed_to_modify);
  // FIXME: need to delete code mem before returns
  if (jump_dist < 0 && jump_dist < max_negative_jump)
    return false;
  if (jump_dist > 0 && jump_dist > max_positive_jump)
    return false;

  context->trampoline_code_start = reinterpret_cast<char *> (code_start);
  context->trampoline_code_end = reinterpret_cast<char *> (code_start)
                                 + copy_size;
  // copy the hook template to code mem.
  memcpy (reinterpret_cast<void *> (code_start), (char *)template_for_hook,
          copy_size);
  // copy the original target code to trampoline
  char *copy_start = (char *)code_start
                     + ((char *)template_for_hook2 - (char *)template_for_hook)
                     - max_tempoline_insert_space;

  int code_len = reinterpret_cast<intptr_t> (context->machine_defined);
  memcpy (copy_start, context->code_point, code_len);
  return true;
}

mem_modify_instr *
x64_target_client::modify_code (code_context *context, void *called_callback,
                                void *return_callback)
{
  context->called_callback = called_callback;
  context->return_callback = return_callback;
  const char *function_name = context->function_name;
  const void **modify_pointer
      = static_cast<const void **> (context->trampoline_code_start);
  char *target_code_point = static_cast<char *> (context->code_point);
  int modified_code_len
      = reinterpret_cast<intptr_t> (context->machine_defined);
  modify_pointer[-4] = function_name;
  modify_pointer[-3] = called_callback;
  modify_pointer[-2] = target_code_point + modified_code_len;
  modify_pointer[-1] = return_callback;
  // At here the code mem has been modified completely.
  int code_len = reinterpret_cast<intptr_t> (context->machine_defined);
  mem_modify_instr *instr = static_cast<mem_modify_instr *> (
      malloc (sizeof (mem_modify_instr) + code_len));
  instr->where = context->code_point;
  instr->size = code_len;
  char *modify_intr_pointer = reinterpret_cast<char *> (instr + 1);
  memset (modify_intr_pointer, code_len, 0x90);
  modify_intr_pointer[0] = 0xff;
  modify_intr_pointer[1] = 0x25;
  intptr_t jump_dist
      = reinterpret_cast<intptr_t> (context->trampoline_code_start)
        - reinterpret_cast<intptr_t> (target_code_point
                                      + byte_needed_to_modify);
  int jump_dist_int = static_cast<int> (jump_dist);
  memcpy (&modify_intr_pointer[2], &jump_dist_int, sizeof (int));
  return instr;
}
