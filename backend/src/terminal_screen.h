/* Alternate-screen layout. Task logs scroll inside the body; header, context
 * and input remain outside the scroll region. Never emit ANSI into a pipe. */
#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif
static int screen_active,screen_rows=24;
#ifdef _WIN32
static DWORD screen_mode;
#endif
static int screen_available(void){
    const char *term=getenv("TERM");if(term&&!strcmp(term,"dumb"))return 0;
#ifdef _WIN32
    DWORD mode;return _isatty(0)&&_isatty(1)&&_isatty(2)&&GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),&mode);
#else
    return isatty(0)&&isatty(1)&&isatty(2);
#endif
}
static void screen_size(void){
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;if(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE),&info))screen_rows=info.srWindow.Bottom-info.srWindow.Top+1;
#else
    struct winsize size;if(!ioctl(1,TIOCGWINSZ,&size)&&size.ws_row)screen_rows=size.ws_row;
#endif
    if(screen_rows<8)screen_rows=8;
}
static void screen_end(void){
    if(!screen_active)return;
    fputs("\033[r\033[?25h\033[?1049l",stdout);fflush(stdout);screen_active=0;
#ifdef _WIN32
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),screen_mode);
#endif
}
static int screen_start(void){
    if(!screen_available())return 0;
#ifdef _WIN32
    HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);GetConsoleMode(output,&screen_mode);
    if(!SetConsoleMode(output,screen_mode|ENABLE_VIRTUAL_TERMINAL_PROCESSING))return 0;
#endif
    screen_size();screen_active=1;atexit(screen_end);
    printf("\033[?1049h\033[2J\033[H\033[1;36m Cognitive OS  |  Terminal workspace\033[0m\033[2;%dr\033[2;1H",screen_rows-3);
    fflush(stdout);return 1;
}
static void screen_context(const char *session,const char *workspace){
    if(!screen_active)return;
    fputs("\0337",stdout);screen_size();printf("\033[2;%dr\033[%d;1H\033[2K\033[2m /new  /sessions  /workspace PATH  |  Ctrl+C cancel\033[0m\033[%d;1H\033[2K Session: ",screen_rows-3,screen_rows-2,screen_rows-1);
    text_out(stdout,session);fputs("  |  ",stdout);text_out(stdout,workspace?workspace:"server workspace");fputs("\0338",stdout);fflush(stdout);
}
static void screen_prompt(void){
    if(screen_active)printf("\0337\033[%d;1H\033[2K\033[1;35m❯\033[0m ",screen_rows);
    else fputs("❯ ",stdout);
    fflush(stdout);
}
static void screen_submit(const char *line){
    if(!screen_active)return;
    printf("\033[%d;1H\033[2K\0338\033[1;35m❯ ",screen_rows);text_out(stdout,line);fputs("\033[0m\n",stdout);fflush(stdout);
}
