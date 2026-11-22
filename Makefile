ezcast_mirror: ezcast_mirror.c
	gcc -O3 -march=native ezcast_mirror.c -o ezcast_mirror -lX11 -lXext -lXfixes -lturbojpeg

clean:
	rm -f ezcast_mirror