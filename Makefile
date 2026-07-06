.PHONY = all clean

CC ?= gcc
CFLAGS ?= -Wall -std=c99 -fPIC -fPIE -g -rdynamic -O0

INCLUDES := -Ithirdparty/libffi-3.5.2/include \
			-Ithirdparty/wayland-libs-1.24.0/include \
			-Ithirdparty/wayland-protocols \
			-Ithirdparty/xkbcommon-1.13.1/include

STATIC_LIBS := thirdparty/wayland-libs-1.24.0/libwayland-egl.a \
		thirdparty/wayland-libs-1.24.0/libwayland-client.a \
		thirdparty/libffi-3.5.2/libffi.a \
		thirdparty/xkbcommon-1.13.1/libxkbcommon.a

DYNAMIC_LIBS := -lEGL -lGLESv2

all: compile_commands.json test

test: obj/test.o libshimpl.a $(STATIC_LIBS)
	$(CC) $(CFLAGS) $^ -o $@ $(DYNAMIC_LIBS)

libshimpl.a: obj/shimpl-backend-linux.o
	ar rcs libshimpl.a $^

obj/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

compile_commands.json: $(SRCS) Makefile
	compiledb -nf make

clean:
	rm -rf obj/
	rm -f test
	rm -f libshimpl.a
	rm -rf .cache/
	rm -f compile_commands.json
