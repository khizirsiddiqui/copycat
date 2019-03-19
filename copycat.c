/*----- includes -----*/
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/*----- data -----*/
struct termios orig_termios;

/*----- defines -----*/
// CTRL key assigns 5 and 6 bit to 0 and send it
#define CTRL_KEY(k) ((k) & 0x1f)

/*----- terminal -----*/
void die(const char* s){
    perror(s);
    exit(1);
}

void disableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1){
        die("tcsetattr");
    }
}

void enableRawMode(){
    // read the current attributes
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");

    // automatically disable raw mode on exit
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    
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

/*----- init -----*/

int main(){
    enableRawMode();
    printf("copycat text editor\r\bPress Ctrl+Q to exit\r\nAuthor:github@khizirsiddiqui\r\n\r\n");
    while(1){
        char c = '\0';
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");
        if(iscntrl(c))
            // If the input char is a control character (non-printable)
            printf("%d\r\n", c);
        else
            printf("%d ('%c')\r\n", c, c);
        if(c == CTRL_KEY('q'))
            // Ctrl+Q: Quit
            break;
    }
    return 0;
}