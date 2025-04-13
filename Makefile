# // Marceli Buczek 339966

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pedantic
TARGET = router
SRCS = main.cpp
OBJS = $(SRCS:.cpp=.o)
all: $(TARGET)
	sudo setcap cap_net_raw+ep $(TARGET)  # uprawnienia po kompilacji
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS) $(TARGET)
run: $(TARGET)
	./$(TARGET) 8.8.8.8