#pragma once
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long s64;
typedef __SIZE_TYPE__ size_t;                  /* (unsigned int on the pad; the host tests: their own) */
#define NULL ((void *)0)
#define REG(a) (*(volatile u32 *)(a))
#define REG16(a) (*(volatile u16 *)(a))
