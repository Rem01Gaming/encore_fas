#include "fas_elf.h"

#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct elf_view {
    const uint8_t *base;
    size_t size;
    const Elf64_Ehdr *eh;
};

/**
 * @brief Checks that a range lies inside the file.
 *
 * @param v The mapped file.
 * @param off The start of the range.
 * @param len The length of the range.
 * @return 1 when the range is inside the file. Otherwise 0.
 */
static int in_file(const struct elf_view *v, uint64_t off, uint64_t len) {
    return off <= v->size && len <= v->size - off;
}

/**
 * @brief Converts a virtual address to a file offset with the PT_LOAD headers.
 *
 * @param v The mapped file.
 * @param vaddr The virtual address.
 * @param out The variable that receives the file offset.
 * @return 0 on success. Otherwise -1.
 */
static int vaddr_to_offset(const struct elf_view *v, uint64_t vaddr, uint64_t *out) {
    if (!in_file(v, v->eh->e_phoff, (uint64_t)v->eh->e_phnum * sizeof(Elf64_Phdr)))
        return -1;

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(v->base + v->eh->e_phoff);

    for (int i = 0; i < v->eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (!in_file(v, ph[i].p_offset, ph[i].p_filesz))
            continue;
        if (vaddr >= ph[i].p_vaddr && vaddr - ph[i].p_vaddr < ph[i].p_filesz) {
            *out = ph[i].p_offset + (vaddr - ph[i].p_vaddr);
            return 0;
        }
    }
    return -1;
}

/**
 * @brief Looks for a function symbol in one symbol table.
 *
 * @param v The mapped file.
 * @param symtab The section header of the symbol table.
 * @param symbols The names to look for.
 * @param count The number of names.
 * @param out_vaddr The variable that receives the address of the first name that the table holds.
 * @return 0 on success. Otherwise -1.
 */
static int find_symbol(const struct elf_view *v, const Elf64_Shdr *symtab, const char *const *symbols, size_t count, uint64_t *out_vaddr) {
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(v->base + v->eh->e_shoff);

    if (symtab->sh_link >= v->eh->e_shnum || symtab->sh_entsize != sizeof(Elf64_Sym))
        return -1;

    const Elf64_Shdr *strtab = &sh[symtab->sh_link];

    if (!in_file(v, symtab->sh_offset, symtab->sh_size) || !in_file(v, strtab->sh_offset, strtab->sh_size) || strtab->sh_size == 0)
        return -1;

    const Elf64_Sym *syms = (const Elf64_Sym *)(v->base + symtab->sh_offset);
    const char *strs = (const char *)(v->base + strtab->sh_offset);
    size_t nsyms = symtab->sh_size / sizeof(Elf64_Sym);

    for (size_t c = 0; c < count; c++) {
        for (size_t i = 0; i < nsyms; i++) {
            if (ELF64_ST_TYPE(syms[i].st_info) != STT_FUNC || syms[i].st_shndx == SHN_UNDEF || syms[i].st_value == 0)
                continue;
            if (syms[i].st_name >= strtab->sh_size)
                continue;
            if (strncmp(strs + syms[i].st_name, symbols[c], strtab->sh_size - syms[i].st_name) == 0) {
                *out_vaddr = syms[i].st_value;
                return 0;
            }
        }
    }
    return -1;
}

/**
 * @brief Finds the file offset of a function in an arm64 ELF file.
 *
 * @param lib_path The path of the ELF file.
 * @param symbols The candidate symbol names. The first name that the file holds wins.
 * @param count The number of names.
 * @param out_offset The variable that receives the file offset.
 * @return 0 on success. Otherwise -1.
 */
int fas_elf_resolve_offset(const char *lib_path, const char *const *symbols, size_t count, uint64_t *out_offset) {
    struct stat st;
    int ret = -1;
    int fd = open(lib_path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;

    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr))
        goto out_close;

    struct elf_view v = {.size = (size_t)st.st_size};

    v.base = mmap(NULL, v.size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (v.base == MAP_FAILED)
        goto out_close;

    v.eh = (const Elf64_Ehdr *)v.base;
    if (memcmp(v.eh->e_ident, ELFMAG, SELFMAG) != 0 || v.eh->e_ident[EI_CLASS] != ELFCLASS64 || v.eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        v.eh->e_machine != EM_AARCH64 || v.eh->e_shentsize != sizeof(Elf64_Shdr) ||
        !in_file(&v, v.eh->e_shoff, (uint64_t)v.eh->e_shnum * sizeof(Elf64_Shdr)))
        goto out_unmap;

    const Elf64_Shdr *sh = (const Elf64_Shdr *)(v.base + v.eh->e_shoff);
    uint64_t vaddr;

    for (int i = 0; i < v.eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM)
            continue;
        if (find_symbol(&v, &sh[i], symbols, count, &vaddr) == 0 && vaddr_to_offset(&v, vaddr, out_offset) == 0) {
            ret = 0;
            break;
        }
    }

out_unmap:
    munmap((void *)v.base, v.size);
out_close:
    close(fd);
    return ret;
}
