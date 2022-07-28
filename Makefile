CXXFLAGS:=-O3 -Wall -Wextra -std=c++20 -march=native # -MJ compile_commands.json
LDFLAGS:=-lpthread -lboost_iostreams -lboost_program_options
PYTHON_EXT=$(shell python3-config --extension-suffix)
PYTHON_LIBS:=analysis
PYTHON_BINDING=$(addsuffix $(PYTHON_EXT),$(PYTHON_LIBS))

.DEFAULT_GOAL=all

T=sender $(PYTHON_BINDING)
HEADERS=echo_test.hpp

sender: $(HEADERS)
$(PYTHON_BINDING): $(HEADERS)

all: $(T)

.PHONY: clean
clean:
	rm -f $(T)

%$(PYTHON_EXT): %.cpp
	$(CC) $(CXXFLAGS) $(LDFLAGS) -shared -fPIC $(shell python3 -m pybind11 --includes) $< -o $@


debug:
	g++ sender.cpp -o sender -ggdb -Og -Wall -Wextra -std=c++20 $(LDFLAGS) -fsanitize=address,undefined,leak # thread
