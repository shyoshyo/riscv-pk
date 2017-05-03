do_reset():
        regfile = 0x0
        mscratch = 0x0
        mtvec = trap_vector
        check mtvec

        sp = addr of mhartid pages after _end

        init_first_hart():
                hart_init():
                        mstatus_init():
                                // set VM page
                                mstatus.VM = VM_SV32 or VM_SV39
                                
                                // Enable FPU
                                mstatus.FS = 1

                                assert mstatus.VM successfully set

                                // ?
                                mucounteren = -1
                                mscounteren = -1

                                // disable timer; enable other interrupts
                                mie = ~MIP_MTIP

                        fp_init():
                                assert mstatus.FS successfully set

                                misa.'F' = 0 
                                misa.'D' = 0

                                assert misa.'F'/'D' successfully set

                        delegate_traps():
                                mideleg = MIP_SSIP |
                                          MIP_STIP |
                                          MIP_SEIP

                                medeleg = (1U << CAUSE_MISALIGNED_FETCH) |
                                          (1U << CAUSE_FAULT_FETCH) |
                                          (1U << CAUSE_BREAKPOINT) |
                                          (1U << CAUSE_FAULT_LOAD) |
                                          (1U << CAUSE_FAULT_STORE) |
                                          (1U << CAUSE_BREAKPOINT) |
                                          (1U << CAUSE_USER_ECALL);

                                assert mideleg, medeleg successfully set


                        hls_init(0):
                                // this might get called again from parse_config_string
                                
                                memset hart-local storage of this hart

                        parse_config_string();
                                #define DEFAULT_RSTVEC     0x00001000
                                #define DEFAULT_NMIVEC     0x00001004
                                #define DEFAULT_MTVEC      0x00001010
                                #define CONFIG_STRING_ADDR 0x0000100C

                                // get mem, plic, rtc, harts from s = *CONFIG_STRING_ADDR
                                query_mem(s):
                                        base = ram{0{addr
                                        mem_size = ram{0{size

                                        assert base == DRAM_BASE
                                
                                query_plic(s):
                                        nothing

                                query_rtc(s):
                                        mtime = rtc{addr

                                query_harts(s):
                                        if core{%d{%d{ipi exists:
                                                hls = hls_init(num_harts):
                                                        memset hart-local storage

                                                hls->ipi = core{%d{%d{ipi

                                                query_hart_plic:
                                                        nothing
                                                
                                                hls->timecmp = core{%d{%d{timecmp

                                                fence

                        plic_init():
                                nothing for spike

                        hart_plic_init():
                                // clear interrupts

                                *HLS()->ipi = 0;
                                *HLS()->timecmp = -1ULL;
                                mip = 0;

                        //prci_test():
                        //      test mip & HLS()->ipi, HLS()->timecmp

                        memory_init():
                                calc memory & first_free_paddr

                        boot_loader():
                                load_kernel_elf():
                                        load_elf

                                supervisor_vm_init():

                                print_logo():
                                        print logo

                                mb():
                                        fence

                                enter_supervisor_mode((void *)info.entry, 0):
                                        MSTATUS_MPP = PRV_S(uper)
                                        MSTATUS_MPIE = 0

                                        mscratch = machine stack

                                        mepc = entry

                                        sptbr = page table

                                        a0 = stack
                                        sp = stack

                                        mret

