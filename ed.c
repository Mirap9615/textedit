// sectioning can be declared as thus
/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

// this macro bitwise-ANDs a character with the value 0011111 in binary. Thus it sets the upper 3 bits of the character to 0, the same thing that ctrl does. 
#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

struct editorConfig {
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s) {
    // standard procedure to clear the screen (J) and reset cursor position (H)
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // most C library functions that fail will set the global errno variable to indicate what that error is. perror will take that error and print out a descriptive 
    // message for it. 
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E) == -1) die("tcsetattr");
}

void enableRawMode() {
    // obtain a copy of terminal flags at call, and restore it when program exits 
    if (tcgetattr(STDIN_FILENO, &E) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    // obtain terminal attributes using termios.h, and read them into a termios struct using tcgetattr

    tcgetattr(STDIN_FILENO, &raw);

    // bitwise or BRKINT, INPCK, ISTIP: Vanity 
    // bitwise or ICRNL: turns off auto-translation of carriage returns (13, 'r') into newlines (10, '\n')
    // bitwise or IXON: turns off software flow control, allowing ctrl-S and ctrl-Q to be inputted 

    raw.c_lflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // bitwise or OPOST: turns off all output processing features, most notably the "\n" to "\r\n" feature, which is turned on by default
    raw.c_lflag &= ~(OPOST);

    // sets character size to 8 bits per byte 
    raw.c_lflag |= (CS8);

    // bitwise or ECHO: removes ECHO ability
    // bitwise or ICANON: turns off canonical mode, allowing us to read byte-by-byte instead of line-by-line
    // bitwise or IEXTEN: turns off ctrl-V, and fixed ctrl-O in macOS 
    // bitwise or ISIG: removes the transferance of keyboard input signals. turns off SIGINT by ctrl-C and SIGTSTP by ctrl-Z; also disables Ctrl-Y on mac. 

    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // VMIN: sets minimum number of bytes of input before read() can return
    // VMAX: sets the maximum amount of time to wait before read() returns

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    // after modifying flags, apply them to the terminal using tcsetattr

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// editorReadKey() belongs in the terminal section because it deals with low level terminal input, while editorProcessKeyPress deals with
// mapping keys to editor functions at a much higher level. 
char editorReadKey() {
    int nread;
    char c;
    // if read() times out it returns -1 with an error no of EAGAIN instead of just returning 0 like it's supposed to. So we won't treat EAGAIN as an error. 
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read"); 
    }
    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    // another reminder that printf() expects strings to end with a 0 byte, which must be added to the end of a string buffer
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return -1;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 ||ws.ws_col == 0) {
        // the hard way
        if (write(STDOUT_FILENO, "\x1b[99C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        // the easy way 
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

// an append buffer consists of a pointer to our buffer in memory and a length. the ABUF_INIT constant represents an empty buffer, acting as a constructor for abuf. 
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    // to append a string s to an abuf, we first ask realloc to give us a block of memory that is the size of the current string plus the size of s. 
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    // copies the string s after the end of the current data in the buffer, then update fields for the abuf 
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

// deallocates the dynamic memory used by an abuf 
void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        abAppend(ab, "~", 1);

        abAppend(ab, "\x1b[k", 3);
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);

    /* write and STDOUT_FILENO comes from unistd.h. 4 is the number of bytes written out to the terminal, and \x1b is the escape character, 27 in decimal. 
    the other thee bytes, [2J, is part of an escape sequence. Escape sequences instruct the terminal to do various text formatting tasks, and always start with an
    escape character followed by a [ character. the J command (Erase in Display) clears the screen, while the argument 2 says to clear the entire screen. 1 would 
    clear the screen up to where the cursor is, and 0 would clear the screen from the cursor up to the end of the screen. 0 is the default argument for J.
    Mostly be using VT100 escape sequences. */
    // write(STDOUT_FILENO, , 4); pre buffer <-, post buffer: 
    // abAppend(&ab, "\x1b[2J", 4); replaces with abAppend(ab, "\x1b[k", 3) in editorDrawRows (entire screen -> one line)
    
    // H (cursor position) actually takes two arguments: [cow;col]. They start at 1, not 0. 
    // write(STDOUT_FILENO, "\x1b[H", 3); pre buffer <-, post buffer: 
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, "\x1b[?25h", 6);

    // after drawing, we reposition the cursor back up at the top-left corner. 
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editorProcessKeypress() {
    char c = editorReadKey();
    switch (c) {
        case CTRL_KEY('q'):
            // standard procedure to clear the screen (J) and reset cursor position (H)
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
        
            exit(0);
            break;
    }
}
/*** init ***/

void initEditor() {
    // job is to initiaize the rows and cols attributes in the E struct. This is the easy way. 
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}