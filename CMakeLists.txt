cmake_minimum_required(VERSION 3.16.1)
project(parse_elf)

add_executable(parse_elf main.c
        mac_elf_struct.h
)
