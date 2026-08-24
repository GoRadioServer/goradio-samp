# Build the goradio SA-MP plugin.
#
#   make              -> bin/goradio.so, 32-bit (what samp03svr loads)
#   make BITS=64      -> a 64-bit build, for open.mp or for local testing
#   make TLS=1        -> link OpenSSL so https:// audio server URLs work
#   make test         -> host-side tests against a fake audio server
#
# A 32-bit build needs the multilib toolchain:
#   Debian/Ubuntu   apt install g++-multilib
#   Fedora          dnf install glibc-devel.i686 libstdc++-devel.i686

CXX      ?= g++
BITS     ?= 32
TLS      ?= 0
# Baked into the binary and reported in the server log. Release builds
# pass the tag in explicitly; a local build gets whatever git knows.
VERSION  ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
BUILDDIR ?= build
BINDIR   ?= bin
TARGET   ?= $(BINDIR)/goradio.so

SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(patsubst src/%.cpp,$(BUILDDIR)/%.o,$(SOURCES))

CXXFLAGS := -std=c++11 -m$(BITS) -O2 -fPIC -W -Wall -Wextra -Wno-unused-parameter \
            -fvisibility=hidden -Isdk -Isrc -DGORADIO_VERSION=\"$(VERSION)\"
# The server this loads into is often an old box with an old libstdc++;
# linking the C++ runtime in statically is what makes one build work
# across them.
LDFLAGS  := -m$(BITS) -shared -static-libgcc -static-libstdc++ \
            -Wl,--version-script=sdk/goradio.ver
LDLIBS   := -lpthread

ifeq ($(TLS),1)
	CXXFLAGS += -DGORADIO_TLS
	LDLIBS   += -lssl -lcrypto
endif

.PHONY: all clean test dirs docker check-windows check-names

all: dirs $(TARGET)

dirs:
	@mkdir -p $(BUILDDIR) $(BINDIR)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "built $@ ($(BITS)-bit, TLS=$(TLS), version $(VERSION))"

$(BUILDDIR)/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test:
	@sh test/run_tests.sh

# Builds in a container that has the 32-bit toolchain, for hosts that
# don't. Needs BuildKit (Docker 23+, or DOCKER_BUILDKIT=1).
docker:
	docker build --target export --output type=local,dest=$(BINDIR) \
		--build-arg VERSION=$(VERSION) --build-arg BITS=$(BITS) --build-arg TLS=$(TLS) .
	@echo "built $(BINDIR)/goradio.so in a container ($(BITS)-bit, TLS=$(TLS), version $(VERSION))"

# Compiles every source against real Windows headers via MinGW, at both
# widths. Worth running before a release: MSVC is the only thing CI can
# check Windows with, and waiting on it is a slow way to find a typo.
check-windows:
	docker build --target windows-check -t goradio-windows-check .

# Cross-checks the PAWN API surface against the natives table, and against
# PAWN's 31-character symbol limit.
check-names:
	@sh scripts/check-names.sh

clean:
	rm -rf $(BUILDDIR) $(BINDIR) dist
