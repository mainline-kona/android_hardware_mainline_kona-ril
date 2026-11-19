# Incremental host-test build for kebab-rild.
#   make -j$(nproc)                 # ASAN binary (default)
#   make -j$(nproc) SANITIZE=0      # plain binary (fast)
#   make clean
CLANG ?= clang
CCACHE := $(shell command -v ccache 2>/dev/null)
CC := $(if $(CCACHE),$(CCACHE) ,)$(CLANG)

RADIO ?= ..
LOS ?= ../../../../../..
INC := -I$(RADIO) -I$(RADIO)/kebab-rild \
       -I$(LOS)/hardware/ril/include \
       -I$(LOS)/system/logging/liblog/include \
       -I$(LOS)/system/core/libcutils/include \
       -I$(LOS)/system/core/libbase/include
CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-parameter \
          -D_GNU_SOURCE -DRIL_SHLIB $(INC)

SANITIZE ?= 1
ifeq ($(SANITIZE),1)
CFLAGS += -fsanitize=address
BUILD := build/asan
else
BUILD := build/normal
endif

MODS := kebab-ril state uim dms nas wms voice data ims cell dcm
MOD_OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(MODS)))
EXTRA_OBJS := $(BUILD)/qmi.o $(BUILD)/host-stubs.o \
              $(BUILD)/mock-test.o $(BUILD)/smoke.o
OBJS := $(MOD_OBJS) $(EXTRA_OBJS)
BIN := $(BUILD)/kebab_mock

COMPILE = @mkdir -p $(BUILD) && $(CC) $(CFLAGS) -MMD -MP -c $< -o $@

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lpthread

$(BUILD)/%.o: $(RADIO)/kebab-rild/%.c
	$(COMPILE)

$(BUILD)/qmi.o: $(RADIO)/kebab-rild/qmi/qmi.c
	$(COMPILE)

$(BUILD)/host-stubs.o: $(RADIO)/kebab-rild/tests/kebab-rild-host-stubs.c
	$(COMPILE)

$(BUILD)/mock-test.o: $(RADIO)/kebab-rild/tests/kebab-rild-mock-test.c
	$(COMPILE)

$(BUILD)/smoke.o: $(RADIO)/kebab-rild/tests/kebab-rild-smoke.c
	$(COMPILE)

-include $(OBJS:.o=.d)

clean:
	rm -rf build

.PHONY: all clean
