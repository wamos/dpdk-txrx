sudo gdb --args ./build/receiver -l 1 -n 3 -w 04:00.0
#sudo gdb --args ./build/basicfwd --no-huge -m 512 -l 1 -n 3 -w 04:00.0 # without hugepage support
#CFLAGS=-g make
