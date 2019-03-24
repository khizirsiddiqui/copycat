/*----- includes -----*/

// Feature test Macros:
// https://www.gnu.org/software/libc/manual/html_node/Feature-Test-Macros.html
// To allow compiler compatibilities

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE 

#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/ioctl.h> // For window size
#include <sys/types.h> // ssize_t

/*----- data -----*/

// To store row data
typedef struct erow {
    int size;
    char *chars;

    int rsize;
    char *renders;
} erow;

struct editorConfig{
    // Cursor position
    int cx;
    int cy;

    // Screen Dimensions
    int screenrows;
    int screencols;

    // Data
    int numrows;
    erow *row;
    int rowoff;
    int coloff;

    // Terminal Identity
    struct termios orig_termios;
};

struct editorConfig E;

/*----- defines -----*/
#define COPYCAT_VERSION "0.0.1"
// CTRL key assigns 5 and 6 bit to 0 and then send it
#define CTRL_KEY(k) ((k) & 0x1f)

#define COPYCAT_TAB_STOP 4

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
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*----- row operations -------*/
void editorUpdateRow(erow *row) {
    int tabs=0;
    int j;

    for ( j = 0; j<row->size; j++)
        if (row->chars[j] == '\t') tabs++;
    
    free(row->renders);
    row->renders = malloc(row->size + tabs*(COPYCAT_TAB_STOP - 1) + 1);
    
    int idx=0;
    for ( j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->renders[idx++] = ' ';
            while (idx % COPYCAT_TAB_STOP != 0)
                row->renders[idx++] = ' ';
        } else 
            row->renders[idx++] = row->chars[j];
    }
    row->renders[idx] = '\0';
    row->rsize = idx;
}


void editorAppendRow(char *s, size_t len){
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].renders = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
}


/*----- file i/o -----*/
void editorOpen(char *filename){
    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = NULL;

    ssize_t linelen;
    size_t linecap = 0; // stores how much memory was allocated

    // getline usefull for assignments when the instream memory managament can not
    // be predicted 
    while ((linelen = getline(&line, &linecap, fp)) != -1){
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
                linelen--;
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
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
void editorScroll(){
    if (E.cy < E.rowoff) {
        // If the cursor is above visible window
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows)
        // If the cursor is below the bottom of visible window
        E.rowoff = E.cy - E.screenrows + 1;
    if (E.cx < E.coloff)
        E.coloff = E.cx;
    if (E.cx >= E.coloff + E.screenrows)
        E.coloff = E.cx - E.screenrows + 1;
}

void editorDrawRows(struct abuf *ab){
    int y=0;
    for(; y < E.screenrows; y++){
        int filerows = y + E.rowoff;
        if (filerows >= E.numrows){
            if (E.numrows == 0 && y == E.screenrows / 4){
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
        } else {
            // Print the content of data row-wise
            int len = E.row[filerows].rsize - E.coloff;
            if (len < 0) len = 0; // User scrolling past the end of line
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerows].renders[E.coloff], len);
        }
        // Erase the part of line to the right of curson
        abAppend(ab, "\x1b[K", 3);
        if ( y < E.screenrows - 1)
            abAppend(ab, "\r\n", 2);
    }    
}

void editorRefreshScreen(){
    editorScroll();

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
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.cx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));
    
    // Show cursor
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*----- input -----*/
void editorMoveCursor(int key){
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    switch (key)
    {
        case ARROW_LEFT:
            if(E.cx != 0)
                E.cx--;
            else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size)
                // Move only when to the left of end of line
                E.cx++;
            else if (row && E.cx == row->size) {
                // When the cursor reaches end of line
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows)
                E.cy++;
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen)
        E.cx = rowlen;
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
    E.numrows = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.row = NULL;
    if(getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
}

int main(int argc, char *argv[]){
    enableRawMode();
    initEditor();
    if (argc >= 2)
        editorOpen(argv[1]);
    
    while(1){
        editorRefreshScreen();
        editorProcessKeyPress();
    }
    return 0;
}