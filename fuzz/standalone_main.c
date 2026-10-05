/* Drives a libFuzzer target over files, for compilers without a libFuzzer
 * runtime (Apple's): tools/run-fuzz --replay builds with ASan and UBSan and
 * runs each named file once. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        uint8_t *buf = NULL;
        size_t n = 0, cap = 0;
        int c;
        if (!f) {
            perror(argv[i]);
            return 2;
        }
        while ((c = fgetc(f)) != EOF) {
            if (n == cap) {
                uint8_t *grown;
                cap = cap ? cap * 2 : 4096;
                grown = (uint8_t *)realloc(buf, cap);
                if (!grown) return 2;
                buf = grown;
            }
            buf[n++] = (uint8_t)c;
        }
        fclose(f);
        LLVMFuzzerTestOneInput(buf, n);
        free(buf);
    }
    printf("replayed %d inputs\n", argc - 1);
    return 0;
}
