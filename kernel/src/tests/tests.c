#include <krnl/tests/tests.h>

#include <krnl/mem/allocator.h>
#include <krnl/mem/vmm.h>

#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stdbool.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/debug/debug.h>

#define TEST_ASSERT(x, msg, ...) \
    if (!(x)) { kprintf("TEST FAILED: " msg "\n", ##__VA_ARGS__); panic("KMALLOC TEST FAILURE\n"); }

//static uint64_t align_up(uint64_t x, uint64_t a) {
//    return (x + (a-1)) & ~(a-1);
//}

static uint16_t lfsr_state = TEST_RAND_SEED;

void srand16(uint16_t seed) {
    if (seed == 0) seed = TEST_RAND_SEED;   // LFSR cannot start at 0
    lfsr_state = seed;
}

uint16_t rand16(void) {
    uint16_t bit =
        ((lfsr_state >> 0) ^
         (lfsr_state >> 2) ^
         (lfsr_state >> 3) ^
         (lfsr_state >> 5)) & 1;

    lfsr_state = (lfsr_state >> 1) | (bit << 15);

    return lfsr_state;
}

int rand(void) {
    return (uint32_t)rand16();
}

static void test_basic_alloc() {
    kprintf("[KMALLOC TEST] Basic allocation\n");

    void *a = kmalloc(32);
    TEST_ASSERT(a != NULL, "kmalloc returned NULL for 32 bytes");

    // alignment test (assume 16B or 8B alignment)
    TEST_ASSERT(((uint64_t)a % 8) == 0, "allocation not aligned");

    kfree(a);
}

static void test_zero_alloc() {
    kprintf("[KMALLOC TEST] Zero-size\n");

    expect_panic(1);
    void *a = kmalloc(0);
    (void)a;
    TEST_ASSERT(check_and_clear_panicked(), "kmalloc(0) did not panic as expected");
    expect_panic(0);
}

static void test_multiple_allocs() {
    kprintf("[KMALLOC TEST] Multiple allocations\n");

    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kmalloc(256);

    TEST_ASSERT(a && b && c, "multiple allocations failed");

    // write patterns
    memset(a, 0xAA, 64);
    memset(b, 0xBB, 128);
    memset(c, 0xCC, 256);

    // verify patterns
    for (int i = 0; i < 64; i++)  TEST_ASSERT(((uint8_t*)a)[i] == 0xAA, "corruption in block a");
    for (int i = 0; i < 128; i++) TEST_ASSERT(((uint8_t*)b)[i] == 0xBB, "corruption in block b");
    for (int i = 0; i < 256; i++) TEST_ASSERT(((uint8_t*)c)[i] == 0xCC, "corruption in block c");

    kfree(a);
    kfree(b);
    kfree(c);
}

static void test_reuse() {
    kprintf("[KMALLOC TEST] Free & reuse\n");

    void *a = kmalloc(200);
    memset(a, 0x11, 200);
    kfree(a);

    void *b = kmalloc(200);
    TEST_ASSERT(b == a, "allocator did not reuse free block");
    
    kfree(b);
}

static void test_double_free() {
    kprintf("[KMALLOC TEST] Double free\n");

    void *a = kmalloc(100);
    kfree(a);
    expect_panic(1);
    kfree(a);
    TEST_ASSERT(check_and_clear_panicked(), "double free did not panic as expected");
    expect_panic(0);
}

static void test_invalid_free() {
    kprintf("[KMALLOC TEST] Invalid free\n");

    void *a = (void*)0x12345678; // invalid pointer
    expect_panic(1);
    kfree(a);
    TEST_ASSERT(check_and_clear_panicked(), "invalid free did not panic as expected");
    expect_panic(0);
}

static void test_fragmentation() {
    kprintf("[KMALLOC TEST] Fragmentation\n");

    void *a = kmalloc(200);
    void *b = kmalloc(300);
    void *c = kmalloc(400);

    kfree(b);  // create hole

    void *d = kmalloc(250);

    TEST_ASSERT((d >= b && d < c), 
        "allocator failed to fill fragmentation hole");

    kfree(a);
    kfree(c);
    kfree(d);
}

static void test_boundary_write() {
    kprintf("[KMALLOC TEST] Boundary write\n");

    uint64_t sz = 128;
    uint8_t *p = kmalloc(sz);

    for (uint64_t i = 0; i < sz; i++)
        p[i] = 0x5A;

    TEST_ASSERT(p[0] == 0x5A && p[sz-1] == 0x5A,
                "boundary values corrupted");

    // Write just inside boundary
    p[sz-1] = 0xA5;
    TEST_ASSERT(p[sz-1] == 0xA5, "failed to write last byte");

    kfree(p);
}

static void test_stress() {
    kprintf("[KMALLOC TEST] Stress test start\n");

    void *blocks[512] = {0};
    uint32_t sizes[512] = {0};

    for (int i = 0; i < 5000; i++) {
        int idx = rand() % 512;
        if (blocks[idx] == NULL) {
            // allocate
            uint32_t sz = (rand() % 4096) + 1; // 1–4096 bytes
            void *p = kmalloc(sz);
            TEST_ASSERT(p != NULL, "stress allocation returned NULL");

            memset(p, 0xCD, sz);
            blocks[idx] = p;
            sizes[idx] = sz;
        } else {
            // verify contents
            uint8_t *p = blocks[idx];
            for (uint32_t k = 0; k < sizes[idx]; k++)
                TEST_ASSERT(p[k] == 0xCD, "memory corruption during stress test");

            // free
            kfree(blocks[idx]);
            blocks[idx] = NULL;
            sizes[idx] = 0;
        }
    }

    // cleanup leftovers
    for (int i = 0; i < 512; i++) {
        if (blocks[i]) kfree(blocks[i]);
    }

    kprintf("[KMALLOC TEST] Stress test complete\n");
}

void vmm_tests(void) {
    kprintf("[VMM TEST] Basic VMM tests\n");

    char * test_buffer = kmalloc(128);
    strcpy(test_buffer, "abcdefghijklmnopqrstuvwxyz");

    status_t st;
    vmm_root_t * root;
    uint64_t buffer_phaddr;
    uint64_t test_phaddr;
    char * test_vaddr = (char * )0xFFFFD00000000000;
    root = vmm_get_root();
    st = vmm_get_physical_address(
        root,
        (uint64_t)test_buffer,
        &buffer_phaddr
    );

    if (st != SUCCESS) {
        panic("vmm_get_physical_address failed with status: %d\n", st);
    }

    st = vmm_map_pages(
        root,
        (uint64_t)test_vaddr,
        buffer_phaddr,
        0x1,
        0x1000,
        0x1 //Read and write permissions
    );

    if (st != SUCCESS) {
        panic("vmm_map_pages failed with status: %d\n", st);
    }

    st = vmm_get_physical_address(
        root,
        (uint64_t)test_vaddr,
        &test_phaddr
    );

    if (st != SUCCESS) {
        panic("vmm_get_physical_address failed with status: %d\n", st);
    }

    TEST_ASSERT(test_phaddr == buffer_phaddr, "Mapped physical address does not match original");

    TEST_ASSERT(strcmp(test_vaddr, "abcdefghijklmnopqrstuvwxyz") == 0, "Data mismatch in mapped memory");

    st = vmm_unmap_pages(
        root,
        (uint64_t)test_vaddr,
        0x1,
        0x1000
    );

    if (st != SUCCESS) {
        panic("vmm_unmap_pages failed with status: %d\n", st);
    }

    kfree(test_buffer);

    kprintf("[VMM TEST] All tests passed!\n");
}

void allocator_tests(void) {
    test_basic_alloc();
    test_zero_alloc();
    test_multiple_allocs();
    test_reuse();
    test_double_free();
    test_invalid_free();
    test_fragmentation();
    test_boundary_write();
    test_stress();
    kprintf("[KMALLOC TEST] All tests passed!\n");
}

void run_all_tests(void) {
    allocator_tests();
    vmm_tests();
}