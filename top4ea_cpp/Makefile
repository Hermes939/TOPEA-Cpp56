CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pthread -I include
LDFLAGS  := -pthread -lcurl
SRCDIR   := src
BUILDDIR := build
TARGET   := $(BUILDDIR)/top4ea

SRCS := $(SRCDIR)/main.cpp \
        $(SRCDIR)/metaapi_bridge.cpp \
        $(SRCDIR)/grid_engine.cpp \
        $(SRCDIR)/basket_manager.cpp \
        $(SRCDIR)/recovery_engine.cpp \
        $(SRCDIR)/top4ea_engine.cpp

OBJS := $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/%.o, $(SRCS))

.PHONY: all clean run

all: $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "✓ Build réussi: $(TARGET)"

clean:
	rm -rf $(BUILDDIR)

run: all
	./$(TARGET) top4ea.conf
