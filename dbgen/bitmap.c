#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

#define BITMAP_SIZE (sizeof(unsigned long)*8)
#define BM_J(a) ((a) / BITMAP_SIZE)
#define BM_K(a) ((a) % BITMAP_SIZE)

unsigned long* bitmap_create(int N)
{
	int len = N + 1;
	size_t size = (len+BITMAP_SIZE-1)/BITMAP_SIZE;
	unsigned long* bitmap = (unsigned long*) malloc(size*sizeof(unsigned long));
	memset(bitmap, 0, size*sizeof(unsigned long));
	return (void*) bitmap;
}

int bitmap_test(unsigned long* bitmap, int i)
{
	return !!(bitmap[BM_J(i)] & ((unsigned long)1 << BM_K(i)));
}

void bitmap_set(unsigned long* bitmap, int i)
{
	bitmap[BM_J(i)] = bitmap[BM_J(i)] | ((unsigned long)1 << BM_K(i));
}

void bitmap_unset(unsigned long* bitmap, int i)
{
	bitmap[BM_J(i)] = bitmap[BM_J(i)] & (~((unsigned long)1 << BM_K(i)));
}