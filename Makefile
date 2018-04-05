
all: xwiilua.so

xwiilua.so: xwiilua.c
	$(CC) -shared -fPIC -o $@ $< $(shell pkg-config --cflags --libs libxwiimote) $(shell pkg-config --cflags --libs lua)

clean:
	rm -f xwiilua.so
