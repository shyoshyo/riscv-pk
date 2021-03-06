#include "emulation.h"
#include "fp_emulation.h"
#include "config.h"
#include "unprivileged_memory.h"
#include "mtrap.h"
#include <limits.h>

void illegal_insn_trap(uintptr_t* regs, uintptr_t mcause, uintptr_t mepc)
{
  asm (".pushsection .rodata\n"
       "illegal_insn_trap_table:\n"
       "  .word truly_illegal_insn\n"
#if !defined(__riscv_flen) && defined(PK_ENABLE_FP_EMULATION)
       "  .word emulate_float_load\n"
#else
       "  .word truly_illegal_insn\n"
#endif
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
#if !defined(__riscv_flen) && defined(PK_ENABLE_FP_EMULATION)
       "  .word emulate_float_store\n"
#else
       "  .word truly_illegal_insn\n"
#endif
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
#if !defined(__riscv_muldiv)
       "  .word emulate_mul_div\n"
#else
       "  .word truly_illegal_insn\n"
#endif
       "  .word truly_illegal_insn\n"
#if !defined(__riscv_muldiv) && __riscv_xlen >= 64
       "  .word emulate_mul_div32\n"
#else
       "  .word truly_illegal_insn\n"
#endif
       "  .word truly_illegal_insn\n"
#ifdef PK_ENABLE_FP_EMULATION
       "  .word emulate_fmadd\n"
       "  .word emulate_fmadd\n"
       "  .word emulate_fmadd\n"
       "  .word emulate_fmadd\n"
       "  .word emulate_fp\n"
#else
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
#endif
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word emulate_system_opcode\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .word truly_illegal_insn\n"
       "  .popsection");

  uintptr_t mstatus;
  insn_t insn = get_insn(mepc, &mstatus);

  // log("insn = %x, mepc = %x, mstatus = %x", insn, mepc, mstatus);
  
  if (unlikely((insn & 3) != 3))
    return truly_illegal_insn(regs, mcause, mepc, mstatus, insn);

  write_csr(mepc, mepc + 4);

  extern uint32_t illegal_insn_trap_table[];
  uint32_t* pf = (void*)illegal_insn_trap_table + (insn & 0x7c);
  emulation_func f = (emulation_func)(uintptr_t)*pf;
  f(regs, mcause, mepc, mstatus, insn);
}

void tlb_miss_trap(uintptr_t* regs, uintptr_t mcause, uintptr_t mepc, int ex, int rd, int wt)
{
  static uint32_t index = 0;

  uint32_t levels, ptesize, vpnlen;

  uintptr_t mstatus = read_csr(mstatus);
  uint32_t vm = (EXTRACT_FIELD(mstatus, MSTATUS_VM));

#if __riscv_xlen == 32
  uint32_t p = 32;
  uintptr_t a = ((read_csr(sptbr)) & ((1 << 22) - 1)) * RISCV_PGSIZE;
#else
  uint32_t p = 64;
  uintptr_t a = ((read_csr(sptbr)) & ((1ll << 38) - 1)) * RISCV_PGSIZE;
#endif

  switch(vm)
  {
    case VM_SV32: levels = 2; ptesize = 4; vpnlen = 10; break;
    case VM_SV39: levels = 3; ptesize = 8; vpnlen = 9; break;
    case VM_SV48: levels = 4; ptesize = 8; vpnlen = 9; break;
    default: die("unsupport mstatus.vm = %x", vm);
  }

  uintptr_t mask = 0;
  uintptr_t va = read_csr(mbadaddr);

  // log("tlb_miss_trap, mepc = %p, mbadaddr = %p, ex %d, rd %d, wt %d",
  //    mepc, va, ex, rd, wt);

  int mxr = (EXTRACT_FIELD(mstatus, MSTATUS_MXR));
  int i;  
  for(i = levels - 1; ; i--)
  {
    if(!(i >= 0)) goto fail;
    // log("tlb_miss_trap, mepc = %p, mbadaddr = %p, ex %d, rd %d, wt %d, i level = %d",
    ///    mepc, va, ex, rd, wt, i);


    p -= vpnlen;
    mask = ~((~mask) >> vpnlen);

    uintptr_t vpn = ((va >> p) & ((1 << vpnlen) - 1));
    uintptr_t *pte_p = (uintptr_t *)(a + vpn * ptesize);
    uintptr_t pte = *pte_p;

    if(!((pte & PTE_V) != 0)) goto fail;
    if(!(((pte & PTE_R) == 0 && (pte & PTE_W) != 0) == 0)) goto fail;

    if ((pte & (PTE_X | PTE_W | PTE_R)) == 0)
    {
      a = (pte >> 10) << RISCV_PGSHIFT;
    }
    else
    {
      if(ex == 1) { if(!(pte & PTE_X)) goto fail; }
      // TODO: mxr
      if(rd == 1) { if(!(pte & (PTE_R | PTE_X))) goto fail; }
      if(wt == 1) { if(!(pte & PTE_W)) goto fail; }

      pte |= PTE_A;
      if(wt) pte |= PTE_D;
      *pte_p = pte;

      // need update
      if(((uintptr_t)read_csr(0x7c0)) >> (__riscv_xlen - 1))
      {
        // log("tlb_miss_trap: update, mepc = %p, mbadaddr = %p, ex %d, rd %d, wt %d",
        //   mepc, va, ex, rd, wt);

        uintptr_t index_old = read_csr(0x7c0);
        uintptr_t va_old = read_csr(0x7c1);
        uintptr_t mask_old = read_csr(0x7c2);
        uintptr_t pte_old = read_csr(0x7c3);
        uintptr_t *pte_p_old = (uintptr_t *)read_csr(0x7c4);

        assert(index_old >> (__riscv_xlen - 1));
        assert((va & mask) == (va_old & mask_old));
        assert(mask == mask_old);
        assert((pte | PTE_D) == (pte_old | PTE_A | PTE_D));
        assert(pte_p == pte_p_old);

        *pte_p = pte;
        write_csr(0x7c3, pte);

        write_csr(0x7c0, index_old);
        assert(read_csr(0x7c0) == (index_old << 1) >> 1);
        return;
      }

      write_csr(0x7c0, index);
      write_csr(0x7c1, va & mask);
      write_csr(0x7c2, mask);
      write_csr(0x7c3, pte);
      write_csr(0x7c4, pte_p);

      index += 1;
      return;
    }
  }

fail:
  
  write_csr(sbadaddr, read_csr(mbadaddr));
  redirect_trap(mepc, mstatus);
  __builtin_unreachable();
}

void tlb_i_miss_trap(uintptr_t* regs, uintptr_t mcause, uintptr_t mepc)
{
  tlb_miss_trap(regs, mcause, mepc, 1, 0, 0);
}
void tlb_r_miss_trap(uintptr_t* regs, uintptr_t mcause, uintptr_t mepc)
{
  tlb_miss_trap(regs, mcause, mepc, 0, 1, 0);
}
void tlb_w_miss_trap(uintptr_t* regs, uintptr_t mcause, uintptr_t mepc)
{
  tlb_miss_trap(regs, mcause, mepc, 0, 0, 1);
}

void tlb_flush()
{
  // log("tlb_flush()");

  int i;
  for(i = 0; ; i++)
  {
    write_csr(0x7c0, i);
    if(read_csr(0x7c0) != i) break;
    
    write_csr(0x7c3, 0x0);
  }
}

void __attribute__((noinline)) truly_illegal_insn(uintptr_t* regs, uintptr_t mcause, uintptr_t mepc, uintptr_t mstatus, insn_t insn)
{
  redirect_trap(mepc, mstatus);
}

static inline int emulate_read_csr(int num, uintptr_t mstatus, uintptr_t* result)
{
  uintptr_t counteren =
    EXTRACT_FIELD(mstatus, MSTATUS_MPP) == PRV_U ? read_csr(mucounteren) :
                                                   read_csr(mscounteren);

  // log("emulate_read_csr = %x, CSR_TIME[H] = %x, %x", num, CSR_TIME, CSR_TIMEH);

  switch (num)
  {
    case CSR_TIME:
      if (!((counteren >> (CSR_TIME - CSR_CYCLE)) & 1))
        return -1;
      *result = *mtime;
      return 0;
#if __riscv_xlen == 32
    case CSR_TIMEH:
      if (!((counteren >> (CSR_TIME - CSR_CYCLE)) & 1))
        return -1;
      *result = *mtime >> 32;
      return 0;
#endif
#if !defined(__riscv_flen) && defined(PK_ENABLE_FP_EMULATION)
    case CSR_FRM:
      if ((mstatus & MSTATUS_FS) == 0) break;
      *result = GET_FRM();
      return 0;
    case CSR_FFLAGS:
      if ((mstatus & MSTATUS_FS) == 0) break;
      *result = GET_FFLAGS();
      return 0;
    case CSR_FCSR:
      if ((mstatus & MSTATUS_FS) == 0) break;
      *result = GET_FCSR();
      return 0;
#endif
  }
  return -1;
}

static inline int emulate_write_csr(int num, uintptr_t value, uintptr_t mstatus)
{
  switch (num)
  {
#if !defined(__riscv_flen) && defined(PK_ENABLE_FP_EMULATION)
    case CSR_FRM: SET_FRM(value); return 0;
    case CSR_FFLAGS: SET_FFLAGS(value); return 0;
    case CSR_FCSR: SET_FCSR(value); return 0;
#endif
  }
  return -1;
}

DECLARE_EMULATION_FUNC(emulate_system_opcode)
{
  int rs1_num = (insn >> 15) & 0x1f;
  uintptr_t rs1_val = GET_RS1(insn, regs);
  int csr_num = (uint32_t)insn >> 20;
  uintptr_t csr_val, new_csr_val;

  if((insn & 0xFFF07FFF) == 0x10400073)
  {
    tlb_flush();
    return;
  }

  if (emulate_read_csr(csr_num, mstatus, &csr_val))
    return truly_illegal_insn(regs, mcause, mepc, mstatus, insn);

  int do_write = rs1_num;
  switch (GET_RM(insn))
  {
    case 0: return truly_illegal_insn(regs, mcause, mepc, mstatus, insn);
    case 1: new_csr_val = rs1_val; do_write = 1; break;
    case 2: new_csr_val = csr_val | rs1_val; break;
    case 3: new_csr_val = csr_val & ~rs1_val; break;
    case 4: return truly_illegal_insn(regs, mcause, mepc, mstatus, insn);
    case 5: new_csr_val = rs1_num; do_write = 1; break;
    case 6: new_csr_val = csr_val | rs1_num; break;
    case 7: new_csr_val = csr_val & ~rs1_num; break;
  }

  if (do_write && emulate_write_csr(csr_num, new_csr_val, mstatus))
    return truly_illegal_insn(regs, mcause, mepc, mstatus, insn);

  SET_RD(insn, regs, csr_val);
}
