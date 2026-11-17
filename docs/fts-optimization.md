# IK classification optimization

The original do_classify decodes the same input separately for each category check:
one decode for alphabetic characters, two for digits, three for Chinese, and four
for other CJK or UTF-8 separators. The new implementation decodes once, then uses
the same Unicode predicates in the same order. UTF-16 surrogate checks remain
restricted to UTF-16 and UTF-16LE. Decoder failures still leave the output USELESS.

This is a source inspection finding, not a measured CPU profile. It affects IK
tokenization used during indexing and query analysis; no BENG improvement is claimed.

Regression: run the fts_index ik_char_classification mysqltest case and the existing
fts_index suite. The new case compares full token/frequency JSON across separator
categories, including four-byte UTF-8. UTF-16 and malformed-input coverage remains
to be run or extended before treating correctness as fully validated.

Validation status: not compiled or executed here. The host is Windows with no
configured WSL distribution or seekdb instance, and direct GitHub cloning failed.
No performance numbers or score are claimed. Run the unchanged competition
benchmark before and after this commit on the same Linux host, with SKIP_LOAD=0
and all prescribed parameters, saving each report to a new file. The benchmark,
SQL workload, timing, scoring and baseline files are unchanged.
