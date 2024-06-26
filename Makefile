# $@: name of the target file (one before colon)
# $<: name of first prerequisite file (first one after colon)
# $^: names of all prerequisite files (space separated)
# $*: stem (bit which matches the % wildcard in rule definition)
#
# VAR = val: Normal setting - values within are recursively expand when var used.
# VAR := val: Setting of var with simple expansion of values inside - values are expanded at decl time.
# VAR ?= val: Set var only if it doesn't have a value.
# VAR += val: Append val to existing value (or set if var didn't exist).

# To use static analyzer:
#   http://clang-analyzer.llvm.org/scan-build.html
# Ie:
#   scan-build -k -V --use-analyzer ~/bin/clang make

NAME = inotify-info

CFG ?= release
ifeq ($(CFG), debug)
    ASAN ?= 1
endif

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

LD = $(CC)
RM = rm -f
MKDIR = mkdir -p
VERBOSE ?= 0

INOTIFYINFO_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo unknown)

COMPILER = $(shell $(CC) -v 2>&1 | grep -q "clang version" && echo clang || echo gcc)

WARNINGS = -Wall -Wextra -Wpedantic -Wmissing-include-dirs -Wformat=2 -Wshadow
ifneq ($(COMPILER),clang)
  # https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html
  WARNINGS += -Wsuggest-attribute=format -Wall
endif

DEFINES  = -D_LARGEFILE64_SOURCE=1 -D_FILE_OFFSET_BITS=64
DEFINES += -DINOTIFYINFO_VERSION=\"$(INOTIFYINFO_VERSION)\"

CFLAGS += $(WARNINGS) $(DEFINES)
CFLAGS += -std=gnu99 -fno-exceptions

CXXFLAGS += $(WARNINGS) $(DEFINES)
CXXFLAGS += -std=c++11 -fno-exceptions -fno-rtti -Woverloaded-virtual

LIBS = -Wl,--no-as-needed -lm -ldl -lpthread -lstdc++

CFILES = \
	inotify-info.cpp \
	lfqueue/lfqueue.c

# Useful GCC address sanitizer checks not enabled by default
# https://kristerw.blogspot.com/2018/06/useful-gcc-address-sanitizer-checks-not.html

ifeq ($(ASAN), 1)
	# https://gcc.gnu.org/gcc-5/changes.html
	#  -fsanitize=float-cast-overflow: check that the result of floating-point type to integer conversions do not overflow;
	#  -fsanitize=vptr: enable checking of C++ member function calls, member accesses and some conversions between pointers to base and derived classes, detect if the referenced object does not have the correct dynamic type.
	ASAN_FLAGS = -fno-omit-frame-pointer -fno-optimize-sibling-calls
	ASAN_FLAGS += -fsanitize=address # fast memory error detector (heap, stack, global buffer overflow, and use-after free)
	ASAN_FLAGS += -fsanitize=leak # detect leaks
	ASAN_FLAGS += -fsanitize=undefined # fast undefined behavior detector
	ASAN_FLAGS += -fsanitize=float-divide-by-zero # detect floating-point division by zero;
	ASAN_FLAGS += -fsanitize=bounds # enable instrumentation of array bounds and detect out-of-bounds accesses;
	ASAN_FLAGS += -fsanitize=object-size # enable object size checking, detect various out-of-bounds accesses.
	ASAN_FLAGS += -fsanitize=alignment # enable alignment checking, detect various misaligned objects;
	CFLAGS += $(ASAN_FLAGS)
	CXXFLAGS += $(ASAN_FLAGS)
	LDFLAGS += $(ASAN_FLAGS)
endif

ifeq ($(CFG), debug)
	ODIR=_debug
	CFLAGS += -O0 -DDEBUG
	CFLAGS += -D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC -D_GLIBCXX_SANITIZE_VECTOR -D_LIBCPP_DEBUG=1 -D_LIBCPP_ENABLE_DEBUG_MODE=1
else
	ODIR=_release
	CFLAGS += -O2 -DNDEBUG
endif

PROJ = $(ODIR)/$(NAME)

ifeq ($(VERBOSE), 1)
	VERBOSE_PREFIX=
else
	VERBOSE_PREFIX=@
endif

C_OBJS = ${CFILES:%.c=${ODIR}/%.o}
OBJS = ${C_OBJS:%.cpp=${ODIR}/%.o}

all: $(PROJ)

$(ODIR)/$(NAME): $(OBJS)
	@echo "Linking $@...";
	$(VERBOSE_PREFIX)$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

-include $(OBJS:.o=.d)

$(ODIR)/%.o: %.c Makefile
	$(VERBOSE_PREFIX)echo "---- $< ----";
	@$(MKDIR) $(dir $@)
	$(VERBOSE_PREFIX)$(CC) -MMD -MP $(CFLAGS) -o $@ -c $<

$(ODIR)/%.o: %.cpp Makefile
	$(VERBOSE_PREFIX)echo "---- $< ----";
	@$(MKDIR) $(dir $@)
	$(VERBOSE_PREFIX)$(CXX) -MMD -MP $(CXXFLAGS) -o $@ -c $<

.PHONY: lint
lint:
	find . -name '*.h' -o -name '*.c' -o -name '*.cpp' | xargs clang-format -i --style=webkit

.PHONY: clean

clean:
	@echo Cleaning...
	$(VERBOSE_PREFIX)$(RM) $(PROJ)
	$(VERBOSE_PREFIX)$(RM) $(OBJS)
	$(VERBOSE_PREFIX)$(RM) $(OBJS:.o=.d)
	$(VERBOSE_PREFIX)$(RM) $(OBJS:.o=.dwo)

.PHONY: install

install: all
	install -D $(PROJ) $(BINDIR)/$(NAME)

.PHONY: uninstall

uninstall:
	$(RM) $(BINDIR)/$(NAME)

define RELEASE_RULES
inotify-info-$(TAG).tar.gz:
	git archive --prefix=inotify-info-$(TAG)/ v$(TAG) | gzip -n > $$@
endef
$(foreach TAG,$(shell git tag 2>/dev/null | sed -n '/^v/ s/^v//p'),$(eval $(RELEASE_RULES)))
