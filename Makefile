all:
	gcc main.c -luring -lpthread -o main -Wno-format-truncation
	gcc fast.c -luring -o fast
	gcc slow.c -o slow -Wno-incompatible-pointer-types

benchmark:
	./fast &
	./slow &
