#pragma once

// libffi's configuration for Linux on x86_64 and aarch64: what its configure script finds there. shuild compiles libffi with it instead of running configure.

#if !defined(__linux__) || !(defined(__x86_64__) || defined(__aarch64__))
#error "libffi is configured for Linux on x86_64 and aarch64 only"
#endif

#define STDC_HEADERS 1
#define HAVE_ALLOCA_H 1
#define HAVE_DLFCN_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_MEMCPY 1
#define HAVE_MEMFD_CREATE 1

#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE 16
#define SIZEOF_SIZE_T 8
#define HAVE_LONG_DOUBLE 1
#define HAVE_INT128 1

#define HAVE_AS_CFI_PSEUDO_OP 1
#define HAVE_RO_EH_FRAME 1
#define EH_FRAME_FLAGS "a"
#define HAVE_HIDDEN_VISIBILITY_ATTRIBUTE 1

// closures use trampolines from a code page that libffi maps, never writable and executable memory
#define FFI_EXEC_STATIC_TRAMP 1

#if defined(__x86_64__)
#define HAVE_AS_X86_PCREL 1
#define HAVE_AS_X86_64_UNWIND_SECTION_TYPE 1
#endif

// from libffi's configure.ac (AH_BOTTOM)
#ifdef HAVE_HIDDEN_VISIBILITY_ATTRIBUTE
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name) .hidden name
#else
#define FFI_HIDDEN __attribute__((visibility("hidden")))
#endif
#else
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name)
#else
#define FFI_HIDDEN
#endif
#endif
