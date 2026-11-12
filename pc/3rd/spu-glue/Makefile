# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the SPU core archive (spu_psx.a) from the fetched upstream sources in
# SPU_CORE_DIR plus this directory's stub headers and C shim.
#
#   make -C pc/3rd/spu-glue [SPU_CORE_DIR=/abs/path]   # default ../spu-core
#
# The upstream sources are not in the repository: pc/3rd/SPU-CORE.md says how
# to fetch them (pc/3rd/spu-glue/fetch.sh).  pc/Makefile calls this for the
# native Linux build; the Windows and old-root package builds compile the same
# files with the same flags into their own build directories.

GLUE_DIR     := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
SPU_CORE_DIR ?= $(abspath $(GLUE_DIR)/../spu-core)

CXX      ?= g++
AR       ?= ar
CXXFLAGS ?= -std=c++17 -O2 -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter \
            -Wno-unused-variable -Wno-unused-function -Wno-sign-compare \
            -Wno-missing-field-initializers -Wno-attributes -Wno-deprecated-declarations \
            -Wno-class-memaccess -Wno-uninitialized -Wno-maybe-uninitialized

# Include order:
#   1. SPU_CORE_DIR       -- so the stubs' "upstream/<file>" redirects resolve
#   2. stubs/             -- shim and coupling-point stubs (this project's)
#   3. SPU_CORE_DIR/upstream -- the fetched, patched upstream sources
INCS := -I$(SPU_CORE_DIR) -I$(GLUE_DIR)/stubs -I$(SPU_CORE_DIR)/upstream

OBJ_DIR := $(SPU_CORE_DIR)/obj
OBJS    := $(OBJ_DIR)/spu.o $(OBJ_DIR)/spu_stubs.o $(OBJ_DIR)/spu_shim.o
STAMP   := $(SPU_CORE_DIR)/.fetched
TARGET  := $(SPU_CORE_DIR)/spu_psx.a
HEADERS := $(wildcard $(GLUE_DIR)/stubs/*.h $(GLUE_DIR)/stubs/*/*.h)

.PHONY: all clean

all: $(TARGET)

# fetch.sh writes the stamp after it has verified and patched the sources; a
# patch newer than the stamp means the fetched copy is stale.
# (A missing upstream file gets the same message, not make's "No rule".)
define spu_core_missing
	@echo "error: the SPU core is missing or out of date in $(SPU_CORE_DIR)." >&2
	@echo "       Fetch it with pc/3rd/spu-glue/fetch.sh (see pc/3rd/SPU-CORE.md)." >&2
	@exit 1
endef
$(STAMP): $(wildcard $(GLUE_DIR)/patches/*.patch)
	$(spu_core_missing)
$(SPU_CORE_DIR)/upstream/%:
	$(spu_core_missing)

$(OBJ_DIR)/spu.o: $(STAMP) $(SPU_CORE_DIR)/upstream/spu.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	@echo "CXX $(notdir $(filter %.cpp,$^)) (SPU core)"
	@$(CXX) $(CXXFLAGS) $(INCS) -c $(filter %.cpp,$^) -o $@

$(OBJ_DIR)/%.o: $(STAMP) $(GLUE_DIR)/stubs/%.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	@echo "CXX $(notdir $(filter %.cpp,$^)) (SPU core)"
	@$(CXX) $(CXXFLAGS) $(INCS) -c $(filter %.cpp,$^) -o $@

$(TARGET): $(OBJS)
	@rm -f $@
	@$(AR) rcs $@ $(OBJS)
	@echo "Built $@"

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
