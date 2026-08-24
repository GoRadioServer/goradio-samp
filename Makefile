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

# A TLS build links OpenSSL statically by default. The alternative is a
# plugin that needs a matching libssl on the target box, and SA-MP servers
# are frequently old machines with an OpenSSL that is older still -- a
# release binary has to carry its own. Set OPENSSL_STATIC=0 to link
# against the system OpenSSL instead.
OPENSSL_STATIC ?= 1
UNAME_S := $(shell uname -s)

ifeq ($(TLS),1)
	CXXFLAGS += -DGORADIO_TLS
	ifeq ($(OPENSSL_STATIC),1)
		ifeq ($(UNAME_S),Darwin)
			# Apple's linker has no -Bstatic/-Bdynamic pair.
			LDLIBS += -lssl -lcrypto
		else
			LDLIBS += -Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic -ldl
		endif
	else
		LDLIBS += -lssl -lcrypto
	endif
endif

.PHONY: all clean test dirs docker check-windows check-names check-exports

all: dirs $(TARGET)

dirs:
	@mkdir -p $(BUILDDIR) $(BINDIR)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "built $@ ($(BITS)-bit, TLS=$(TLS), version $(VERSION))"
	@nm -D --defined-only $@ | wc -l | xargs -I{} sh -c 'test {} -eq 6 || \
		{ echo "expected 6 exported symbols, got {}:" >&2; nm -D --defined-only $@ >&2; exit 1; }'

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

# Tests the Windows DLL export checker against captured dumpbin output.
# Needs PowerShell; falls back to Microsoft's container image, which is
# how it gets run on a machine that has neither PowerShell nor Windows.
check-exports:
	@if command -v pwsh >/dev/null 2>&1; then \
		pwsh -File test/test-dll-exports.ps1; \
	else \
		echo "no local pwsh; running in mcr.microsoft.com/powershell"; \
		docker run --rm -v "$(CURDIR):/src" -w /src \
			mcr.microsoft.com/powershell:latest pwsh -File test/test-dll-exports.ps1; \
	fi

clean:
	rm -rf $(BUILDDIR) $(BINDIR) dist
