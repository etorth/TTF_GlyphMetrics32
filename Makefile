.PHONY: all clean

all: main.cpp
	g++ main.cpp `pkg-config --cflags --libs sdl2 SDL2_ttf` -std=c++17

clean:
	rm a.out
