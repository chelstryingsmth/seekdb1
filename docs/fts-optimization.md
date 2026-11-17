# FTS tokenization hot-path optimization

The original do_classify decodes the same input separately for each category check:
one decode for alphabetic characters, two for digits, three for Chinese, and four
for other CJK or UTF-8 separators. The new implementation decodes once, then uses
the same Unicode predicates in the same order. UTF-16 surrogate checks remain
restricted to UTF-16 and UTF-16LE. Decoder failures still leave the output USELESS.

The BENG token stream now avoids calling the collation-aware lowercasing
allocator for tokens already consisting entirely of lowercase ASCII letters
and digits. Other strings still take the original trim-and-lowercase path,
including Unicode, uppercase, and punctuation. The BENG parser also writes
returned token bytes into bounded allocator-owned blocks instead of requesting
an allocation for every token. Each returned token remains at a stable address
until the caller releases the allocator, as before.

These are hot-path changes identified by source inspection, not measured CPU
profiles. They are intended to reduce normalization work and allocation counts
for the English tokenizer; the actual full-text index/build/query impact must
be checked by running the benchmark.

Regression: run the fts_index ik_char_classification and beng_fast_path mysqltests
along with the existing fts_index suite. The IK case checks token/frequency
output across separator categories, including four-byte UTF-8; the BENG case
checks normalized token equivalence for lowercase, mixed-case, repetition and
punctuation. UTF-16 and malformed-input coverage remains to be run or extended.

Validation status: the updated sources pass git whitespace checks; the full
engine has not been compiled or run in this workspace (no Bazel/CMake toolchain
or seekdb runtime available). No performance numbers or score are claimed.
Run the unchanged competition
benchmark before and after this commit on the same Linux host, with SKIP_LOAD=0
and all prescribed parameters, saving each report to a new file. The benchmark,
SQL workload, timing, scoring and baseline files are unchanged.
