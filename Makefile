default:
	g++ lib.cpp -o libcompskyplayaudio.so -fPIC -shared -std=c++20 -O3 -march=native -lavformat -lavcodec -lavutil -lswresample -lpulse-simple -lpulse # -pthread
	g++ lib.cpp main.cpp -o test -g -fsanitize=address -std=c++20 -lavformat -lavcodec -lavutil -lswresample -lpulse-simple -lpulse # -pthread
