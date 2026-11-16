CXX      ?= g++
STD      := -std=c++17
WARN     := -Wall -Wextra -Wpedantic
OPTFLAGS := -O2
DBGFLAGS := -O0 -g

VERSION  := 1.1.0
TARGET   := ponypush
TESTBIN  := tests/test_runner
TESTBIN_SAN := tests/test_runner_san

# Alvo do sistema: alvo nativo por padrao; passe TARGET_OS=Windows_NT ao
# compilar com mingw-w64 (x86_64-w64-mingw32-g++ / i686-w64-mingw32-g++).
TARGET_OS ?= $(shell uname -s)

# Windows (MinGW): binario 100% estatico com transporte WinHTTP nativo.
# -mwindows marca o subsystem GUI: rodando como tarefa agendada o processo
# nao cria janela de console (nao pisca). O uso interativo reaproveita o
# console do pai via AttachConsole em main.cpp.
# POSIX: transporte libcurl dinamico (libcurl4-openssl-dev).
ifeq ($(TARGET_OS),Windows_NT)
  LIBS := -static -static-libgcc -static-libstdc++ -mwindows -lwinhttp -lws2_32 -Wl,--no-insert-timestamp
else
  LIBS := -lcurl
endif

CXXFLAGS := $(STD) $(WARN)

SRCS := main.cpp ponypush.cpp config.cpp selfupdate.cpp

.PHONY: all check test sanitize version install clean

all: $(TARGET)

$(TARGET): $(SRCS) ponypush.hpp selfupdate.hpp
	$(CXX) $(CXXFLAGS) $(OPTFLAGS) -DPONYPUSH_VERSION=\"$(VERSION)\" -o $@ $(SRCS) $(LIBS)

# Verificacao estatica: warnings tratados como erro
check:
	$(CXX) $(CXXFLAGS) -Werror -fsyntax-only $(SRCS)
	@echo "verificacao ok (zero warnings)"

$(TESTBIN): tests/ponypush_tests.cpp ponypush.cpp config.cpp selfupdate.cpp ponypush.hpp tests/test_framework.hpp
	$(CXX) $(CXXFLAGS) $(DBGFLAGS) -I. -DPONYPUSH_VERSION=\"$(VERSION)\" -o $@ tests/ponypush_tests.cpp ponypush.cpp config.cpp selfupdate.cpp $(LIBS)

test: $(TESTBIN)
	./$(TESTBIN)

# Build com sanitizers (AddressSanitizer + UndefinedBehaviorSanitizer)
$(TESTBIN_SAN): tests/ponypush_tests.cpp ponypush.cpp config.cpp selfupdate.cpp ponypush.hpp tests/test_framework.hpp
	$(CXX) $(CXXFLAGS) $(DBGFLAGS) -I. -fsanitize=address,undefined -fno-omit-frame-pointer -DPONYPUSH_VERSION=\"$(VERSION)\" -o $@ tests/ponypush_tests.cpp ponypush.cpp config.cpp selfupdate.cpp $(LIBS)

sanitize: $(TESTBIN_SAN)
	./$(TESTBIN_SAN)

version:
	@echo "$(TARGET) v$(VERSION)"

# Padrão na pasta do usuário (sem sudo), para o `ponypush update` conseguir se
# substituir. Outro destino: sudo make install BINDIR=/usr/local/bin
BINDIR ?= $(HOME)/.local/bin

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/ponypush

clean:
	rm -f $(TARGET) $(TARGET).exe $(TESTBIN) $(TESTBIN_SAN)
