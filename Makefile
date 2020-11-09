CC = g++
prog1 = np_simple
prog2 = np_single_proc

all: $(prog1) $(prog2)
$(prog1): np_simple.cpp
	$(CC) -o $@ $^
$(prog2): np_single_proc.cpp
	$(CC) -o $@ $^

clean:
	rm $(prog1) $(prog2)
