#include "mtrap.h"
#include "atomic.h"
#include "vm.h"
#include "fp_emulation.h"
#include <string.h>
#include <limits.h>

pte_t* root_page_table;
uintptr_t first_free_paddr;
uintptr_t mem_size;
uintptr_t num_harts;
volatile uint64_t* mtime;
volatile uint32_t* plic_priorities;
size_t plic_ndevs;

static void mstatus_init()
{
  // Enable FPU and set VM mode
  uintptr_t ms = 0;
  ms = INSERT_FIELD(ms, MSTATUS_VM, VM_CHOICE);
  ms = INSERT_FIELD(ms, MSTATUS_FS, 1);
  write_csr(mstatus, ms);

  // Make sure the hart actually supports the VM mode we want
  ms = read_csr(mstatus);
  assert(EXTRACT_FIELD(ms, MSTATUS_VM) == VM_CHOICE);

  // Enable user/supervisor use of perf counters
  write_csr(mucounteren, -1);
  write_csr(mscounteren, -1);
  write_csr(mie, ~MIP_MTIP); // disable timer; enable other interrupts
}

// send S-mode interrupts and most exceptions straight to S-mode
static void delegate_traps()
{
  uintptr_t interrupts = MIP_SSIP | MIP_STIP | MIP_SEIP;
  uintptr_t exceptions =
    (1U << CAUSE_MISALIGNED_FETCH) |
    // (1U << CAUSE_FAULT_FETCH) |
    (1U << CAUSE_BREAKPOINT) |
    // (1U << CAUSE_FAULT_LOAD) |
    // (1U << CAUSE_FAULT_STORE) |
    (1U << CAUSE_BREAKPOINT) |
    (1U << CAUSE_USER_ECALL);

  write_csr(mideleg, interrupts);
  write_csr(medeleg, exceptions);
  assert(read_csr(mideleg) == interrupts);
  assert(read_csr(medeleg) == exceptions);
}

static void fp_init()
{
  assert(read_csr(mstatus) & MSTATUS_FS);

#ifdef __riscv_flen
  if (supports_extension('D'))
  {
    for (int i = 0; i < 32; i++)
      init_fp_reg(i);
    write_csr(fcsr, 0);
    return;
  }
#endif
  // log("FPU not found; recompile pk with -msoft-float");
  uintptr_t fd_mask = (1 << ('F' - 'A')) | (1 << ('D' - 'A'));
  clear_csr(misa, fd_mask);
  assert(!(read_csr(misa) & fd_mask));
}

hls_t* hls_init(uintptr_t id)
{
  hls_t* hls = OTHER_HLS(id);
  memset(hls, 0, sizeof(*hls));
  //hls->console_ibuf = -1;
  return hls;
}

static uintptr_t sbi_top_paddr()
{
  extern char _end;
  return ROUNDUP((uintptr_t)&_end, RISCV_PGSIZE);
}

static void memory_init()
{
  mem_size = mem_size / MEGAPAGE_SIZE * MEGAPAGE_SIZE;
  first_free_paddr = sbi_top_paddr() + num_harts * RISCV_PGSIZE;
}

static void memory_check()
{
  
}

static void hart_init()
{
  num_harts = 0;
  mstatus_init();
  fp_init();
  delegate_traps();
}

static void plic_init()
{
  for (size_t i = 1; i <= plic_ndevs; i++)
    plic_priorities[i] = 1;
}

static void prci_test()
{
  assert(!(read_csr(mip) & MIP_MSIP));
  *HLS()->ipi = 1;
  assert(read_csr(mip) & MIP_MSIP);
  *HLS()->ipi = 0;

  assert(!(read_csr(mip) & MIP_MTIP));
  *HLS()->timecmp = 0;
  assert(read_csr(mip) & MIP_MTIP);
  *HLS()->timecmp = -1ULL;
}

static void hart_plic_init()
{
  // clear pending interrupts
  *HLS()->ipi = 0;
  *HLS()->timecmp = -1ULL;
  write_csr(mip, 0);

  if (!plic_ndevs)
    return;

  die("not implemented");

  size_t ie_words = plic_ndevs / sizeof(uintptr_t) + 1;
  for (size_t i = 0; i < ie_words; i++)
    HLS()->plic_s_ie[i] = ULONG_MAX;
  *HLS()->plic_m_thresh = 1;
  *HLS()->plic_s_thresh = 0;
}

static void unaligned_r_w_test()
{
  // test unaligned r/w through mtrap
  
  uint32_t s[2] = {0x12345678, 0x9abcdef0};

  uint32_t value = 123;
  asm volatile("lw %0, (%1)" : "=r"(value) : "r"((uintptr_t)s + 1));
  if(value != 0xf0123456)
    log("value = %x\n", value);
  assert(value == 0xf0123456);
  
  value = 0x87654321;
  asm volatile("sw %0, (%1)" : :"r"(value), "r"((uintptr_t)s + 3) : "memory");
  
  if(s[0] != 0x21345678)
    log("s[0] = %x\n", s[0]);
  assert(s[0] == 0x21345678);
  
  if(s[1] != 0x9a876543)
    log("s[1] = %x\n", s[1]);
  assert(s[1] == 0x9a876543);
}

void clock_test()
{
  *HLS()->timecmp = *mtime + 0x100000;
  clear_csr(mip, MIP_STIP);
  set_csr(mie, MIP_MTIP);
  set_csr(mstatus, MSTATUS_MIE);
  
  for(;;);

  __builtin_unreachable();
}

void init_first_hart()
{
  hart_init();
  hls_init(0); // this might get called again from parse_config_string
  parse_config_string();
  plic_init();
  hart_plic_init();
  prci_test();
  unaligned_r_w_test();
  memory_init();
  memory_check();
#if 0
  clock_test();
#endif
  boot_loader();
}

void init_other_hart()
{
  hart_init();
  hart_plic_init();
  boot_other_hart();
}

void enter_supervisor_mode(void (*fn)(uintptr_t), uintptr_t stack)
{
  uintptr_t mstatus = read_csr(mstatus);
  mstatus = INSERT_FIELD(mstatus, MSTATUS_MPP, PRV_S);
  mstatus = INSERT_FIELD(mstatus, MSTATUS_MPIE, 0);
  write_csr(mstatus, mstatus);
  write_csr(mscratch, MACHINE_STACK_TOP() - MENTRY_FRAME_SIZE);
  write_csr(mepc, fn);
  write_csr(sptbr, (uintptr_t)root_page_table >> RISCV_PGSHIFT);

  asm volatile ("mv a0, %0; mv sp, %0; mret" : : "r" (stack));
  __builtin_unreachable();
}
