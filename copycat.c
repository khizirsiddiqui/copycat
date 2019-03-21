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
    // Cursor position
    int cx;
    int cy;

    // Screen Dimensions
    int screenrows;
    int screencols;

    // Terminal Identity
    struct termios orig_termios;
};

struct editorConfig E;

/*----- defines -----*/
#define COPYCAT_VERSION "0.0.1"
// CTRL key assigns 5 and 6 bit to 0 and then send it
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,

    DEL_KEY,

    HOME_KEY,   // Fn + Left Arrow
    END_KEY,    // Fn + Right Arrow
    PAGE_UP,     // Also works for Fn+Up Arrow
    PAGE_DOWN    // Also works for Fn+Down Arrow
};

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
    raw.c_cc[VTIME] = 1;

    // update the attributes
    // TCSAFLUSH discards any unread input before applying changes to
    //    terminal
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
      char seq[3];
      if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
      if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

      if (seq[0] == '[') {
          if (seq[1] >= '0' && seq[1] <= '9') {
              if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
              if (seq[2] == '~') {
                  switch (seq[1]) {
                      case '3': return DEL_KEY;
                      case '1':
                      case '7': return HOME_KEY;
                      case '4':
                      case '8': return END_KEY;

                      case '5': return PAGE_UP;
                      case '6': return PAGE_DOWN;
                  }
              }
          } else {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;

                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
          }
      } else if (seq[0] == 'O') {
          switch (seq[1]) {
              case 'H': return HOME_KEY;
              case 'F': return END_KEY;
          }
      }
      return '\x1b';      
  } else {
      return c;
  }

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
    int y=0;
    for(; y < E.screenrows; y++){
        if (y == E.screenrows / 4){
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome),
                "COPYCAT: A light Text-Editor in C Ver(%s)", COPYCAT_VERSION);
            if (welcomelen > E.screencols)
                welcomelen = E.screencols;
            int padding = (E.screencols - welcomelen) / 2;
            if (padding) {
                abAppend(ab, "~", 1);
                padding--;
            }
            while (padding--)
                abAppend(ab, " ", 1);
            abAppend(ab, welcome, welcomelen);
        } else {
            abAppend(ab, "~", 1);
        }
        // Erase the part of line to the right of curson
        abAppend(ab, "\x1b[K", 3);
        if ( y < E.screenrows - 1)
            abAppend(ab, "\r\n", 2);
    }    
}

void editorRefreshScreen(){
    struct abuf ab = ABUF_INIT;

    // Hide cursor when refreshing
    abAppend(&ab, "\x1b[?25l", 6);
    // Erase everything on screen
    // \x1b is the escape character (27 in decimal)
    // followed by a [ character.
    // J command is Erase in Display
    // For other commands see https://vt100.net/docs/vt100-ug/chapter3.html#ED
    // 2 is the argument for J: erase eveything on screen
    // 4 is the number of bytes to be written
    // abAppend(&ab, "\x1b[2J", 4);

    // Reposition the cursor to the row and col
    // H command is for the cursor
    // \x1b[15;45H is center on a 30x90 terminal
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buf[25];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));
    
    // Show cursor
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*----- input -----*/
void editorMoveCursor(int key){
    switch (key)
    {
        case ARROW_LEFT:
            if(E.cx != 0)
                E.cx--;
            break;
        case ARROW_RIGHT:
            if(E.cx != E.screencols - 1)
                E.cx++;
            break;
        case ARROW_UP:
            if(E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy != E.screenrows - 1)
                E.cy++;
            break;
    }
}

void editorProcessKeyPress(){
    int c = editorReadKey();
    switch (c){
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.screencols - 1;
            break;
        
        case PAGE_DOWN:
        case PAGE_UP:
            {
                int times = E.screenrows; // variable declaration isn't allowed in 
                                          // switch case unless inside a block
                while(times--)
                    editorMoveCursor(c == PAGE_DOWN ? ARROW_DOWN : ARROW_UP);
            }
            break;
        case ARROW_UP:
        case ARROW_LEFT:
        case ARROW_DOWN:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

/*----- init -----*/
void initEditor(){
    E.cx = 0;
    E.cy = 0;
    if(getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
}

int main(){
    enableRawMode();
    initEditor();
    
    while(1){
        editorRefreshScreen();
        editorProcessKeyPress();
    }
    return 0;
}