all :  player

clean : 
	rm -rf player *.o

player : player.c
	gcc -o player player.c -lavformat -lavcodec -lswscale  -lz -lm `sdl-config --cflags --libs` -lavutil
