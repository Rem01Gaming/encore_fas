#ifndef FAS_ELF_H
#define FAS_ELF_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Resolves a symbol's file offset inside an ELF64 shared object by
 *        walking its symbol table and program headers. Tries each candidate
 *        name in order and returns on the first match, since a single logical
 *        function can be mangled differently across ABI/AOSP revisions.
 * @param lib_path Path to the .so on disk.
 * @param symbol_candidates Array of mangled symbol names to try, in priority order.
 * @param candidate_count Number of entries in symbol_candidates.
 * @param out_offset Receives the resolved on-disk file offset on success.
 * @return 0 on success, -1 if the file could not be read or no candidate matched.
 */
int fas_elf_resolve_offset(const char *lib_path, const char *const *symbol_candidates,
                            size_t candidate_count, uint64_t *out_offset);

#endif
