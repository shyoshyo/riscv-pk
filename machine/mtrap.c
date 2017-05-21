#include "mtrap.h"
#include "mcall.h"
#include "htif.h"
#include "atomic.h"
#include "bits.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

uintptr_t uart;

static inline uint8_t
inb(uintptr_t port) {
    uint8_t data = *((volatile uint8_t*) port);
    return data;
}

static inline void
outb(uintptr_t port, uint8_t data) {
    *((volatile uint8_t*) port) = data;
}

static inline uint32_t
inw(uintptr_t port) {
    uint32_t data = *((volatile uintptr_t *) port);
    return data;
}

static inline void
outw(uintptr_t port, uint32_t data) {
    *((volatile uintptr_t *) port) = data;
}

void
fpga_uart_init(uint16_t divclk)
{
    // set clk divsor
    outb(uart + 3, 0x80);

    // 32'd55; <=> wishbone clk 100M         115200
    // 32'd27; <=> wishbone clk 50M,         115200
    // 32'd22; <=> wishbone clk 40M,         115200
    // 32'd14; <=> wishbone clk 25M,         115200
    outb(uart + 1, (uint8_t)(divclk >> 8));
    outb(uart + 0, (uint8_t)divclk);

    // 8 bit, no chk, no parity, 1 stop
    outb(uart + 3, 0x03);
    
    // set interrupt
    outb(uart + 1, 0x01);
}

void
fpga_uart_putchar_sub(uint8_t ch)
{
    while((inb(uart + 5) & 0x20) == 0);
    outb(uart + 0, ch & 0xFF);
}

void
fpga_uart_putchar(uint8_t c) {
    if (c == '\b') {
        fpga_uart_putchar_sub('\b');
        fpga_uart_putchar_sub(' ');
        fpga_uart_putchar_sub('\b');
    } else if(c == '\n') {
        fpga_uart_putchar_sub('\r');
        fpga_uart_putchar_sub('\n');
    } else {
        fpga_uart_putchar_sub(c);
    }
}

static uint8_t
fpga_uart_getchar(void) {
    static int last_is_CR = 0;
    uint8_t c;

    for(;;)
    {
      if( (inb(uart + 5) & 0x01) == 0)
        return 0xffu;
      c = inb(uart + 0) & 0xFF;

      if (c == 127)
      {
          c = '\b';
      }

      if(c == '\r')
      {
          c = '\n';
          last_is_CR = 1;
      }
      else
      {
          if(last_is_CR && c == '\n')
          {
              last_is_CR = 0;
              continue;
          }
          last_is_CR = 0;
      }

      return c;
    }
}

volatile uint64_t tohost __attribute__((aligned(64))) __attribute__((section("htif")));
volatile uint64_t fromhost __attribute__((aligned(64))) __attribute__((section("htif")));
static spinlock_t htif_lock = SPINLOCK_INIT;

void __attribute__((noreturn)) bad_trap()
{
  die("machine mode: unhandlable trap %x @ %p", read_csr(mcause), read_csr(mepc));
}

static uintptr_t mcall_hart_id()
{
  return read_const_csr(mhartid);
}

static void request_htif_keyboard_interrupt()
{
  // assert(tohost == 0);
  tohost = TOHOST_CMD(1, 0, 0);
}

static void __htif_interrupt()
{
  // we should only be interrupted by keypresses
  uint64_t fh = fromhost;
  if (!fh)
    return;
  if (!(FROMHOST_DEV(fh) == 1 && FROMHOST_CMD(fh) == 0))
    die("unexpected htif interrupt");
  HLS()->console_ibuf = 1 + (uint8_t)FROMHOST_DATA(fh);
  fromhost = 0;
  set_csr(mip, MIP_SSIP);
}

static void do_tohost_fromhost(uintptr_t dev, uintptr_t cmd, uintptr_t data)
{
  spinlock_lock(&htif_lock);
    while (tohost)
      __htif_interrupt();
    tohost = TOHOST_CMD(dev, cmd, data);

    while (1) {
      uint64_t fh = fromhost;
      if (fh) {
        if (FROMHOST_DEV(fh) == dev && FROMHOST_CMD(fh) == cmd) {
          fromhost = 0;
          break;
        }
        __htif_interrupt();
      }
    }
  spinlock_unlock(&htif_lock);
}

static void htif_interrupt()
{
  if (spinlock_trylock(&htif_lock) == 0) {
    __htif_interrupt();
    spinlock_unlock(&htif_lock);
  }
}

uintptr_t timer_interrupt()
{
  // just send the timer interrupt to the supervisor
  clear_csr(mie, MIP_MTIP);
  set_csr(mip, MIP_STIP);

  // and poll the HTIF console
  htif_interrupt();

  if(uart != 0)
  {
    uint8_t c;
    c = fpga_uart_getchar();
    if(c != 0xffu)
    {
      HLS()->console_ibuf = 1 + c;

      set_csr(mip, MIP_SSIP);
    }
  }
  return 0;
}

static uintptr_t mcall_console_putchar(uint8_t ch)
{
  if(uart == 0)
  {
    do_tohost_fromhost(1, 1, ch);
  }
  else
  {
    fpga_uart_putchar(ch);
  }
  return 0;
}

static uintptr_t mcall_htif_syscall(uintptr_t magic_mem)
{
  do_tohost_fromhost(0, 0, magic_mem);
  return 0;
}

void poweroff()
{
  log("machine mode: System shutdown!");
  while (1)
    tohost = 1;
}

void putstring(const char* s)
{
  while (*s)
    mcall_console_putchar(*s++);
}

void printm(const char* s, ...)
{
  char buf[4096];
  va_list vl;

  va_start(vl, s);
  vsnprintf(buf, sizeof buf, s, vl);
  va_end(vl);

  putstring(buf);
}

static void send_ipi(uintptr_t recipient, int event)
{
  if ((atomic_or(&OTHER_HLS(recipient)->mipi_pending, event) & event) == 0) {
    mb();
    *OTHER_HLS(recipient)->ipi = 1;
  }
}

static uintptr_t mcall_send_ipi(uintptr_t recipient)
{
  if (recipient >= num_harts)
    return -1;

  send_ipi(recipient, IPI_SOFT);
  return 0;
}

static void reset_ssip()
{
  clear_csr(mip, MIP_SSIP);
  mb();

  if (HLS()->sipi_pending || HLS()->console_ibuf > 0)
    set_csr(mip, MIP_SSIP);
}

static uintptr_t mcall_console_getchar()
{
  int ch = atomic_swap(&HLS()->console_ibuf, -1);
  if(uart == 0)
  {
    if (ch >= 0)
      request_htif_keyboard_interrupt();
  }
  reset_ssip();
  return ch - 1;
}

static uintptr_t mcall_clear_ipi()
{
  int ipi = atomic_swap(&HLS()->sipi_pending, 0);
  reset_ssip();
  return ipi;
}

static uintptr_t mcall_shutdown()
{
  poweroff();
}

static uintptr_t mcall_set_timer(uint64_t when)
{
  *HLS()->timecmp = when;
  clear_csr(mip, MIP_STIP);
  set_csr(mie, MIP_MTIP);
  return 0;
}

void software_interrupt()
{
  *HLS()->ipi = 0;
  mb();
  int ipi_pending = atomic_swap(&HLS()->mipi_pending, 0);

  if (ipi_pending & IPI_SOFT) {
    HLS()->sipi_pending = 1;
    set_csr(mip, MIP_SSIP);
  }

  if (ipi_pending & IPI_FENCE_I)
    asm volatile ("fence.i");

  if (ipi_pending & IPI_SFENCE_VM)
    asm volatile ("sfence.vm");
}

static void send_ipi_many(uintptr_t* pmask, int event)
{
  _Static_assert(MAX_HARTS <= 8 * sizeof(*pmask), "# harts > uintptr_t bits");
  uintptr_t mask = -1;
  if (pmask)
    mask = *pmask;

  // send IPIs to everyone
  for (ssize_t i = num_harts-1; i >= 0; i--)
    if ((mask >> i) & 1)
      send_ipi(i, event);

  // wait until all events have been handled.
  // prevent deadlock while spinning by handling any IPIs from other harts.
  for (ssize_t i = num_harts-1; i >= 0; i--)
    if ((mask >> i) & 1)
      while (OTHER_HLS(i)->mipi_pending & event)
        software_interrupt();
}

static uintptr_t mcall_remote_sfence_vm(uintptr_t* hart_mask, uintptr_t asid)
{
  // ignore the ASID and do a global flush.
  // this allows us to avoid queueing a message.
  send_ipi_many(hart_mask, IPI_SFENCE_VM);
  return 0;
}

static uintptr_t mcall_remote_fence_i(uintptr_t* hart_mask)
{
  send_ipi_many(hart_mask, IPI_FENCE_I);
  return 0;
}

void redirect_trap(uintptr_t epc, uintptr_t mstatus)
{
  write_csr(sepc, epc);
  write_csr(scause, read_csr(mcause));
  write_csr(mepc, read_csr(stvec));

  uintptr_t new_mstatus = mstatus & ~(MSTATUS_SPP | MSTATUS_SPIE | MSTATUS_MPIE);
  uintptr_t mpp_s = MSTATUS_MPP & (MSTATUS_MPP >> 1);
  new_mstatus |= (mstatus / (MSTATUS_MPIE / MSTATUS_SPIE)) & MSTATUS_SPIE;
  new_mstatus |= (mstatus / (mpp_s / MSTATUS_SPP)) & MSTATUS_SPP;
  new_mstatus |= mpp_s;
  write_csr(mstatus, new_mstatus);

  extern void __redirect_trap();
  return __redirect_trap();
}

void mcall_trap(uintptr_t* regs, uintptr_t mcause, uintptr_t mepc)
{
  uintptr_t n = regs[17], arg0 = regs[10], arg1 = regs[11], retval;
  switch (n)
  {
    case MCALL_HART_ID:
      retval = mcall_hart_id();
      break;
    case MCALL_CONSOLE_PUTCHAR:
      retval = mcall_console_putchar(arg0);
      break;
    case MCALL_CONSOLE_GETCHAR:
      retval = mcall_console_getchar();
      break;
    case MCALL_HTIF_SYSCALL:
      retval = mcall_htif_syscall(arg0);
      break;
    case MCALL_SEND_IPI:
      retval = mcall_send_ipi(arg0);
      break;
    case MCALL_CLEAR_IPI:
      retval = mcall_clear_ipi();
      break;
    case MCALL_SHUTDOWN:
      retval = mcall_shutdown();
      break;
    case MCALL_SET_TIMER:
#if __riscv_xlen == 32
      retval = mcall_set_timer(arg0 + ((uint64_t)arg1 << 32));
#else
      retval = mcall_set_timer(arg0);
#endif
      break;
    case MCALL_REMOTE_SFENCE_VM:
      retval = mcall_remote_sfence_vm((uintptr_t*)arg0, arg1);
      break;
    case MCALL_REMOTE_FENCE_I:
      retval = mcall_remote_fence_i((uintptr_t*)arg0);
      break;
    default:
      redirect_trap(read_csr(mepc), read_csr(mstatus));
      retval = -ENOSYS;
      break;
  }
  regs[10] = retval;
  write_csr(mepc, mepc + 4);
}

static void machine_page_fault(uintptr_t* regs, uintptr_t mepc)
{
  // MPRV=1 iff this trap occurred while emulating an instruction on behalf
  // of a lower privilege level. In that case, a2=epc and a3=mstatus.
  if (read_csr(mstatus) & MSTATUS_MPRV) {
    write_csr(sbadaddr, read_csr(mbadaddr));
    return redirect_trap(regs[12], regs[13]);
  }
  bad_trap();
}

void trap_from_machine_mode(uintptr_t* regs, uintptr_t dummy, uintptr_t mepc)
{
  uintptr_t mcause = read_csr(mcause);

  extern void *misaligned_load_trap(uintptr_t*, uintptr_t, uintptr_t);
  extern void *misaligned_store_trap(uintptr_t*, uintptr_t, uintptr_t);

  switch (mcause)
  {
    case CAUSE_MISALIGNED_LOAD:
      misaligned_load_trap(regs, mcause, mepc);
      break;

    case CAUSE_MISALIGNED_STORE:
      misaligned_store_trap(regs, mcause, mepc);
      break;

    case CAUSE_FAULT_LOAD:
    case CAUSE_FAULT_STORE:
      return machine_page_fault(regs, mepc);

    case IRQ_M_TIMER | (~((unsigned)(~0) >> 1u)):
      printm("100 ticks!!\n");
      *HLS()->timecmp = *mtime + 0x100000;
      break;

    default:
      bad_trap();
  }
}
