CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pedantic
LDFLAGS = -lpthread

TARGET = router
SRCS = main.cpp
OBJS = $(SRCS:.cpp=.o)

.PHONY: all clean distclean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)

distclean: clean
	rm -f $(TARGET)