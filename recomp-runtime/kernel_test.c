/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>

int recomp_kernel_device_test(void);
int recomp_kernel_crypto_test(void);
int recomp_kernel_file_test(void);
int recomp_kernel_memory_test(void);
int recomp_kernel_rtl_test(void);
int recomp_kernel_thread_test(void);
int recomp_kernel_video_test(void);
int recomp_flag_macro_test(void);
int recomp_sse_semantics_test(void);

int main(void)
{
    int passed = recomp_kernel_device_test();
    passed &= recomp_kernel_crypto_test();
    passed &= recomp_kernel_file_test();
    passed &= recomp_kernel_memory_test();
    passed &= recomp_kernel_rtl_test();
    passed &= recomp_kernel_thread_test();
    passed &= recomp_kernel_video_test();
    passed &= recomp_flag_macro_test();
    passed &= recomp_sse_semantics_test();
    return passed ? 0 : 1;
}
