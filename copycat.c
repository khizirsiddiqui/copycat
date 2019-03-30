/*
    COPYCAT ver 0.0.1
    A vim-like editor written in C without using
    any external libraries
    Auhor: Mohd Khizir Siddiqui git@khizirsiddiqui
    License: MIT
    Follows from tutorial: https://viewsourcecode.org/snaptoken/kilo/
*/

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
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h> // For window size
#include <sys/types.h> // ssize_t

/*----- prototypes -----*/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char*, int));

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
    int rx; // Keep track of tabs along with cx

    // Screen Dimensions
    int screenrows;
    int screencols;

    // Data
    int numrows;
    erow *row;
    int rowoff;
    int coloff;
    int dirty;  // Tracks whether data has been changed

    // File Data
    char *filename;

    // StatusBar msg
    char statusmsg[80];
    time_t statusmsg_time;

    // Terminal Identity
    struct termios orig_termios;
};

struct editorConfig E;

/*----- defines -----*/
#define COPYCAT_VERSION "0.0.1"
// CTRL key assigns 5 and 6 bit to 0 and then send it
#define CTRL_KEY(k) ((k) & 0x1f)

// Length of a tab space
#define COPYCAT_TAB_STOP 4
// Ctrl+Q quit after n times
#define COPYCAT_QUIT_TIMES 3

enum editorKey {
    BACKSPACE = 127,

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

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;
    for ( j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (COPYCAT_TAB_STOP - 1) - ( rx % COPYCAT_TAB_STOP);
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < E.row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (COPYCAT_TAB_STOP - 1) - (cur_rx % COPYCAT_TAB_STOP);
        cur_rx++;

        if (cur_rx > rx) return cx;
    }
    return cx;

}

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

void editorFreeRow(erow *row) {
    free(row->renders);
    free(row->chars);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows -at - 1));
    E.numrows--;
    E.dirty++;
}

void editorInsertRow(int at, char *s, size_t len){
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].renders = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);

    // Like memcopy but safer when the source and dest arrays overlap
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);

    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);

    E.dirty++;
}

/*----- editor Operations ----*/

void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        // Cursor is at a tilde line
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewLine() {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 1);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &E.row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx=0;
}

void editorDelChar() {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy -  1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*----- file i/o -----*/

char *editorRowToString(int *buflen) {
    // Warning: free() the returned array after use

    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);     // Stores the row text
    char *p = buf;
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename){
    free(E.filename);
    E.filename = strdup(filename); // Duplicate the mallocated string
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
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);

    E.dirty = 0;
}

void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowToString(&len);

    // O_RDWR: Read and write
    // O_CREAT: Create if doesn't exist
    // 0644: Std permission given to file
    //          User can edit and read
    //          Other can read only
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);

    if (fd != -1) {
        // Sets the file size to specific length
        if(ftruncate(fd, len) != -1) {
            if(write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%dKB written to disk", len/1024);
                return;
            }

        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't Save! I/O Error:%s", strerror(errno));
}

/*----- Find --------------*/

void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = -1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }


    if (last_match == -1) direction = 1;
    int current = last_match;
    int i;
    for (i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;

        erow *row = &E.row[current];
        // strstr : Check for a substring
        char *match = strstr(row->renders, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->renders);
            E.rowoff = E.numrows;
            break;
        }
    }
}

void editorFind() {
    int saved_cx = E.cx, saved_cy = E.cy;
    int saved_colloff = E.coloff, saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/ARROW/ENTER", editorFindCallback);
    if (query == NULL)
        free(query);
    else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_colloff;
        E.rowoff = saved_rowoff;
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
void editorScroll(){
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }
    if (E.cy < E.rowoff) {
        // If the cursor is above visible window
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows)
        // If the cursor is below the bottom of visible window
        E.rowoff = E.cy - E.screenrows + 1;
    if (E.rx < E.coloff)
        E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screencols)
        E.coloff = E.rx - E.screencols + 1;
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
        abAppend(ab, "\r\n", 2);
    }    
}

void editorDrawStatusBar(struct abuf *ab) {
    // Graphic Rendition
    // https://vt100.net/docs/vt100-ug/chapter3.html#SGR
    // 0: Attributes Off (Default)
    // 1: Bold
    // 4: Underscore
    // 5: Blink
    // 7: Negative image
    abAppend(ab, "\x1b[7m", 4);      // Invert color of output
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s %s- %d lines",
        E.filename ? E.filename : "[No Name]", 
        E.dirty ? "(modified) " : "", 
        E.numrows);
    int rlen = snprintf(rstatus, sizeof(status), "%d/%d",
        E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);      // Reset the color in output
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);      // Clear the msgBar text
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        // Display only if not older than 5 seconds
        abAppend(ab, E.statusmsg, msglen);
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
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[25];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", 
                            (E.cy - E.rowoff) + 1, 
                            (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));
    
    // Show cursor
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    // printf like function to set status bar message
    va_list ap;         // keeps track of the argumments passed
    va_start(ap, fmt);  // records the addresses of starting from fmt 

    // to help make our own printf function
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*----- input -----*/

char *editorPrompt(char *prompt, void(*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c=='\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}

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
    static int quit_times = COPYCAT_QUIT_TIMES;

    int c = editorReadKey();
    switch (c){
        case '\r':
            editorInsertNewLine();
            break;

        case CTRL_KEY('q'):
            // Exit on CTRL+Q
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING! File was modified"
                    "Press Ctrl+Q %d times more to quit without saving", quit_times);
                    quit_times--;
                    return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        
        case HOME_KEY:
            E.cx = 0;
            break;
        
        case END_KEY:
            if (E.cy < E.numrows)
                // Bring cursor to the end of line
                E.cx = E.row[E.cy].size;
            break;
        
        case PAGE_DOWN:
        case PAGE_UP:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

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

        case CTRL_KEY('s'):
            editorSave();
            break;
        
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            // Screen Refresh
            break;

        default:
            editorInsertChar(c);
            break;
    }
    quit_times = COPYCAT_QUIT_TIMES;
}

/*----- init -----*/
void initEditor(){
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.numrows = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0]='\0';
    E.statusmsg_time = 0;
    if(getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[]){
    enableRawMode();
    initEditor();
    if (argc >= 2)
        editorOpen(argv[1]);
    
    editorSetStatusMessage("Copycat Text Editor : CTRL+S: Save | Ctrl+Q: Quit | CTRL+F: Find");

    while(1){
        editorRefreshScreen();
        editorProcessKeyPress();
    }
    return 0;
}
