#include <stdio.h>
extern int blob_size(void);
int main(void) {
    int n = blob_size();
    if (n <= 0) {
        printf("RCDATA-STATIC FAIL: resource not found through the static lib (got %d)\n", n);
        return 1;
    }
    printf("RCDATA-STATIC OK: %d bytes\n", n);
    return 0;
}
