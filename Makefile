PLUGIN_NAME = librccl-net.so
TARGET = build/$(PLUGIN_NAME)
CC = /opt/rocm/bin/hipcc
CFLAGS = -fPIC -g -O2 -O3 -DNDEBUG -Werror -MMD -MP
CFLAGS += -DTARGET_PLUGIN -DNCCL_BUILD_RDMA_CORE
INCLUDES = -Iinclude \
           -I/opt/rocm/include \
           -I/usr/include

CPPFLAGS = -D__HIP_PLATFORM_AMD__
LDFLAGS = -shared -pthread -L/usr/lib64 -L/usr/lib -L$(MPI_LIB_PATH) -libverbs -lmpi -lionic

SRC_DIRS = src src/misc
EXCLUDE_FILE =

SRCS = $(filter-out $(EXCLUDE_FILE), $(wildcard $(addsuffix /*.cc, $(SRC_DIRS))))
OBJS = $(patsubst %.cc, build/%.o, $(SRCS))
DEPS = $(patsubst %.cc, build/%.d, $(SRCS))

ifeq ($(ANP_DEBUG), 1)
  CFLAGS+= -DANP_DEBUG_ENABLED
else
  CFLAGS+= -UANP_DEBUG_ENABLED
endif

# Check if RCCL_BUILD is provided
ifeq ($(RCCL_BUILD),)
  ifneq ($(filter clean help uninstall,$(MAKECMDGOALS)),)
    # Skip the error check for 'clean', 'uninstall' and 'help'
  else
    $(error RCCL_BUILD is not set. Please specify the RCCL build folder using 'make RCCL_BUILD=/path/to/rccl/build'.)
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
LDFLAGS  +=  -L$(ROCM_PATH)/lib

INCLUDES +=  -I$(RCCL_BUILD)/include \
			 -I$(RCCL_BUILD)/hipify/src \
			 -I$(RCCL_BUILD)/hipify/src/include \
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
	@echo "       RCCL_BUILD must be set to the RCCL build directory."
	@echo "       ANP_DEBUG must be set to 1 to build plugin with debug/telemetry."
	@echo "       Example: make RCCL_BUILD=/home/user/rccl/build ROCM_PATH=/opt/rocm"
	@echo "       If ROCM_PATH is not provided, the default path /opt/rocm/lib is used."
	@echo "       If libmpi.so is not found, provide MPI_LIB_PATH=/path/to/libmpi.so"
	@echo "       If mpi.h is not found, provide MPI_INCLUDE=/path/to/ompi/include"

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
