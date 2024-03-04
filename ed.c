#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>


struct termios orig_termios;

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
    // obtain a copy of terminal flags at call, and restore it when program exits 
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);

    struct termios raw;

    // obtain terminal attributes using termios.h, and read them into a termios struct using tcgetattr

    tcgetattr(STDIN_FILENO, &raw);

    raw.c_lflag &= ~(ECHO | ICANON);

    // after modifying flags, apply them to the terminal using tcsetattr

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
    enableRawMode();

    char c;
    // while there are bytes of input from STDIN to read 
    while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {
        if (iscntrl(c)) { // if a character is a control character (nonprintable)
            printf("%d\n", c);
        } else {
            printf("%d ('%c')\n", c, c); // %d writes c out as a byte, %c as a char 
        }
    }
    return 0;
}