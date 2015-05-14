#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <vector>
#include <unordered_map>
#include <errno.h>
#include <string>
#include "mem_modify.h"
#include "arm_target_client.h"
#include "dis.h"
#include "dis_client.h"
#include "flush_code.h"
#include "log.h"

extern "C" {
extern void template_for_hook_thumb (void);
extern void template_for_hook_thumb_end (void);

extern void template_for_hook_arm (void);
extern void template_for_hook_arm_end (void);
}

static const int arm_jump_bytes = 8;
static const int thumb_ldr_jump_bytes = 8;
static const int thumb_movt_movw_bytes = 10;
static const int arm_movt_movw_bytes = 12;

enum post_process_trampoline_type
{
  INVALID,
  BL,
  CB,
  B,
  ADDPC,
  LDRPC,
  MOVADDR,
};

struct post_process_trampoline_desc
{
  enum post_process_trampoline_type type;
  size_t offset;
  intptr_t addr;
  int offset_add_end;
  size_t instr_size;
  // type bl
  union
  {
    // for bl
    struct
    {
      bool is_blx;
    } bl;
    struct
    {
      bool is_not_zero;
      int rn;
    } cb;
    struct
    {
      int cond_code;
    } b;
    struct
    {
      int rn;
    } add_pc;
    struct
    {
      int rn;
    } ldr_pc;
    struct
    {
      int rn;
    } mov_addr;
  } un;
};

typedef std::vector<post_process_trampoline_desc> desc_vector;

class arm_dis_client : public check_code_dis_client
{
public:
  arm_dis_client (void *code_point);
  inline bool
  is_accept ()
  {
    return is_accept_;
  }

private:
  virtual void on_instr (const char *, char *start, size_t s);
  void on_instr_1 (const char *, char *start, size_t s);
  virtual void on_addr (intptr_t);
  virtual int lowered_original_code_len (int);
  virtual size_t extend_buffer_size ();
  virtual void fill_buffer (void *);
  void commit_desc (const post_process_trampoline_desc &desc);
  void advance (size_t);
  bool is_accept_;
  bool is_thumb_;
  size_t offset_;
  size_t lowered_original_code_len_;
  bool ip_appeared_;
  intptr_t last_addr_;
  std::unique_ptr<desc_vector> desc_;
  friend class arm_target_client;
};

arm_dis_client::arm_dis_client (void *code_point)
    : is_accept_ (true),
      is_thumb_ ((reinterpret_cast<intptr_t> (code_point) & 1) != 0),
      offset_ (0), lowered_original_code_len_ (0), ip_appeared_ (false),
      last_addr_ (-1)
{
}

void
arm_dis_client::commit_desc (const post_process_trampoline_desc &desc)
{
  if (desc_.get () == NULL)
    {
      desc_.reset (new desc_vector);
    }
  desc_->push_back (desc);
  lowered_original_code_len_ += desc.offset_add_end - desc.instr_size;
}

void
arm_dis_client::advance (size_t s)
{
  offset_ += s;
  lowered_original_code_len_ += s;
}

void
arm_dis_client::on_instr (const char *dis_str, char *start, size_t s)
{
  on_instr_1 (dis_str, start, s);
  last_addr_ = -1;
  advance (s);
}

size_t
arm_dis_client::extend_buffer_size ()
{
  if (!desc_.get ())
    {
      return 0;
    }
  return sizeof (post_process_trampoline_desc) * desc_->size ();
}

void
arm_dis_client::fill_buffer (void *b)
{
  assert (desc_.get () != nullptr);
  memcpy (b, desc_->data (),
          sizeof (post_process_trampoline_desc) * desc_->size ());
}

static inline bool
should_lower_branch (char *addr, intptr_t jumpto, size_t current_offset)
{
  char *pinstr
      = reinterpret_cast<char *> (reinterpret_cast<intptr_t> (addr) & ~1UL);
  intptr_t start = reinterpret_cast<intptr_t> (pinstr)
                   - static_cast<int> (current_offset);
  intptr_t end;
  if (start & 3UL)
    {
      end = start + thumb_movt_movw_bytes;
    }
  else
    {
      end = start + thumb_ldr_jump_bytes;
    }
  if (jumpto < end)
    {
      return false;
    }
  return true;
}

static std::unordered_map<std::string, int> g_condi_keywords
    = { { "eq", 0 },  { "ne", 1 },  { "cs", 2 },  { "cc", 3 }, { "mi", 4 },
        { "pl", 5 },  { "vs", 6 },  { "vc", 7 },  { "hi", 8 }, { "ls", 9 },
        { "ge", 10 }, { "lt", 11 }, { "gt", 12 }, { "le", 13 } };

static bool
is_conditional_code (const char *str)
{
  if (g_condi_keywords.find (std::string (str, 2)) != g_condi_keywords.end ())
    {
      return true;
    }
  return false;
}

static bool
is_branch (const char *str)
{
  if (str[1] == '\0')
    {
      // simple
      return true;
    }
  if (str[1] == 'l' && str[2] == 'x')
    {
      // blx
      return false;
    }
  if (is_conditional_code (str + 1))
    {
      return true;
    }
  return false;
}

void
arm_dis_client::on_instr_1 (const char *dis_str, char *start, size_t s)
{
  bool check_pass = false;
  bool pc_showup = false;
  // check the instr.
  enum instr_type
  {
    MOV_TYPE,
    PUSH_TYPE,
    POP_TYPE,
    LDR_TYPE,
    STR_TYPE,
    STM_TYPE,
    LDM_TYPE,
    ADD_TYPE,
    SUB_TYPE,
    MUL_TYPE,
    DIV_TYPE,
    XOR_TYPE,
    OR_TYPE,
    AND_TYPE,
    NOT_TYPE,
    CMP_TYPE,
    LSL_TYPE,
    LSR_TYPE,
    ASR_TYPE,
    B_TYPE,
    BL_TYPE,
    CB_TYPE,
    ASL_TYPE,
    TST_TYPE,
    MVN_TYPE,
    VPUSH_TYPE,
    VLD_TYPE,
    VMOV_TYPE,
  };
  struct
  {
    const char *instr_name;
    int size;
    enum instr_type type;
  } check_list[] = { { "mov", 3, MOV_TYPE },     { "push", 4, PUSH_TYPE },
                     { "pop", 3, POP_TYPE },     { "ldr", 3, LDR_TYPE },
                     { "str", 3, STR_TYPE },     { "stm", 3, STM_TYPE },
                     { "ldm", 3, LDM_TYPE },     { "add", 3, ADD_TYPE },
                     { "sub", 3, SUB_TYPE },     { "mul", 3, MUL_TYPE },
                     { "div", 3, DIV_TYPE },     { "xor", 3, XOR_TYPE },
                     { "or", 2, OR_TYPE },       { "and", 3, AND_TYPE },
                     { "not", 3, NOT_TYPE },     { "cmp", 3, CMP_TYPE },
                     { "lsl", 3, LSL_TYPE },     { "lsr", 3, LSR_TYPE },
                     { "asr", 3, ASR_TYPE },     { "cb", 2, CB_TYPE },
                     { "asl", 3, ASL_TYPE },     { "tst", 3, TST_TYPE },
                     { "mvn", 3, MVN_TYPE },     { "b", 1, B_TYPE },
                     { "vpush", 5, VPUSH_TYPE }, { "vld", 3, VLD_TYPE },
                     { "vmov", 4, VMOV_TYPE } };
  enum instr_type instr_type;
  for (size_t i = 0; i < sizeof (check_list) / sizeof (check_list[0]); ++i)
    {
      if (strncmp (dis_str, check_list[i].instr_name, check_list[i].size) == 0)
        {
          check_pass = true;
          instr_type = check_list[i].type;
          break;
        }
    }
  if (!check_pass)
    {
      is_accept_ = false;
      return;
    }
  char *operand;
  if ((operand = strchr (dis_str, '\t')))
    {
      *operand = '\0';
      operand += 1;
      {
        char *comment;
        if ((comment = strchr (operand, ';')))
          {
            comment[0] = '\0';
          }
      }
      if (strstr (operand, "ip"))
        {
          ip_appeared_ = true;
        }
      // check if pc position independent code is here.
      // currently these code is not supported
      if (strstr (operand, "pc"))
        {
          pc_showup = true;
        }
    }
  // check if pc position independent code is here.
  do
    {
      // hack on bl inst
      if (instr_type == B_TYPE)
        {
          // remove .n, .w suffix
          char *dot_n;
          if ((dot_n = strrchr (dis_str, '.')))
            {
              if (dot_n[1] == 'n' || dot_n[1] == 'w')
                {
                  dot_n[0] = '\0';
                }
            }
          // conditional is harder, not handle now.
          if (ip_appeared_)
            {
              is_accept_ = false;
              break;
            }
          // if this is a bl rx or bx case.
          if (strchr (operand, 'r') || dis_str[1] == 'x')
            {
              // just copy not need to handle
              break;
            }

          // bl and blx unconditional.
          if (dis_str[1] == 'l'
              && (dis_str[2] == '\0'
                  || (dis_str[2] == 'x' && dis_str[3] == '\0')))
            {
              // bl unconditional
              // movt, movw, blx ip
              int offset_add_end = 4 + 4 + (is_thumb_ ? 2 : 4);
              assert (last_addr_ != -1);
              post_process_trampoline_desc desc
                  = { BL, offset_, last_addr_, offset_add_end, s };
              desc.un.bl.is_blx = dis_str[2] == 'x';
              commit_desc (desc);
            }
          else if (is_branch (dis_str))
            {
              // branch
              assert (last_addr_ != -1);

              // in this circumstance, we don't want to do any change.
              // just copy it to the original code part.
              if (!should_lower_branch (start, last_addr_, offset_))
                {
                  // FIXME: ISSUES#10 now we can not handle such a short
                  // branch.
                  is_accept_ = false;
                  break;
                }
              int offset_add_end;
              if (is_thumb_)
                {
                  // bcond, b.n ldr.w nop address
                  // or ldr.w nop nop nop address
                  offset_add_end = 2 + 2 + 4 + 2 + 4;
                }
              else
                {
                  // bcond, b, ldr address
                  // or ldr address, address, nop,nop
                  offset_add_end = 4 + 4 + 4 + 4;
                }
              post_process_trampoline_desc desc
                  = { B, offset_, last_addr_, offset_add_end, s };
              // always;
              int cond_code = 14;
              auto found = g_condi_keywords.find (dis_str + 1);
              if (found != g_condi_keywords.end ())
                {
                  cond_code = found->second;
                }
              desc.un.b.cond_code = cond_code;
              commit_desc (desc);
            }
          else
            {
              is_accept_ = false;
              break;
            }
        }
      else if (instr_type == CB_TYPE)
        {
          assert (last_addr_ != -1);
          uint16_t *pinstr = reinterpret_cast<uint16_t *> (
              reinterpret_cast<intptr_t> (start) & ~1UL);
          // in this circumstance, we don't want to do any change.
          // just copy it to the original code part.
          if (!should_lower_branch (start, last_addr_, offset_))
            {
              // FIXME: ISSUES#10 now we can not handle such a short
              // branch.
              is_accept_ = false;
              break;
            }
          uint16_t instr = *pinstr;
          bool is_not_zero = false;
          if (instr & (1 << 11))
            {
              is_not_zero = true;
            }
          // cb{n}z, ldr.w pc, [pc, 0], nop, 4 byte address
          int offset_add_end = 2 + 4 + 2 + 4;
          post_process_trampoline_desc desc
              = { CB, offset_, last_addr_, offset_add_end, s };
          desc.un.cb.is_not_zero = is_not_zero;
          desc.un.cb.rn = instr & ((1 << 3) - 1);
          commit_desc (desc);
        }
      else if (pc_showup)
        {
          if (operand[0] != 'r' || is_conditional_code (dis_str + 3))
            {
              is_accept_ = false;
              break;
            }
          if (instr_type == LDR_TYPE)
            {
              // ldr rn, [pc, rx] case
              if (last_addr_ == -1)
                {
                  is_accept_ = false;
                  break;
                }
              // ldr rn,[pc, #?] case
              int rn = operand[1] - '0';
              if (rn > 9)
                {
                  is_accept_ = false;
                  break;
                }

              int offset_add_end;
              if (is_thumb_)
                {
                  // movt, movw, ldr.w
                  offset_add_end = 4 + 4 + 4;
                }
              else
                {
                  // same.
                  offset_add_end = 4 + 4 + 4;
                }
              post_process_trampoline_desc desc
                  = { LDRPC, offset_, last_addr_, offset_add_end, s };
              desc.un.ldr_pc.rn = rn;
              commit_desc (desc);
            }
          else if (instr_type == ADD_TYPE)
            {
              // add rn, pc, #? case
              int rn = operand[1] - '0';
              if (rn > 9)
                {
                  is_accept_ = false;
                  break;
                }
              char *operand2 = NULL;
              long offset = 0;
              bool offset_exists = false;
              do
                {
                  if ((operand2 = strchr (operand, ',')))
                    {
                      operand2 += 2;
                      // if operand2 pc
                      if (operand2[0] != 'p' || operand2[1] != 'c')
                        {
                          break;
                        }
                      char *operand3;
                      if ((operand3 = strchr (operand2, ',')))
                        {
                          operand3 += 3;
                          if (operand3[-1] != '#')
                            {
                              // add rn, pc, rx case, must fail.
                              // failed.
                              operand2 = NULL;
                              break;
                            }
                          errno = 0;
                          offset = strtol (operand3, NULL, 10);
                          if (errno)
                            {
                              // failed.
                              operand2 = NULL;
                              break;
                            }
                          offset_exists = true;
                        }
                    }
                }
              while (false);
              if (operand2 == NULL)
                {
                  is_accept_ = false;
                  break;
                }
              int offset_add_end;
              intptr_t addr = reinterpret_cast<intptr_t> (start) & ~1UL;
              if (is_thumb_)
                {
                  addr += 4;
                }
              else
                {
                  addr += 8;
                }
              addr += offset;
              post_process_trampoline_desc desc;
              if (offset_exists)
                {
                  // equals to adr.
                  addr &= ~3UL;
                  desc = { MOVADDR, offset_, addr, 8, s };
                  desc.un.mov_addr.rn = rn;
                }
              else
                {
                  // real add rn, rx.
                  if (is_thumb_)
                    {
                      // push rx; movt rx; movw rx;  add rn, rx;
                      // pop rx;
                      offset_add_end = 2 + 4 + 4 + 2 + 2;
                    }
                  else
                    {
                      // push rx; movt rx; movw rx;  add rn, rx;
                      // pop rx;
                      offset_add_end = 4 + 4 + 4 + 4 + 4;
                    }
                  desc = { ADDPC, offset_, addr, offset_add_end, s };
                  desc.un.add_pc.rn = rn;
                }
              commit_desc (desc);
            }
          else
            {
              is_accept_ = false;
              break;
            }
        }
    }
  while (false);
}

void
arm_dis_client::on_addr (intptr_t addr)
{
  last_addr_ = addr;
}

int
arm_dis_client::lowered_original_code_len (int)
{
  return lowered_original_code_len_;
}

class arm_test_back_egde_client : public dis_client
{
public:
  arm_test_back_egde_client (intptr_t base, intptr_t hook_end);

  bool
  is_accept ()
  {
    return is_accept_;
  }

private:
  virtual void on_instr (const char *, char *start, size_t s);
  virtual void on_addr (intptr_t);
  bool is_accept_;
  intptr_t base_;
  intptr_t hook_end_;
};

arm_test_back_egde_client::arm_test_back_egde_client (intptr_t base,
                                                      intptr_t hook_end)
    : is_accept_ (true), base_ (base), hook_end_ (hook_end)
{
}

void
arm_test_back_egde_client::on_instr (const char *, char *start, size_t s)
{
}

void
arm_test_back_egde_client::on_addr (intptr_t ref)
{
  if (ref < hook_end_ && ref >= base_)
    {
      is_accept_ = false;
    }
}

arm_target_client::arm_target_client () {}
arm_target_client::~arm_target_client () {}

int
arm_target_client::byte_needed_to_modify (intptr_t target_code_point)
{
  if (target_code_point & 1)
    {
      intptr_t target_code_point_2 = target_code_point & ~1UL;
      if (target_code_point_2 & 3UL)
        {
          return thumb_movt_movw_bytes;
        }
      else
        {
          return thumb_ldr_jump_bytes;
        }
    }
  else
    {
      return arm_jump_bytes;
    }
}

disassembler *
arm_target_client::new_disassembler ()
{
  return new disasm::Disassembler ();
}

check_code_dis_client *
arm_target_client::new_code_check_client (void *code_point)
{
  return new arm_dis_client (code_point);
}

dis_client *
arm_target_client::new_backedge_check_client (intptr_t base, intptr_t hookend)
{
  return new arm_test_back_egde_client (base, hookend);
}

extern "C" {
extern void template_for_hook (void);
extern void template_for_hook_thumb_ret (void);
extern void template_for_hook_arm_ret (void);
extern void template_for_hook_end (void);
}

char *
arm_target_client::template_start (intptr_t target_code_point)
{
  if (target_code_point & 1)
    {
      intptr_t r = (intptr_t)template_for_hook_thumb;
      r &= static_cast<intptr_t> (~1);
      return (char *)r;
    }
  else
    {
      return (char *)template_for_hook_arm;
    }
}

char *
arm_target_client::template_end (intptr_t target_code_point)
{
  if (target_code_point & 1)
    {
      intptr_t r = (intptr_t)template_for_hook_thumb_end;
      r &= static_cast<intptr_t> (~1);
      return (char *)r;
    }
  else
    {
      return (char *)template_for_hook_arm_end;
    }
}

bool
arm_target_client::check_jump_dist (intptr_t target_code_point,
                                    intptr_t trampoline_code_start)
{
  return true;
}

void
arm_target_client::flush_code (void *code_start, int len)
{
  ::flush_code (code_start, len);
}

static int
emit_ldr_w (uint16_t *write, bool &emitted_nop)
{
  int index = 0;
  emitted_nop = (reinterpret_cast<intptr_t> (write) & (3UL)) != 0;
  // ldr.w pc, [pc, #x]
  write[index++] = 0xf8df;
  if (emitted_nop)
    {
      // ldr.w pc, [pc, #4], nop
      write[index++] = 0xf004;
      write[index++] = 0xbf00;
    }
  else
    {
      // ldr.w pc, [pc, #0]
      write[index++] = 0xf000;
    }
  return index;
}

static int
emit_nop_thumb (uint16_t *write)
{
  write[0] = 0xbf00;
  return 1;
}

static int
emit_address_thumb (uint16_t *write, intptr_t addr)
{
  addr |= 1;
  write[0] = (addr & 0xffff);
  write[1] = ((addr >> 16) & 0xffff);
  return 2;
}

static int
emit_movt_movw_thumb (intptr_t target, uint16_t *modify_intr_pointer, int rn)
{
  uint16_t first, second, third, forth;
  uint16_t lower_imm16 = (target & 0xffff);
  uint16_t higher_imm16 = static_cast<uintptr_t> (target & 0xffff0000) >> 16;
  // first second: movw
  {
    uint16_t i = (lower_imm16 & (1 << (8 + 3))) >> (8 + 3);
    uint16_t imm8 = lower_imm16 & (0xff);
    uint16_t imm3 = (lower_imm16 & 0x700) >> 8;
    uint16_t imm4 = (lower_imm16 & 0xf000) >> (8 + 3 + 1);
    first = 0x1e << 11;
    first |= i << 10;
    first |= 0x24 << 4;
    first |= imm4;
    second = imm3 << 12;
    second |= rn << 8;
    second |= imm8;
  }
  // third forth: movt
  {
    uint16_t i = (higher_imm16 & (1 << (8 + 3))) >> (8 + 3);
    uint16_t imm8 = higher_imm16 & (0xff);
    uint16_t imm3 = (higher_imm16 & 0x700) >> 8;
    uint16_t imm4 = (higher_imm16 & 0xf000) >> (8 + 3 + 1);
    third = 0x1e << 11;
    third |= i << 10;
    third |= 0x2c << 4;
    third |= imm4;
    forth = imm3 << 12;
    forth |= rn << 8;
    forth |= imm8;
  }
  modify_intr_pointer[0] = first;
  modify_intr_pointer[1] = second;
  modify_intr_pointer[2] = third;
  modify_intr_pointer[3] = forth;
  return 4;
}

static void
emit_movt_movw_branch_thumb (intptr_t target, uint16_t *modify_intr_pointer,
                             int bl)
{
  const uint16_t ip = 12;
  uint16_t fifth;
  emit_movt_movw_thumb (target, modify_intr_pointer, ip);
  {
    fifth = 0x8e << 7;
    fifth |= ip << 3;
  }
  fifth ^= (fifth & (1 << 7)) ^ ((bl & 1) << 7);
  modify_intr_pointer[4] = fifth;
}

static int
emit_movt_movw_arm (intptr_t target, uint32_t *modify_intr_pointer, int rn)
{
  uint32_t first, second;
  uint32_t cond = 0xe; // always
  uint32_t lower_imm16 = (target & 0xffff);
  uint32_t higher_imm16 = static_cast<uintptr_t> (target & 0xffff0000) >> 16;
  {
    uint32_t imm4 = (lower_imm16 & 0xf000) >> 12;
    uint32_t imm12 = 0xfff & lower_imm16;
    first = cond << 28;
    first |= 0x30 << 20;
    first |= imm4 << 16;
    first |= rn << 12;
    first |= imm12;
  }
  {
    uint32_t imm4 = (higher_imm16 & 0xf000) >> 12;
    uint32_t imm12 = 0xfff & higher_imm16;
    second = cond << 28;
    second |= 0x34 << 20;
    second |= imm4 << 16;
    second |= rn << 12;
    second |= imm12;
  }
  modify_intr_pointer[0] = first;
  modify_intr_pointer[1] = second;
  return 2;
}

static void
emit_movt_movw_branch_arm (intptr_t target, uint32_t *modify_intr_pointer,
                           int bl)
{
  const uint16_t ip = 12;
  // use movw ,movt as above.
  uint32_t third;
  uint32_t cond = 0xe; // always
  emit_movt_movw_arm (target, modify_intr_pointer, ip);
  {
    third = cond << 28;
    third |= 0x12fff1 << 4;
    third |= ip;
  }
  third ^= (third & (1 << 5)) ^ ((bl & 1) << 5);

  modify_intr_pointer[2] = third;
}

mem_modify_instr *
arm_target_client::modify_code (target_session *session)
{
  code_context *context = session->code_context ();
  check_code_result_buffer *b = session->check_code_result_buffer ();
  const intptr_t target_code_point
      = reinterpret_cast<intptr_t> (context->code_point);
  int code_len_to_replace = b->code_len_to_replace;
  mem_modify_instr *instr = static_cast<mem_modify_instr *> (
      malloc (sizeof (mem_modify_instr) + code_len_to_replace - 1));
  instr->where = (void *)(target_code_point & static_cast<intptr_t> (~1));
  instr->size = code_len_to_replace;
  intptr_t trampoline_code_start
      = reinterpret_cast<intptr_t> (context->trampoline_code_start);
  if (target_code_point & 1)
    {
      // thumb mode
      // ldr.w pc, [pc, #-0], address
      uint16_t *modify_intr_pointer
          = reinterpret_cast<uint16_t *> (&instr->data[0]);
      intptr_t target_code_point_2 = target_code_point & ~1UL;
      if (target_code_point_2 & 3UL)
        {
          emit_movt_movw_branch_thumb (trampoline_code_start | 1,
                                       modify_intr_pointer, 0);
        }
      else
        {
          modify_intr_pointer[0] = 0xf8df;
          modify_intr_pointer[1] = 0xf000;
          emit_address_thumb (modify_intr_pointer + 2, trampoline_code_start);
        }
    }
  else
    {
      // ldr pc, [pc, #-4], address
      uint32_t *modify_intr_pointer
          = reinterpret_cast<uint32_t *> (&instr->data[0]);
      modify_intr_pointer[0] = 0xe51ff004;
      modify_intr_pointer[1] = trampoline_code_start;
    }
  return instr;
}

static void
handle_cb_thumb (char *&start, char *&write, intptr_t addr, bool is_not_zero,
                 int rn)
{
  // cb{n}z, ldr.w pc, [pc, 0], nop, 4 byte address
  uint16_t cb = 0xb120;
  int _is_not_zero = !is_not_zero;
  cb ^= (cb & (1 << 11)) ^ (_is_not_zero << 11);
  cb |= rn;
  bool emitted_nop;
  // cb completed here.
  uint16_t *_write = reinterpret_cast<uint16_t *> (write);
  int index = 0;
  _write[index++] = cb;
  index += emit_ldr_w (_write + index, emitted_nop);
  index += emit_address_thumb (_write + index, addr);
  if (!emitted_nop)
    {
      index += emit_nop_thumb (_write + index);
    }
  write += sizeof (uint16_t) * index;
}

static void
handle_bl_thumb (char *&start, char *&write, intptr_t addr, bool is_blx)
{
  if (!is_blx)
    {
      addr |= 1;
    }
  emit_movt_movw_branch_thumb (addr, reinterpret_cast<uint16_t *> (write), 1);
  write += thumb_movt_movw_bytes;
}

static void
handle_b_thumb (char *&start, char *&write, intptr_t addr, int cond_code)
{
  int index = 0;
  uint16_t *_write = reinterpret_cast<uint16_t *> (write);
  if (cond_code == 14)
    {
      // always
      // ldr.w nop nop nop address
      bool emitted_nop;
      index += emit_ldr_w (_write, emitted_nop);
      index += emit_address_thumb (_write + index, addr);
      index += emit_nop_thumb (_write + index);
      index += emit_nop_thumb (_write + index);
      if (!emitted_nop)
        {
          index += emit_nop_thumb (_write + index);
        }
    }
  else
    {
      // cond
      // bcond, b.n ldr.w nop address
      bool emitted_nop;
      _write[index++] = 0xd000 | (cond_code << 8);
      // lazy write
      int lazy_index = index++;
      uint16_t bninstr;
      index += emit_ldr_w (_write + index, emitted_nop);
      index += emit_address_thumb (_write + index, addr);
      if (emitted_nop)
        {
          bninstr = 0xe004;
        }
      else
        {
          bninstr = 0xe003;
        }
      _write[lazy_index] = bninstr;
      if (!emitted_nop)
        {
          index += emit_nop_thumb (_write + index);
        }
    }
  write += sizeof (uint16_t) * index;
}

static void
handle_addpc_thumb (char *&start, char *&write, intptr_t addr, int rn)
{
  // push rx; movt rx; movw rx; ldr rx, [rx]; add rn, rx;
  // pop rx;
  uint16_t *_write = reinterpret_cast<uint16_t *> (write);
  int index = 0;
  int rx = (rn + 1) % 8;
  _write[index++] = 0xb400 | (1 << rx);
  index += emit_movt_movw_thumb (addr, _write + index, rx);
  // add
  _write[index++] = 0x4400 | (rx << 3) | rn;
  // pop
  _write[index++] = 0xbc00 | (1 << rx);
  write += sizeof (uint16_t) * index;
}

static void
handle_ldrpc_thumb (char *&start, char *&write, intptr_t addr, int rn)
{
  uint16_t *_write = reinterpret_cast<uint16_t *> (write);
  int index = 0;
  index += emit_movt_movw_thumb (addr, _write, rn);
  _write[index++] = 0xf8d0 | rn;
  _write[index++] = rn << 12;
  write += sizeof (uint16_t) * index;
}

static void
handle_movaddr_thumb (char *&start, char *&write, intptr_t addr, int rn)
{
  uint16_t *_write = reinterpret_cast<uint16_t *> (write);
  int index = 0;
  index += emit_movt_movw_thumb (addr, _write, rn);
  write += sizeof (uint16_t) * index;
}

static void
handle_thumb_entry (post_process_trampoline_desc &desc, char *&start,
                    char *&write)
{
  switch (desc.type)
    {
    case BL:
      handle_bl_thumb (start, write, desc.addr, desc.un.bl.is_blx);
      break;
    case CB:
      handle_cb_thumb (start, write, desc.addr, desc.un.cb.is_not_zero,
                       desc.un.cb.rn);
      break;
    case B:
      handle_b_thumb (start, write, desc.addr, desc.un.b.cond_code);
      break;
    case ADDPC:
      handle_addpc_thumb (start, write, desc.addr, desc.un.add_pc.rn);
      break;
    case LDRPC:
      handle_ldrpc_thumb (start, write, desc.addr, desc.un.ldr_pc.rn);
      break;
    case MOVADDR:
      handle_movaddr_thumb (start, write, desc.addr, desc.un.mov_addr.rn);
      break;
    default:
      assert (false);
    }
  start += desc.instr_size;
}

static void
handle_bl_arm (char *&start, char *&write, intptr_t addr, bool is_blx)
{
  if (is_blx)
    {
      addr |= 1;
    }
  emit_movt_movw_branch_arm (addr, reinterpret_cast<uint32_t *> (write), 1);
  write += arm_movt_movw_bytes;
}

static void
handle_b_arm (char *&start, char *&write, intptr_t addr, int cond_code)
{
  int index = 0;
  uint32_t *_write = reinterpret_cast<uint32_t *> (write);
  if (cond_code == 14)
    {
      // always
      // ldr address, address, nop,nop
      _write[index++] = 0xe51ff004;
      _write[index++] = addr;
      _write[index++] = 0xe320f000;
      _write[index++] = 0xe320f000;
    }
  else
    {
      // bcond, b, ldr address
      _write[index++] = 0x0a000000 | (cond_code << 28);
      _write[index++] = 0xea000001;
      _write[index++] = 0xe51ff004;
      _write[index++] = addr;
    }
  write += index * sizeof (uint32_t);
}

static void
handle_addpc_arm (char *&start, char *&write, intptr_t addr, int rn)
{
  // push rx; movt rx; movw rx; ldr rx, [rx]; add rn, rx;
  // pop rx;
  int index = 0;
  uint32_t *_write = reinterpret_cast<uint32_t *> (write);
  int rx = (rn + 1) % 8;
  _write[index++] = 0xe52d0004 | (rx << 12);
  index += emit_movt_movw_arm (addr, _write + index, rx);
  // add rn, rx
  _write[index++] = 0xe0800000 | (rn << 12) | (rx << 16);
  // pop
  _write[index++] = 0xe49d0004 | (rn << 12);
  write += index * sizeof (uint32_t);
}

static void
handle_ldrpc_arm (char *&start, char *&write, intptr_t addr, int rn)
{
  // push rx; movt rx; movw rx; ldr rx, [rx]; add rn, rx;
  // pop rx;
  int index = 0;
  uint32_t *_write = reinterpret_cast<uint32_t *> (write);
  index += emit_movt_movw_arm (addr, _write, rn);
  _write[index++] = 0xe5900000 | (rn << 12) | (rn << 16);
  write += index * sizeof (uint32_t);
}

static void
handle_movaddr_arm (char *&start, char *&write, intptr_t addr, int rn)
{
  int index = 0;
  uint32_t *_write = reinterpret_cast<uint32_t *> (write);
  index += emit_movt_movw_arm (addr, _write, rn);
  write += index * sizeof (uint32_t);
}

static void
handle_arm_entry (post_process_trampoline_desc &desc, char *&start,
                  char *&write)
{
  switch (desc.type)
    {
    case BL:
      handle_bl_arm (start, write, desc.addr, desc.un.bl.is_blx);
      break;
    case B:
      handle_b_arm (start, write, desc.addr, desc.un.b.cond_code);
      break;
    case ADDPC:
      handle_addpc_arm (start, write, desc.addr, desc.un.add_pc.rn);
      break;
    case LDRPC:
      handle_ldrpc_arm (start, write, desc.addr, desc.un.ldr_pc.rn);
      break;
    case MOVADDR:
      handle_movaddr_arm (start, write, desc.addr, desc.un.mov_addr.rn);
      break;
    default:
      assert (false);
    }
  start += desc.instr_size;
}

void
arm_target_client::copy_original_code (void *trampoline_code_start,
                                       check_code_result_buffer *b)
{
  void *target_code_point = b->code_point;
  int len = b->code_len_to_replace;
  if (!b->size)
    {
      intptr_t target_code_point_i
          = reinterpret_cast<intptr_t> (target_code_point);
      target_code_point_i &= static_cast<intptr_t> (~1);
      memcpy (trampoline_code_start,
              reinterpret_cast<void *> (target_code_point_i), len);
      return;
    }

  char *_target_code_point = reinterpret_cast<char *> (
      reinterpret_cast<intptr_t> (target_code_point) & ~1UL);

  char *start = _target_code_point;
  char *write = static_cast<char *> (trampoline_code_start);
  bool is_thumb = (reinterpret_cast<intptr_t> (target_code_point) & 1) != 0;
  size_t count = b->size / sizeof (post_process_trampoline_desc);
  post_process_trampoline_desc *descs
      = reinterpret_cast<post_process_trampoline_desc *> (b + 1);
  for (size_t i = 0; i < count; ++i)
    {
      char *end = _target_code_point + descs[i].offset;
      memcpy (write, start, reinterpret_cast<int> (end - start));
      write += reinterpret_cast<int> (end - start);
      start = end;
      if (is_thumb)
        {
          handle_thumb_entry (descs[i], start, write);
        }
      else
        {
          handle_arm_entry (descs[i], start, write);
        }
    }

  memcpy (write, start,
          reinterpret_cast<int> (_target_code_point + len - start));
}

bool
arm_target_client::use_target_code_point_as_hint (void)
{
  return false;
}

void
arm_target_client::add_jump_to_original (char *code_start, int offset,
                                         code_context *code_context)
{
  if (reinterpret_cast<intptr_t> (code_context->code_point) & 1)
    {
      uint16_t *data = reinterpret_cast<uint16_t *> (code_start);
      data[0] = 0xf85f;
      offset -= 4;
      offset = -offset;
      // 4 byte align.
      offset &= ~(4UL - 1UL);
      data[1] = ((0xf) << 12) | offset;
    }
  else
    {
      uint32_t *data = reinterpret_cast<uint32_t *> (code_start);
      offset -= 8;
      offset = -offset;
      data[0] = 0xe51ff000 | offset;
    }
}

int
arm_target_client::jump_back_instr_len (code_context *)
{
  return 4;
}
