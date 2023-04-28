#include <stdio.h>
#include "elf_self.h"
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


Elf64_Addr findSymbolOffset(FILE *, const char *);

Elf64_Addr findDynSymbolOffset(FILE *, const char *, SectionHeader *, const char *, void *);

/**
 * 寻找符号，寻找的链路为：当.symtab节不存在时，查找.dynsym节以此找到对应的动态符号基址，否则从.symtab当中寻找需要的符号
 * @param fd 打开的elf文件的句柄
 * @param symbol 需要找到的符号名
 * @param retSymHeader 需要返回的符号头
 * @return 返回值为0则意味着找不到或者其符号实际上不在该elf当中
 */
Elf64_Addr findSymbolOffset(FILE *fd, const char *symbol) {
    if (fd == NULL) return 0;

    // 防止游标传入时不在文件起始位置
    fseek(fd, 0, SEEK_SET);
    ElfHeader *header = NULL;

    struct stat buf;
    int ret = fstat(fileno(fd), &buf);
    char *mmapPtr = NULL;
    int pageSize = getpagesize();
    if (ret == 0) {
        // mmap映射的内存大小必须为page size的整数倍，这里我们有必要进行计算一下
        size_t mmapSize = (buf.st_size + pageSize - 1) / pageSize * pageSize;
        mmapPtr = mmap(NULL, mmapSize, PROT_READ, MAP_SHARED, fileno(fd), 0);
        if (mmapPtr == NULL) {
            printf("get the mmap ptr error: %s", strerror(errno));
            return 0;
        }

        header = (ElfHeader *) mmapPtr;

        // 获取.shstrtab节
        SectionHeader *shstrSectionHeader = (SectionHeader *) (mmapPtr + ELF_FIELD(*header, shoff) +
                                                               ELF_FIELD(*header, shentsize) *
                                                               ELF_FIELD(*header, shstrndx));

        const char *strtab;
        const char *dynstr;
        const char *shstrtab = (const char *) (mmapPtr + SECTION_FIELD(*header, *shstrSectionHeader, offset));

        SectionHeader symSection;
        SectionHeader dynsymSection;

        // 获取.symtab或.dynsym节区的Elf_Sym头信息以此找到对应符号的偏移
        bool getSymSection = false;
        for (int i = 0; i < ELF_FIELD(*header, shnum); i++) {
            SectionHeader *sectionHeader = (SectionHeader *) (mmapPtr + ELF_FIELD(*header, shoff) +
                                                              ELF_FIELD(*header, shentsize) * i);
            const char *sectionName = shstrtab + SECTION_FIELD(*header, *sectionHeader, name);
            if (strcmp(".strtab", sectionName) == 0) {
                strtab = (char *) (mmapPtr + SECTION_FIELD(*header, *sectionHeader, offset));
            } else if (strcmp(".dynstr", sectionName) == 0) {
                dynstr = (char *) (mmapPtr + SECTION_FIELD(*header, *sectionHeader, offset));
            } else if (strcmp(".symtab", sectionName) == 0) {
                symSection = *sectionHeader;
                getSymSection = true;
            } else if (strcmp(".dynsym", sectionName) == 0) {
                dynsymSection = *sectionHeader;
            }
            // 当.symtab节不存在时，查找.dynsym节以此找到对应的动态符号基址
            if (i == ELF_FIELD(*header, shnum) - 1 && !getSymSection) {
                Elf64_Addr symOff = findDynSymbolOffset(fd, symbol, &dynsymSection, dynstr, mmapPtr);
                munmap(mmapPtr, mmapSize);
                return symOff;
            }
        }

        // 拿到符号表偏移
        uint64_t off = SECTION_FIELD(*header, symSection, offset);

        // 计算symbol table的长度
        int arrayLen = SECTION_FIELD(*header, symSection, size) / SECTION_FIELD(*header, symSection, entsize);

        for (int i = 0; i < arrayLen; ++i) {
            SymHeader *symHeader = (SymHeader *) (mmapPtr + off + i * sizeof(SymHeader));
            unsigned char info = SYM_FIELD(*header, *symHeader, info);
            const char *symbolName = strtab + SYM_FIELD(*header, *symHeader, name);
            // 如果类型为STT_FUNC,那么此符号为一个函数
            if (ELF_ST_TYPE(info) == STT_FUNC && strcmp(symbol, symbolName) == 0) {
                printf("sym name: %s, offset: 0x%lx\n", symbolName, SYM_FIELD(*header, *symHeader, value));
                Elf64_Addr addr =
                        SYM_FIELD(*header, *symHeader, value) >= 0 ? SYM_FIELD(*header, *symHeader, value) : -1;

                munmap(mmapPtr, mmapSize);
                return addr;
            }
        }
        munmap(mmapPtr, mmapSize);
    }
    printf("parse elf success but can't find the symbol\n");
    return 0;
}

/**
 * 通过动态链接表查找符号
 * @param fd 打开的so句柄
 * @param symbol 需要找到的符号
 * @param retSectionHeader 需要返回的节头
 * @param retSymHeader 需要返回的符号头
 * @param dynstr 动态符号表需要对应的字符串指针
 * @param mmapPtr 对应so的mmap指针
 * @return
 */
Elf64_Addr findDynSymbolOffset(FILE *fd, const char *symbol, SectionHeader *retSectionHeader,
                               const char *dynstr, void *mmapPtr) {
    if (fd == NULL) return 0;
    fseek(fd, 0, SEEK_SET);

    ElfHeader *header = (ElfHeader *) mmapPtr;

    int arrayLen =
            SECTION_FIELD(*header, *retSectionHeader, size) / SECTION_FIELD(*header, *retSectionHeader, entsize);

    unsigned long off = SECTION_FIELD(*header, *retSectionHeader, offset);
    for (int i = 0; i < arrayLen; ++i) {
        SymHeader *symHeader = (SymHeader *) (mmapPtr + off + i * sizeof(SymHeader));
        unsigned char info = SYM_FIELD(*header, *symHeader, info);
        // 如果类型为STT_FUNC,那么此符号为一个函数
        if (ELF_ST_TYPE(info) == STT_FUNC && strcmp(symbol, dynstr + SYM_FIELD(*header, *symHeader, name)) == 0) {
            printf("dynsym name: %s, offset: 0x%lx\n", dynstr + SYM_FIELD(*header, *symHeader, name),
                   SYM_FIELD(*header, *symHeader, value));
            Elf64_Addr addr = SYM_FIELD(*header, *symHeader, value) >= 0 ? SYM_FIELD(*header, *symHeader, value) : -1;
            return addr;
        }
    }
    return 0;
}

Elf64_Addr getElfSymbolAddress(const char *elfFilePath, const char *symbol) {
    FILE *fd = fopen(elfFilePath, "rb");
    if (fd == NULL) {
        printf("fopen error: %s", strerror(errno));
        return -1;
    }

    Elf64_Addr result = findSymbolOffset(fd, symbol);
    if (result == 0) {
        printf("can't find symbol at this elf");
        return 0;
    }
    printf("get Symbol offset: 0x%lx", result);
    fclose(fd);
    return result;
}

int main() {
    return (int) getElfSymbolAddress(
            "/mnt/c/Users/juziss/Desktop/github_project/parse_elf/libdusanwa.so",
            "JNI_OnLoad!");
}
