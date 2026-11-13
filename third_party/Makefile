# Vendored third-party dependencies of ChampSim.
#
# The sources below are checked into this repository so that ChampSim can be
# built without network access, a package manager, or CMake. Only a C and a C++
# compiler, `ar`, and GNU make are required. The top-level Makefile invokes this
# file automatically; it can also be used directly:
#
#   make -C third_party          # build the libraries the simulator links against
#   make -C third_party test     # additionally build Catch2 (needed by `make test`)
#   make -C third_party clean    # remove everything that was built
#
# Provenance. Each directory holds the unmodified upstream release that the
# former vcpkg manifest resolved to, reduced to the files needed to build the
# library. The archives were checked against the SHA512 recorded by vcpkg.
#
#   fmt/            fmt 10.2.1               https://github.com/fmtlib/fmt/archive/10.2.1.tar.gz
#   CLI11/          CLI11 2.4.2 (hdr only)   https://github.com/CLIUtils/CLI11/archive/v2.4.2.tar.gz
#   nlohmann_json/  JSON 3.11.3 (hdr only)   https://github.com/nlohmann/json/archive/v3.11.3.tar.gz
#   Catch2/         Catch2 3.6.0             https://github.com/catchorg/Catch2/archive/v3.6.0.tar.gz
#   zlib/           zlib 1.3.1               https://github.com/madler/zlib/archive/v1.3.1.tar.gz
#   bzip2/          bzip2 1.0.8              https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
#   xz/             XZ Utils 5.6.2 (liblzma) https://github.com/tukaani-project/xz/archive/v5.6.2.tar.gz
#
# Three files are not from upstream. They replace headers that the upstream
# CMake builds generate at configure time or that the vcpkg port installed:
#
#   xz/config.h                             build configuration of liblzma
#   Catch2/src/catch2/catch_user_config.hpp catch_user_config.hpp.in with Catch2's defaults
#   Catch2/src/catch.hpp                    includes <catch2/catch_all.hpp>
#
# All variables defined here carry a TP_ prefix so that variables passed to the
# top-level make (OBJ_ROOT, CXXFLAGS, ...) do not leak into these builds. The
# compilers are taken from CC and CXX as usual.

TP_BUILD_DIR := _build
TP_OBJ_DIR := $(TP_BUILD_DIR)/obj
TP_LIB_DIR := $(TP_BUILD_DIR)/lib

# Warnings are disabled: this is third-party code that is not maintained here.
TP_COMMON_FLAGS := -O3 -fPIC -w
TP_CFLAGS := -std=gnu99 $(TP_COMMON_FLAGS)
TP_CXXFLAGS := -std=c++17 $(TP_COMMON_FLAGS)

# Set V=1 to print the full compiler command lines
ifeq ($(V),1)
TP_Q :=
else
TP_Q := @
endif

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all test clean

### Library definitions

TP_fmt_SRCS := fmt/src/format.cc fmt/src/os.cc
TP_fmt_FLAGS := -Ifmt/include

TP_z_SRCS := $(sort $(wildcard zlib/*.c))
TP_z_FLAGS := -DHAVE_UNISTD_H -DHAVE_STDARG_H

TP_bz2_SRCS := $(addprefix bzip2/,blocksort.c huffman.c crctable.c randtable.c compress.c decompress.c bzlib.c)
TP_bz2_FLAGS := -D_FILE_OFFSET_BITS=64

TP_lzma_SRCS := $(sort $(wildcard xz/src/common/*.c xz/src/liblzma/*/*.c))
TP_lzma_FLAGS := -DHAVE_CONFIG_H -Ixz -Ixz/src/common $(addprefix -Ixz/src/liblzma/,api common check lz rangecoder lzma delta simple)

TP_catch2_all_srcs := $(sort $(wildcard Catch2/src/catch2/*.cpp Catch2/src/catch2/*/*.cpp Catch2/src/catch2/*/*/*.cpp))
TP_Catch2Main_SRCS := Catch2/src/catch2/internal/catch_main.cpp
TP_Catch2Main_FLAGS := -ICatch2/src
TP_Catch2_SRCS := $(filter-out $(TP_Catch2Main_SRCS),$(TP_catch2_all_srcs))
TP_Catch2_FLAGS := -ICatch2/src

TP_RUNTIME_LIBS := fmt lzma z bz2
TP_TEST_LIBS := Catch2Main Catch2

all: $(patsubst %,$(TP_LIB_DIR)/lib%.a,$(TP_RUNTIME_LIBS))
test: all $(patsubst %,$(TP_LIB_DIR)/lib%.a,$(TP_TEST_LIBS))

clean:
	$(RM) -r $(TP_BUILD_DIR)

### Build rules

# Declare a static library
# $1 - the library name, such that TP_$1_SRCS and TP_$1_FLAGS are defined
define tp_library
TP_$1_OBJS := $$(patsubst %,$$(TP_OBJ_DIR)/%.o,$$(TP_$1_SRCS))
TP_ALL_OBJS += $$(TP_$1_OBJS)
$$(TP_$1_OBJS): TP_LIB_FLAGS := $$(TP_$1_FLAGS)
$$(TP_LIB_DIR)/lib$1.a: $$(TP_$1_OBJS)
	@mkdir -p $$(@D)
	@echo "  AR      third_party/$$@"
	$$(TP_Q)$$(RM) $$@
	$$(TP_Q)$$(AR) rcs $$@ $$^
endef

TP_ALL_OBJS :=
$(foreach lib,$(TP_RUNTIME_LIBS) $(TP_TEST_LIBS),$(eval $(call tp_library,$(lib))))

$(TP_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	@echo "  CC      third_party/$<"
	$(TP_Q)$(CC) $(TP_CFLAGS) $(TP_LIB_FLAGS) -MMD -MP -c -o $@ $<

$(TP_OBJ_DIR)/%.cc.o: %.cc
	@mkdir -p $(@D)
	@echo "  CXX     third_party/$<"
	$(TP_Q)$(CXX) $(TP_CXXFLAGS) $(TP_LIB_FLAGS) -MMD -MP -c -o $@ $<

$(TP_OBJ_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	@echo "  CXX     third_party/$<"
	$(TP_Q)$(CXX) $(TP_CXXFLAGS) $(TP_LIB_FLAGS) -MMD -MP -c -o $@ $<

-include $(TP_ALL_OBJS:.o=.d)
