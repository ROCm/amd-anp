PLUGIN_NAME = librccl-net.so
TARGET = build/$(PLUGIN_NAME)
CC = /opt/rocm/bin/hipcc
CFLAGS = -fPIC -O2 -O3 -DNDEBUG -MMD -MP
CFLAGS += -DTARGET_PLUGIN
INCLUDES = -Iinclude \
		   -I/opt/rocm/include \
		   -I/usr/include \
		   -include cstring

CPPFLAGS = -D__HIP_PLATFORM_AMD__
LDFLAGS = -shared -pthread -libverbs -lmpi -lionic

SRC_DIRS = src src/misc
EXCLUDE_FILE =

SRCS = $(filter-out $(EXCLUDE_FILE), $(wildcard $(addsuffix /*.cc, $(SRC_DIRS))))
SRCS += $(RCCL_HOME)/src/misc/socket.cc \
    $(RCCL_HOME)/src/misc/param.cc \
    $(RCCL_HOME)/src/misc/ibvsymbols.cc \
    $(RCCL_HOME)/src/misc/ibvwrap.cc \
    $(RCCL_BUILD)/hipify/src/misc/utils.cc \
    $(RCCL_BUILD)/hipify/src/misc/rocmwrap.cc \
    $(RCCL_BUILD)/hipify/src/debug.cc
OBJS = $(patsubst %.cc, build/%.o, $(SRCS))
DEPS = $(patsubst %.cc, build/%.d, $(SRCS))

ifeq ($(ANP_TELEMETRY_ENABLED), 1)
    CFLAGS+= -DANP_TELEMETRY_ENABLED
else
    CFLAGS+= -UANP_TELEMETRY_ENABLED
endif

# Require RCCL_HOME unless target is clean/help/uninstall
ifneq ($(filter clean help uninstall,$(MAKECMDGOALS)),)
    # Skip checks
else
    ifeq ($(RCCL_HOME),)
        $(error RCCL_HOME is not set. Please specify it using 'make RCCL_HOME=/path/to/rccl')
    endif
    ifeq ($(origin RCCL_BUILD), undefined)
        RCCL_BUILD := $(RCCL_HOME)/build/release
    endif
endif

# Check if RCCL build is in order
ifneq ($(RCCL_BUILD),)
    ifneq ($(wildcard $(RCCL_BUILD)/librccl.so),)
        ifneq ($(wildcard $(RCCL_BUILD)/hipify),)
            ifneq ($(wildcard $(RCCL_BUILD)/include),)
                ifneq ($(wildcard $(RCCL_BUILD)/hipify/src),)
                    # All required files and directories exist
                else
                    $(error Directory 'hipify/src' is missing in $(RCCL_BUILD))
                endif
            else
                $(error Directory 'include' is missing in $(RCCL_BUILD))
            endif
        else
            $(error Directory 'hipify' is missing in $(RCCL_BUILD))
        endif
    else
        $(error File 'lib/librccl.so' is missing in $(RCCL_BUILD))
    endif
endif

# Check if ROCM_PATH is provided
ifeq ($(ROCM_PATH),)
    ROCM_PATH = /opt/rocm
else
    ROCM_PATH = $(ROCM_PATH)
endif
LDFLAGS  +=  -L$(ROCM_PATH)/lib \
             -L$(RCCL_BUILD)/build/lib \
             -L/usr/lib64 \
             -L/usr/lib \
             -L$(MPI_LIB_PATH)

INCLUDES +=  -I$(RCCL_BUILD)/include \
    -I$(RCCL_BUILD)/hipify/src \
    -I$(RCCL_BUILD)/hipify/src/include \
    -I$(RCCL_BUILD)/hipify/src/include/plugin \
    -I$(RCCL_BUILD)/build/include \
    -I$(MPI_INCLUDE)

# Ensure build directories exist
$(shell mkdir -p build $(addprefix build/, $(SRC_DIRS)))

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

build/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) -c $< -o $@

# Include the generated dependency files
-include $(DEPS)

all: $(TARGET)

clean:
	rm -rf build

install: all
	@cp $(TARGET) $(ROCM_PATH)/lib
	@echo "Installed $(TARGET) to $(ROCM_PATH)/lib/"

uninstall: clean
	rm -f $(ROCM_PATH)/lib/$(PLUGIN_NAME)
	@echo "Removed $(PLUGIN_NAME) from $(ROCM_PATH)/lib/"

help:
	@echo "Usage: make RCCL_BUILD=/path/to/rccl/build [ROCM_PATH=/path/to/rocm]"
	@echo "	   RCCL_BUILD must be set to the RCCL build directory."
	@echo "	   ANP_TELEMETRY_ENABLED must be set to 1 to build plugin with debug/telemetry."
	@echo "	   Example: make RCCL_BUILD=/home/user/rccl/build ROCM_PATH=/opt/rocm"
	@echo "	   If ROCM_PATH is not provided, the default path /opt/rocm/lib is used."
	@echo "	   If libmpi.so is not found, provide MPI_LIB_PATH=/path/to/libmpi.so"
	@echo "	   If mpi.h is not found, provide MPI_INCLUDE=/path/to/ompi/include"

# Build target for the "bootstrap" binary from src/bootstrap.cc
bootstrap: build/bootstrap.o

build/bootstrap.o:
	g++ -fPIC $(CPPFLAGS) $(INCLUDES) -o bootstrap src/bootstrap.cc src/bootstrap_socket.cc
	@echo "------------------------------------------------------------"
	@echo "Binary 'bootstrap' built successfully!"
	@echo "Usage: ./bootstrap <ip_list_file>"
	@echo "  <ip_list_file> should be a text file with one IP per line."
	@echo "------------------------------------------------------------"

.PHONY: all clean
