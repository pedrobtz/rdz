# Double encodings: the Stage J experiment

The evidence behind encoding 23 (`.agents/encoding-research.md`, "plan-c Stage J
result"). Not part of the package build.

    Rscript tools/experiments/doubles/gen.R DIR      # 4e6 doubles per distribution
    cc -O2 -std=gnu11 -Isrc/vendor/zstd -DZSTDLIB_VISIBLE= -DZSTDERRORLIB_VISIBLE= \
      tools/experiments/doubles/exp.c src/vendor/zstd/zstd.c -o exp
    ./exp DIR/*.bin                                   # ALP_DIV=1: the division decode
    Rscript tools/experiments/doubles/ref.R DIR       # qs2 and fst on the same data

`exp.c` is a prototype: ALP and ALP-RD per 131,072-value block, against plain
and shuffled bytes under zstd 1, each round trip checked bit for bit.
