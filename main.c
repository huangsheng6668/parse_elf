#include <stdio.h>
#include "elf_self.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>

ElfHeader header;
char *strtab;
char *shstrtab;
char *dynstr;

bool findSymbol(FILE *, const char *, SymHeader *);

bool findDynSymbol(FILE *, const char *, SectionHeader *, SymHeader *);

bool findSymbol(FILE *fd, const char *symbol, SymHeader *retSymHeader) {
    if (fd == NULL) return false;
    fseek(fd, 0, SEEK_SET);

    fread(&header, 1, sizeof(ElfHeader), fd);


//    uint64_t addr = (uint64_t) &header + off;

    // 获取shstrtab段的SectionHeader以此拿到Section节区的名字
    SectionHeader sectionHeader;
    fseek(fd, ELF_FIELD(header, shoff) + ELF_FIELD(header, shentsize) * ELF_FIELD(header, shstrndx), SEEK_SET);
    fread(&sectionHeader, 1, sizeof(SectionHeader), fd);

    shstrtab = (char *) malloc(SECTION_FIELD(header, sectionHeader, size));
    fseek(fd, SECTION_FIELD(header, sectionHeader, offset), SEEK_SET); //定位到shstrtab段的起始位置
    fread(shstrtab, SECTION_FIELD(header, sectionHeader, size), 1, fd); //读取shstrtab段的内容

    SectionHeader symSection;
    SectionHeader dynsymSection;

    // 获取.symtab或.dynsym节区的Elf_Sym头信息以此找到对应符号的偏移
    bool getSymSection = false;
    for (int i = 0; i < ELF_FIELD(header, shnum); i++) {
        SectionHeader sectionHeader_;
        fseek(fd, ELF_FIELD(header, shoff) + ELF_FIELD(header, shentsize) * i, SEEK_SET);
        fread(&sectionHeader_, 1, sizeof(SectionHeader), fd);
        if (strcmp(".strtab", shstrtab + SECTION_FIELD(header, sectionHeader_, name)) == 0) {
            strtab = (char *) malloc(SECTION_FIELD(header, sectionHeader_, size));
            fseek(fd, SECTION_FIELD(header, sectionHeader_, offset), SEEK_SET); //定位到shstrtab段的起始位置
            fread(strtab, SECTION_FIELD(header, sectionHeader_, size), 1, fd); //读取shstrtab段的内容
        } else if (strcmp(".dynstr", shstrtab + SECTION_FIELD(header, sectionHeader_, name)) == 0) {
            dynstr = (char *) malloc(SECTION_FIELD(header, sectionHeader_, size));
            fseek(fd, SECTION_FIELD(header, sectionHeader_, offset), SEEK_SET); //定位到dynstr段的起始位置
            fread(dynstr, SECTION_FIELD(header, sectionHeader_, size), 1, fd); //读取dynstr段的内容
        } else if (strcmp(".symtab", shstrtab + SECTION_FIELD(header, sectionHeader_, name)) == 0) {
            symSection = sectionHeader_;
            getSymSection = true;
        } else if (strcmp(".dynsym", shstrtab + SECTION_FIELD(header, sectionHeader_, name)) == 0) {
            dynsymSection = sectionHeader_;
        }
        // 当.symtab节不存在时，查找.dynsym节以此找到对应的动态符号基址
        if (i == ELF_FIELD(header, shnum) - 1 && !getSymSection) {
            bool hasSym = findDynSymbol(fd, symbol, &dynsymSection, retSymHeader);
            if (hasSym) return true;
            return false;
        }
    }

    uint64_t off = SECTION_FIELD(header, symSection, offset);

    // 计算symbol table的长度
    int arrayLen = SECTION_FIELD(header, symSection, size) / SECTION_FIELD(header, symSection, entsize);

    SymHeader sym[arrayLen];
    fseek(fd, off, SEEK_SET);
    fread(sym, sizeof(SymHeader), arrayLen, fd);

    for (int i = 0; i < arrayLen; ++i) {
        unsigned char info = SYM_FIELD(header, sym[i], info);

        // 如果类型为STT_FUNC,那么此符号为一个函数
        if (ELF_ST_TYPE(info) == STT_FUNC && strcmp(symbol, strtab + SYM_FIELD(header, sym[i], name)) == 0) {
            printf("sym name: %s, offset: 0x%lx\n", strtab + SYM_FIELD(header, sym[i], name),
                   SYM_FIELD(header, sym[i], value));
            memcpy(retSymHeader, &sym[i], sizeof(SymHeader));
            return true;
        }
    }

    // 后期研究看看怎么通过段去找符号
//    ProgramHeader programHeader;
//    int phArrLen = ELF_FIELD(header, phnum);
//    ProgramHeader programHeaderTable[phArrLen];
//
//    fseek(fd, ELF_FIELD(header, phoff), SEEK_SET); //定位到shstrtab段的起始位置
//    fread(&programHeaderTable, sizeof(ProgramHeader), phArrLen, fd); //读取shstrtab段的内容
//
//    for (int i = 0; i < phArrLen; ++i) {
//        // 找到动态段
//        if (PROGRAM_FIELD(header, programHeaderTable[i], type) == PT_DYNAMIC) {
//            DynamicEntry dynamicEntry;
//            fseek(fd, PROGRAM_FIELD(header, programHeaderTable[i], offset), SEEK_SET);
//            PROGRAM_FIELD(header, programHeaderTable[i], offset) / PROGRAM_FIELD(header, programHeaderTable[i], memsz) /
//                    sizeof(ProgramHeader);
//            fread(&dynamicEntry, sizeof(DynamicEntry), 1, fd);
//            break;
//        }
//    }
}

bool findDynSymbol(FILE *fd, const char *symbol, SectionHeader *retSectionHeader, SymHeader *retSymHeader) {
    if (fd == NULL) return false;
    fseek(fd, 0, SEEK_SET);

    fread(&header, 1, sizeof(ElfHeader), fd);

    // 计算symbol table的长度
    int arrayLen = SECTION_FIELD(header, *retSectionHeader, size) / SECTION_FIELD(header, *retSectionHeader, entsize);

    SymHeader sym[arrayLen];
    fseek(fd, SECTION_FIELD(header, *retSectionHeader, offset), SEEK_SET);
    fread(sym, sizeof(SymHeader), arrayLen, fd);

    for (int i = 0; i < arrayLen; ++i) {
        unsigned char info = SYM_FIELD(header, sym[i], info);
        // 如果类型为STB_GLOBAL | STT_FUNC,那么此符号为一个函数
        if (ELF_ST_TYPE(info) == STT_FUNC && strcmp(symbol, dynstr + SYM_FIELD(header, sym[i], name)) == 0) {
            printf("dynsym name: %s, offset: 0x%lx\n", dynstr + SYM_FIELD(header, sym[i], name),
                   SYM_FIELD(header, sym[i], value));
            memcpy(retSymHeader, &sym[i], sizeof(SymHeader));
            return true;
        }
    }

    return false;
}


int main() {
    const char *soPath = "/mnt/f/CppProject/parse_elf/libcpixel11.so";
    FILE *fd = fopen(soPath, "rb");
    if (fd == NULL) {
        printf("fopen error: %s", strerror(errno));
    }
    const char* symbol = "openat";
    SymHeader *symHeader = (SymHeader *) malloc(sizeof(SymHeader));
    bool resultBool = findSymbol(fd, symbol, symHeader);
    if (resultBool) printf("offset: 0x%lx", SYM_FIELD(header, *symHeader, value));
    return 0;
}
