#include <common.h>

#include <pspkernel.h>

#define OP1(TYPE) int __attribute__((noinline)) op_##TYPE(int x       ) { int result; asm volatile(#TYPE " %0, %1" : "=r"(result) : "r"(x)); return result; }
#define OP2(TYPE) int __attribute__((noinline)) op_##TYPE(int x, int y) { int result; asm volatile(#TYPE " %0, %1, %2" : "=r"(result) : "r"(x), "r"(y)); return result; }

#define OPX_TEST_START() printf("--\n");
#define OP1_TEST_X(TYPE, a) printf("%s 0x%08X -> %d\n", #TYPE, a, op_##TYPE(a));
#define OP1_TEST(TYPE, a) printf("%s %d -> %d\n", #TYPE, a, op_##TYPE(a));
#define OP2_TEST(TYPE, a, b) printf("%s %d, %d -> %d\n", #TYPE, a, b, op_##TYPE(a, b));

#define OP2_TEST_SET(TYPE) \
	OPX_TEST_START(); \
	OP2_TEST(TYPE, 1, 7); \
	OP2_TEST(TYPE, 3, 1); \
	OP2_TEST(TYPE, -1, 0); \
	OP2_TEST(TYPE, -1, -10); \
	OP2_TEST(TYPE, 0, 0); \

__attribute__ ((noinline)) unsigned int fixed_ror(unsigned int value) {
	int ret;
	asm volatile (
		"ror %0, %1, 4\n"
		: "=r"(ret) : "r"(value)
	);
	return ret;
}

__attribute__ ((noinline)) unsigned int fixed_rorv(unsigned int value, int offset) {
	int ret;
	asm volatile (
		"rorv %0, %1, %2\n"
		: "=r"(ret) : "r"(value), "r"(offset)
	);
	return ret;
}

__attribute__ ((noinline)) unsigned int test_bitrev(unsigned int value) {
	int ret;
	asm volatile (
		"bitrev %0, %1\n"
		: "=r"(ret) : "r"(value)
	);
	return ret;
}

void test_mul64() {
  volatile unsigned long long a = 0x8234567812345678ULL;
  volatile unsigned long long b = 0x2345678123456783ULL;
  volatile signed long long c = 0x8234567812345678ULL;
  volatile signed long long d = 0xF234567812345678ULL;
  printf("%llu\n", a * b);
  printf("%llu\n", b * c - 1);
  printf("%llu\n", (c * d) >> 7);
}

void test_div() {
  volatile int a = 1 << 31; 
  volatile int b = -1;
  volatile int c = 100;
  volatile int d = 3;
  printf("%08x\n", a/b);
  printf("%08x\n", c/d); 
}

OP2(max)
OP2(min)

OP2(add)
OP2(addu)
OP2(sub)
OP2(subu)

OP1(clo)
OP1(clz)

int main(int argc, char *argv[]) {
	OP2_TEST_SET(max);
	OP2_TEST_SET(min);
	OP2_TEST_SET(add);
	OP2_TEST_SET(addu);
	OP2_TEST_SET(sub);
	OP2_TEST_SET(subu);
	
	OPX_TEST_START();
	OP1_TEST_X(clo, 0x00000000)
	OP1_TEST_X(clo, 0x80000000)
	OP1_TEST_X(clo, 0xF0000000)
	OP1_TEST_X(clo, 0xF000000F)
	OP1_TEST_X(clo, 0xFFFF000F)
	OP1_TEST_X(clo, 0xF7FF000F)
	OP1_TEST_X(clo, 0xFFFFFFFF)

	OPX_TEST_START();
	OP1_TEST_X(clz, 0x00000000)
	OP1_TEST_X(clz, 0x0000000F)
	OP1_TEST_X(clz, 0xF000000F)
	OP1_TEST_X(clz, 0x0000007F)
	OP1_TEST_X(clz, 0xFFFFFFFF)

	OPX_TEST_START();
	printf("rotr 0x%08X\n", fixed_ror(0x12345678));
	printf("rotrv 0x%08X\n", fixed_rorv(0x12345678, 8));

	OPX_TEST_START();
  test_mul64();
  test_div();

	OPX_TEST_START();
  printf("bitrev 0x%08x\n", test_bitrev(0xF18abcde));
	return 0;
}
