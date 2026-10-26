/* cortex-m7 startup for the qemu mps2-an500 reference of the whole firmware.
 * enables the fpu, hands over to newlib's semihosting crt0 (_start: bss, data,
 * constructors including the board init, then the firmware's own main()).
 * faults print their registers through semihosting and exit, so a broken run
 * fails loudly instead of hanging. SPDX-License-Identifier: GPL-3.0-only */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "stm32f7xx.h"

extern uint32_t __StackTop;
extern void _start(void);

void lxr_reset(void)
{
    SCB->CPACR |= (0xFu << 20);     /* cp10/cp11 full access */
    __DSB(); __ISB();
    _start();
    for (;;) {}
}

__attribute__((used)) void lxr_fault_c(uint32_t *sp, uint32_t exc)
{
    printf("FAULT exc %lu pc %08lx lr %08lx cfsr %08lx hfsr %08lx mmfar %08lx bfar %08lx\n",
           (unsigned long)exc, (unsigned long)sp[6], (unsigned long)sp[5], (unsigned long)SCB->CFSR,
           (unsigned long)SCB->HFSR, (unsigned long)SCB->MMFAR, (unsigned long)SCB->BFAR);
    exit(3);
}
__attribute__((naked)) static void lxr_fault(void)
{
    __asm volatile ("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n mrs r1, ipsr\n b lxr_fault_c\n");
}
static void lxr_unexpected(void) { printf("unexpected interrupt ipsr %lu\n", (unsigned long)__get_IPSR()); exit(4); }

__attribute__((section(".isr_vector"), used))
const void *const lxr_vectors[16 + 16] = {
    &__StackTop, lxr_reset, lxr_fault, lxr_fault, lxr_fault, lxr_fault, lxr_fault, 0, 0, 0, 0,
    lxr_unexpected, lxr_unexpected, 0, lxr_unexpected, lxr_unexpected,
    lxr_unexpected, lxr_unexpected, lxr_unexpected, lxr_unexpected, lxr_unexpected, lxr_unexpected, lxr_unexpected, lxr_unexpected,
    lxr_unexpected, lxr_unexpected, lxr_unexpected, lxr_unexpected, lxr_unexpected, lxr_unexpected, lxr_unexpected, lxr_unexpected,
};
