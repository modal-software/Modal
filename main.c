#include "parser.h"
#include "tokenizer/tokenizer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    exit(1);
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  rewind(f);

  char *buf = malloc(size + 1);
  fread(buf, 1, size, f);
  buf[size] = 0;

  fclose(f);
  return buf;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file.mal>\n", argv[0]);
    return 1;
  }

  char *src = read_file(argv[1]);

  Tokenizer t;
  init(&t, src);

  Parser p;
  parser_init(&p, &t, argv[1]);
  parse_program(&p);

  free(src);
  return 0;
}
