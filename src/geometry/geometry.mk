SRCS := $(wildcard *.c)

$(PROG):	$(SRCS)
	$(CC) $(CFLAGS) -o $(PROG) $(SRCS) $(LIBS) $(INCLUDE_PATHS) $(LIBRARY_PATHS)
	@echo $(SRCS)

clean:
	rm -f $(PROG)
