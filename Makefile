.PHONY: all clean

all: main.cpp
	g++ main.cpp -g3 `pkg-config --cflags --libs sdl2 SDL2_ttf` -std=c++17

clean:
	rm a.out
