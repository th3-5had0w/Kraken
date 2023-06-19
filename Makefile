all:
	gcc main.c -luring -lpthread -o main
	gcc fast.c -luring -o fast
	gcc slow.c -o slow
