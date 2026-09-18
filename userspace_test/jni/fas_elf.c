#include "fas_elf.h"

#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int vaddr_to_file_offset(const uint8_t *map, const Elf64_Ehdr *eh, uint64_t vaddr, uint64_t *out) {
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(map + eh->e_phoff);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (vaddr >= ph[i].p_vaddr && vaddr < ph[i].p_vaddr + ph[i].p_memsz) {
            *out = ph[i].p_offset + (vaddr - ph[i].p_vaddr);
            return 0;
        }
    }
    return -1;
}

static int find_symbol_vaddr(const uint8_t *map, const Elf64_Shdr *symtab, const Elf64_Shdr *strtab, const char *const *candidates, size_t candidate_count, uint64_t *out_vaddr) {
    const Elf64_Sym *syms = (const Elf64_Sym *)(map + symtab->sh_offset);
    size_t count = symtab->sh_size / sizeof(Elf64_Sym);
    const char *strs = (const char *)(map + strtab->sh_offset);

    for (size_t c = 0; c < candidate_count; c++) {
        for (size_t i = 0; i < count; i++) {
            if (ELF64_ST_TYPE(syms[i].st_info) != STT_FUNC || syms[i].st_value == 0)
                continue;
            if (strcmp(strs + syms[i].st_name, candidates[c]) == 0) {
                *out_vaddr = syms[i].st_value;
                return 0;
            }
        }
    }
    return -1;
}

int fas_elf_resolve_offset(const char *lib_path, const char *const *symbol_candidates, size_t candidate_count, uint64_t *out_offset) {
    struct stat st;
    uint8_t *map = MAP_FAILED;
    int ret = -1;
    int fd = open(lib_path, O_RDONLY);

    if (fd < 0)
        return -1;

    if (fstat(fd, &st) != 0)
        goto out_close;

    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED)
        goto out_close;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)map;

    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64)
        goto out_unmap;

    const Elf64_Shdr *sh = (const Elf64_Shdr *)(map + eh->e_shoff);
    const Elf64_Shdr *dynsym = NULL;
    const Elf64_Shdr *symtab = NULL;

    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_DYNSYM)
            dynsym = &sh[i];
        else if (sh[i].sh_type == SHT_SYMTAB)
            symtab = &sh[i];
    }

    const Elf64_Shdr *target = symtab ? symtab : dynsym;

    if (!target)
        goto out_unmap;

    const Elf64_Shdr *strtab = &sh[target->sh_link];
    uint64_t vaddr;

    if (find_symbol_vaddr(map, target, strtab, symbol_candidates, candidate_count, &vaddr) != 0)
        goto out_unmap;

    if (vaddr_to_file_offset(map, eh, vaddr, out_offset) != 0)
        goto out_unmap;

    ret = 0;

out_unmap:
    munmap(map, (size_t)st.st_size);
out_close:
    close(fd);
    return ret;
}
