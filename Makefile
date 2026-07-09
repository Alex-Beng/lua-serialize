all : serialize

linux : serialize.so

serialize.so : serialize.c lz4.c
	gcc -Wall -g -o $@ -fPIC --shared $^

serialize : serialize.c lz4.c
	gcc -Wall -g -o $@.dll --shared $^ -ID:\Path\Lua53\include -LD:\Path\Lua53\bin -llua53