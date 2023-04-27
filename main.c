#include <stdio.h>
#include "elf_self.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

//TODO: 后期会对这个全局变量进一步的处理，起码是让其变成局部，而findSymbol应该只返回对应的符号偏移为宜，只不过后续有可能会想着SymHeader
// 能有什么可利用的点所以才出此下策
ElfHeader *header = NULL;

bool findSymbol(FILE *, const char *, SymHeader *);

bool findDynSymbol(FILE *, const char *, SectionHeader *, SymHeader *, const char *, void *);

/**
 * 寻找符号，寻找的链路为：当.symtab节不存在时，查找.dynsym节以此找到对应的动态符号基址，否则从.symtab当中寻找需要的符号
 * @param fd 打开的elf文件的句柄
 * @param symbol 需要找到的符号名
 * @param retSymHeader 需要返回的符号头
 * @return
 */
bool findSymbol(FILE *fd, const char *symbol, SymHeader *retSymHeader) {
    if (fd == NULL) return false;
    fseek(fd, 0, SEEK_SET);
    header = (ElfHeader *) malloc(sizeof(ElfHeader));

    struct stat buf;
    int ret = fstat(fileno(fd), &buf);
    char *mmapPtr = NULL;
    int pageSize = getpagesize();
    if (ret == 0) {
        // mmap映射的内存大小必须为page size的整数倍，这里我们有必要进行计算一下
        size_t mmapSize = (buf.st_size + pageSize - 1) / pageSize * pageSize;
        mmapPtr = mmap(NULL, mmapSize, PROT_READ, MAP_SHARED, fileno(fd), 0);
        if (mmapPtr == NULL) {
            free(header);
            printf("get the mmap ptr error: %s", strerror(errno));
            return false;
        }

        memcpy(header, mmapPtr, sizeof(ElfHeader));

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
                bool hasSym = findDynSymbol(fd, symbol, &dynsymSection, retSymHeader, dynstr, mmapPtr);

                if (munmap(mmapPtr, mmapSize) == 0) printf("free mmap space success!\n");
                if (hasSym) return true;

                return false;
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
                memcpy(retSymHeader, symHeader, sizeof(SymHeader));
                if (munmap(mmapPtr, mmapSize) == 0) printf("free mmap space success!\n");
                return true;
            }
        }
        munmap(mmapPtr, mmapSize);
    }
    return false;
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
bool findDynSymbol(FILE *fd, const char *symbol, SectionHeader *retSectionHeader, SymHeader *retSymHeader,
                   const char *dynstr, void *mmapPtr) {
    if (fd == NULL) return false;
    fseek(fd, 0, SEEK_SET);


    if (header == NULL) {
        header = (ElfHeader *) malloc(sizeof(ElfHeader));
        memcpy(header, mmapPtr, sizeof(ElfHeader));
    }

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
            memcpy(retSymHeader, symHeader, sizeof(SymHeader));
            return true;
        }
    }
    return false;
}


int main() {
    const char *soPath = "/mnt/f/CppProject/parse_elf/libaosp11android_runtime64.so";
    FILE *fd = fopen(soPath, "rb");
    if (fd == NULL) {
        printf("fopen error: %s", strerror(errno));
        return -1;
    }
    const char *symbol = "JNI_OnLoad";
    SymHeader *symHeader = (SymHeader *) malloc(sizeof(SymHeader));

    bool resultBool = findSymbol(fd, symbol, symHeader);
    fclose(fd);
    if (resultBool) {
        Elf64_Addr addr = SYM_FIELD(*header, *symHeader, value);
        free(symHeader);
        free(header);
        return addr;
    }
    free(symHeader);
    free(header);
    printf("parse elf finish, the symbol not be found");
    return 0;
}
