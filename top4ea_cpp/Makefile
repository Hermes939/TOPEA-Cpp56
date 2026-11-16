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
DEPFLAGS  := -MMD -MP
CURL_CONFIG := $(shell command -v curl-config 2>/dev/null)
ifeq ($(CURL_CONFIG),)
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS   := $(shell pkg-config --libs libcurl 2>/dev/null)
CURL_LIBDIR := $(shell pkg-config --variable=libdir libcurl 2>/dev/null)
else
CURL_CFLAGS := $(shell $(CURL_CONFIG) --cflags)
CURL_LIBS   := $(shell $(CURL_CONFIG) --libs)
CURL_LIBDIR := $(shell $(CURL_CONFIG) --prefix)/lib
endif
CXXFLAGS += $(CURL_CFLAGS)
LDFLAGS   := -pthread -ldl $(CURL_LIBS) -Wl,-rpath,$(CURL_LIBDIR)

SRCDIR    := src
BUILDDIR  := build

## ── Sources DLL (toute la logique, sans loader ni main) ──────
DLL_SRCS  := $(SRCDIR)/metaapi_bridge.cpp       \
	     $(SRCDIR)/tickerall_bridge.cpp      \
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
     $(SRCDIR)/recycling_agents.cpp      \
	     $(SRCDIR)/top4ea_engine.cpp         \
	     $(SRCDIR)/top4ea_dll.cpp

## ── Source loader (thin main) ─────────────────────────────────
LOADER_SRC := $(SRCDIR)/loader.cpp

## ── Objets DLL (.o avec -fPIC) ──────────────────────────────
DLL_OBJS   := $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/dll_%.o, $(DLL_SRCS))
DLL_DEPS   := $(DLL_OBJS:.o=.d)

## ── Objet loader ─────────────────────────────────────────────
LOADER_OBJ := $(BUILDDIR)/loader.o
LOADER_DEP := $(LOADER_OBJ:.o=.d)

## ── Outputs ──────────────────────────────────────────────────
LIB_TARGET    := $(BUILDDIR)/libtop4ea.so
LOADER_TARGET := $(BUILDDIR)/top4ea

.PHONY: all lib loader validate-calibration check clean run test-grid-units test-recycling test-tickerall test-basket-manager

all: lib loader validate-calibration
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
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -fPIC -c $< -o $@

## ── Loader thin ──────────────────────────────────────────────
loader: $(LOADER_TARGET)

## ── Validation forward de l'impact de calibration ─────────────
validate-calibration:
	python3 ../tests/test_calibration_impact.py \
		--csv data/error_log.csv \
		--params automl/output/params.json

test-grid-units: $(BUILDDIR)/test_grid_engine_pips
	./$(BUILDDIR)/test_grid_engine_pips

test-recycling: $(BUILDDIR)/test_recycling_agents
	./$(BUILDDIR)/test_recycling_agents

test-tickerall: $(BUILDDIR)/test_tickerall_bridge
	env -u TICKERALL_API_KEY ./$(BUILDDIR)/test_tickerall_bridge

test-basket-manager: $(BUILDDIR)/test_basket_manager
	./$(BUILDDIR)/test_basket_manager

$(BUILDDIR)/test_basket_manager: tests/test_basket_manager.cpp $(SRCDIR)/basket_manager.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILDDIR)/test_tickerall_bridge: tests/test_tickerall_bridge.cpp $(SRCDIR)/tickerall_bridge.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILDDIR)/test_recycling_agents: tests/test_recycling_agents.cpp $(SRCDIR)/recycling_agents.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILDDIR)/test_grid_engine_pips: tests/test_grid_engine_pips.cpp $(SRCDIR)/grid_engine.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

check: all

$(LOADER_TARGET): $(LOADER_OBJ) $(LIB_TARGET) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $< -o $@ -L$(BUILDDIR) -ltop4ea -Wl,-rpath,'$$ORIGIN/../build' $(LDFLAGS)
	@echo "Loader OK : $@"

$(LOADER_OBJ): $(LOADER_SRC) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

-include $(DLL_DEPS) $(LOADER_DEP)

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
