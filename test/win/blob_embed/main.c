#include <stdio.h>
#include <stdint.h>
extern uint64_t blob_size(void);
extern unsigned char blob_first_byte(void);

int main(void) {
    uint64_t n = blob_size();
    unsigned char b = blob_first_byte();
    if (n != 1024) {
        printf("BLOB-OBJ FAIL: size = %llu, want 1024\n", (unsigned long long)n);
        return 1;
    }
    if (b != '0') {
        printf("BLOB-OBJ FAIL: first byte = 0x%02x, want '0'\n", b);
        return 1;
    }
    printf("BLOB-OBJ OK: %llu bytes through a static lib, first byte '%c'\n",
           (unsigned long long)n, b);
    return 0;
}
