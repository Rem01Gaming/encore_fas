#ifndef FAS_ELF_H
#define FAS_ELF_H

#include <stddef.h>
#include <stdint.h>

int fas_elf_resolve_offset(const char *lib_path, const char *const *symbols, size_t count, uint64_t *out_offset);

#endif
