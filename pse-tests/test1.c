#include <stdio.h>

int main(int argc, char *argv[]) {

  if (argc > 1) {
    printf("blah, blah, blah\n");
  }

  if (argv[0] != NULL) {
    *argv[0] = '\0';
  }
  return 0;
}

