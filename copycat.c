/*----- includes -----*/
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/ioctl.h> // For window size

/*----- data -----*/
struct editorConfig{
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;

/*----- defines -----*/
// CTRL key assigns 5 and 6 bit to 0 and then send it
#define CTRL_KEY(k) ((k) & 0x1f)

/*----- terminal -----*/
void die(const char* s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1){
        die("tcsetattr");
    }
}

void enableRawMode(){
    // read the current attributes
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");

    // automatically disable raw mode on exit
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    
    //             modify the attributes in flags
    // IXON: Disable software control flow using Ctrl+S and Ctrl+Q
    // ICRNL: Turn off Carraige Return/New Line thing (Fix Ctrl+M)
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // ICANON to turn off Canonical Mode off ie. read byte-by-byte
    //     so the program quits as soon as q is pressed
    // ISIG to turn off Ctrl+Z Ctr+C signals
    // IEXTEN for Ctrl+V
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    // OPOST: turn off output processing
    //      because of this flag, use \r\n everywhere for a new line
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    // read() returns 0 if no input given in 1/10 sec
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 20;

    // update the attributes
    // TCSAFLUSH discards any unread input before applying changes to
    //    terminal
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

char editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }
  return c;
}

int getCursorPosition(int *rows, int *cols){
    char buf[35];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;
    
    while (i < sizeof(buf) - 1){
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i]='\0';
    printf("\r\n&buf[1]: '%s'\r\n", &buf[1]);
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) 
            return -1;
        getCursorPosition(rows, cols);
        return -1;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*----- append buffer -----*/
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len){
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL){
        return;
    }
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab){
    free(ab->b);
}
/*----- output -----*/
void editorDrawRows(struct abuf *ab){
    int y;
    for(; y < E.screenrows; y++){
        abAppend(ab, "~", 1);

        if ( y < E.screenrows - 1)
            abAppend(ab, "\r\n", 2);
    }    
}

void editorRefreshScreen(){
    struct abuf ab = ABUF_INIT;
    // Erase everything on screen
    // \x1b is the escape character (27 in decimal)
    // followed by a [ character.
    // J command is Erase in Display
    // For other commands see https://vt100.net/docs/vt100-ug/chapter3.html#ED
    // 2 is the argument for J: erase eveything on screen
    // 4 is the number of bytes to be written
    abAppend(&ab, "\x1b[2J", 4);

    // Reposition the cursor to the first row and col
    // H command is for the cursor
    // \x1b[15;45H is center on a 30x90 terminal
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    abAppend(&ab, "\x1b[H", 3);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*----- input -----*/
void editorProcessKeyPress(){
    char c = editorReadKey();
    switch (c){
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
    }
}

/*----- init -----*/
void initEditor(){
    if(getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
}

int main(){
    enableRawMode();
    initEditor();
    // printf("Copycat: A text editor in C\r\nPress Ctrl+Q to exit\r\nAuthor:github@khizirsiddiqui\r\n\r\n");
    while(1){
        editorRefreshScreen();
        editorProcessKeyPress();
    }
    return 0;
}