/*
 * Static build configuration for the vendored copy of liblzma (XZ Utils 5.6.2).
 *
 * This file is NOT part of upstream XZ Utils. Upstream generates the
 * equivalent settings with CMake or autoconf; it is written by hand here so
 * that liblzma can be built by third_party/Makefile with nothing more than a
 * C compiler. It mirrors the feature set of the upstream CMake build (all
 * integrity checks, match finders, encoders and decoders), with two
 * simplifications that do not affect the single-threaded streaming API used by
 * ChampSim:
 *
 *   - Threading is disabled (no lzma_stream_encoder_mt/lzma_stream_decoder_mt),
 *     so no pthread dependency is introduced.
 *   - The portable C implementations of CRC32/CRC64 are used instead of the
 *     CPU-specific (CLMUL / ARMv8 CRC) variants.
 */

#ifndef CHAMPSIM_VENDORED_LZMA_CONFIG_H
#define CHAMPSIM_VENDORED_LZMA_CONFIG_H

/* Package info */
#define PACKAGE_NAME "XZ Utils"
#define PACKAGE_BUGREPORT "xz@tukaani.org"
#define PACKAGE_URL "https://tukaani.org/xz/"

/* Standard C99 headers and types */
#define HAVE_STDBOOL_H 1
#define HAVE__BOOL 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1

/* Disable assert() inside the library, as a release build would */
#ifndef NDEBUG
#  define NDEBUG 1
#endif

/* Static library: no symbol visibility tricks */
#define HAVE_VISIBILITY 0

/* Keep the tuklib helper functions inside the lzma_ namespace */
#define TUKLIB_SYMBOL_PREFIX lzma_

/* Physical memory detection through sysconf() (Linux, macOS, BSDs) */
#define TUKLIB_PHYSMEM_SYSCONF 1

/* Byte order and compiler builtins (GCC and Clang) */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define WORDS_BIGENDIAN 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define HAVE___BUILTIN_BSWAPXX 1
#  define HAVE___BUILTIN_ASSUME_ALIGNED 1
#endif

/* Architectures on which unaligned integer access is known to be fast */
#if defined(__x86_64__) || defined(__i386__) || defined(__ARM_FEATURE_UNALIGNED) || defined(__riscv_misaligned_fast)                              \
    || (defined(__powerpc64__) && !defined(WORDS_BIGENDIAN)) || (defined(__powerpc__) && defined(WORDS_BIGENDIAN))
#  define TUKLIB_FAST_UNALIGNED_ACCESS 1
#endif

/* Integrity checks */
#define HAVE_CHECK_CRC32 1
#define HAVE_CHECK_CRC64 1
#define HAVE_CHECK_SHA256 1

/* Match finders */
#define HAVE_MF_HC3 1
#define HAVE_MF_HC4 1
#define HAVE_MF_BT2 1
#define HAVE_MF_BT3 1
#define HAVE_MF_BT4 1

/* Encoders */
#define HAVE_ENCODERS 1
#define HAVE_ENCODER_LZMA1 1
#define HAVE_ENCODER_LZMA2 1
#define HAVE_ENCODER_DELTA 1
#define HAVE_ENCODER_X86 1
#define HAVE_ENCODER_ARM 1
#define HAVE_ENCODER_ARMTHUMB 1
#define HAVE_ENCODER_ARM64 1
#define HAVE_ENCODER_POWERPC 1
#define HAVE_ENCODER_IA64 1
#define HAVE_ENCODER_SPARC 1
#define HAVE_ENCODER_RISCV 1

/* Decoders */
#define HAVE_DECODERS 1
#define HAVE_DECODER_LZMA1 1
#define HAVE_DECODER_LZMA2 1
#define HAVE_DECODER_DELTA 1
#define HAVE_DECODER_X86 1
#define HAVE_DECODER_ARM 1
#define HAVE_DECODER_ARMTHUMB 1
#define HAVE_DECODER_ARM64 1
#define HAVE_DECODER_POWERPC 1
#define HAVE_DECODER_IA64 1
#define HAVE_DECODER_SPARC 1
#define HAVE_DECODER_RISCV 1
#define HAVE_LZIP_DECODER 1

#endif
