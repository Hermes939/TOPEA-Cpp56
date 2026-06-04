## ============================================================
##  TOP4EA — Makefile  C++20  Architecture 100% DLL
##
##  Targets :
##    all          → libtop4ea.so + top4ea (loader)
##    lib          → libtop4ea.so (DLL seulement)
##    loader       → top4ea (loader thin seulement)
##    clean        → supprime build/
##    run          → compile + lance top4ea
## ============================================================

CXX       := g++
CXXFLAGS  := -std=c++20 -O2 -Wall -Wextra -pthread -I include
LDFLAGS   := -pthread -lcurl -ldl

SRCDIR    := src
BUILDDIR  := build

## ── Sources DLL (toute la logique, sans loader ni main) ──────
DLL_SRCS  := $(SRCDIR)/metaapi_bridge.cpp       \
	     $(SRCDIR)/grid_engine.cpp           \
	     $(SRCDIR)/basket_manager.cpp        \
	     $(SRCDIR)/recovery_engine.cpp       \
	     $(SRCDIR)/profit_lock_manager.cpp   \
	     $(SRCDIR)/intelligence_engine.cpp   \
	     $(SRCDIR)/geometric_imbalance.cpp   \
	     $(SRCDIR)/institutional_delta.cpp   \
	     $(SRCDIR)/error_memory.cpp          \
	     $(SRCDIR)/math_calibrator.cpp       \
	     $(SRCDIR)/deepseek_vision.cpp       \
	     $(SRCDIR)/dxy_analyzer.cpp          \
	     $(SRCDIR)/market_intelligence.cpp   \
	     $(SRCDIR)/grid_slot_agent.cpp       \
	     $(SRCDIR)/agent_orchestrator.cpp    \
	     $(SRCDIR)/top4ea_engine.cpp         \
	     $(SRCDIR)/top4ea_dll.cpp

## ── Source loader (thin main) ─────────────────────────────────
LOADER_SRC := $(SRCDIR)/loader.cpp

## ── Objets DLL (.o avec -fPIC) ──────────────────────────────
DLL_OBJS   := $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/dll_%.o, $(DLL_SRCS))

## ── Objet loader ─────────────────────────────────────────────
LOADER_OBJ := $(BUILDDIR)/loader.o

## ── Outputs ──────────────────────────────────────────────────
LIB_TARGET    := $(BUILDDIR)/libtop4ea.so
LOADER_TARGET := $(BUILDDIR)/top4ea

.PHONY: all lib loader clean run

all: lib loader
	@echo ""
	@echo "TOP4EA DLL build OK"
	@echo "  $(LIB_TARGET)"
	@echo "  $(LOADER_TARGET)"

## ── Shared library (DLL) ─────────────────────────────────────
lib: $(LIB_TARGET)

$(LIB_TARGET): $(DLL_OBJS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS)
	@echo "DLL OK : $@"

## ── DLL objects (Position Independent Code) ─────────────────
$(BUILDDIR)/dll_%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

## ── Loader thin ──────────────────────────────────────────────
loader: $(LOADER_TARGET)

$(LOADER_TARGET): $(LOADER_OBJ) $(LIB_TARGET) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $< -o $@ -L$(BUILDDIR) -ltop4ea -Wl,-rpath,'$$ORIGIN/../build' $(LDFLAGS)
	@echo "Loader OK : $@"

$(LOADER_OBJ): $(LOADER_SRC) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

## ── Répertoire de build ──────────────────────────────────────
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

## ── Nettoyage ────────────────────────────────────────────────
clean:
	rm -rf $(BUILDDIR)
	@echo "Nettoyé"

## ── Lancement ────────────────────────────────────────────────
run: all
	LD_LIBRARY_PATH=$(BUILDDIR):$$LD_LIBRARY_PATH $(LOADER_TARGET) top4ea.conf
