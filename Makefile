ttf_demo: main.cpp
	g++ main.cpp -o ttf_demo `pkg-config --cflags --libs sdl2 SDL2_ttf` -std=c++17
