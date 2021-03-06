/*!
 * \file support.c
 * 
 * \brief This file implements support routines declared in support.h.
 * 
 */

#include "support.h"

/*!
 */
void *signal_listener(void *d)
{
    game_data *data = (game_data*) d;
    struct signalfd_siginfo signal_info;

    /* create poll to wait on signal file descriptor */
    struct pollfd pfd[1];
    pfd[0].fd = data->signal_fd;
    pfd[0].events = POLLERR | POLLHUP | POLLIN;

    while (1)
    {
        /* wait for event on signal fd and then read signal from pipe */
        poll(pfd, 1, 1);
        read(data->signal_fd, &signal_info, sizeof (struct signalfd_siginfo));

        /* manage signal */
        switch (signal_info.ssi_signo)
        {
            case SIGKILL:
            case SIGTERM:
            case SIGINT:
                /* quit game safely */
                termination_handler();
                break;

            case SIGWINCH:
                /* resize field (critical section) */
                pthread_mutex_lock(&data->mut);
                resize_handler(data);
                pthread_mutex_unlock(&data->mut);
                break;

            default:
                break;
        }
        
    }
}

/*!
 * This procedure is an handler to manage window resize.
 */
void resize_handler(game_data *data)
{
    struct winsize ws;

    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws); /* get terminal size */
    wresize(stdscr, ws.ws_row, ws.ws_col); /* resize ncurses window */

    endwin();

    /* update field size */
    data->bottom_row = getmaxy(stdscr) - 1;
    data->paddle_col = getmaxx(stdscr) - 1;

    /* ensure objects are inside the new field */
    if (data->paddle_pos > data->bottom_row - PADDLE_WIDTH / 2)
        data->paddle_pos = MAX(
                data->bottom_row - PADDLE_WIDTH / 2,
                PADDLE_WIDTH / 2); /* avoid the paddle to go above top row */
    if (data->ai_paddle_pos > data->bottom_row - PADDLE_WIDTH / 2)
        data->ai_paddle_pos = MAX(
                data->bottom_row - PADDLE_WIDTH / 2,
                PADDLE_WIDTH / 2);
    if (data->ball_y > data->bottom_row)
        data->ball_y = data->bottom_row;
    if (data->ball_x > getmaxx(stdscr))
        data->ball_y = getmaxx(stdscr) / 2;

    /* update screen content */
    clear();
    draw_paddle(data, AI_TAG);
    draw_paddle(data, KBD_TAG);
    draw_ball(data);
    refresh();
}

/*!
 * This procedure is a listener for keyboard input during the game.
 * When a player press a key, the input triggers the related action and a 
 * message to the game main thread is sent throug the pipe.
 */
void *keyboard_handler(void *d)
{
    game_data *data = (game_data*) d;

    // Don't mask any mouse events
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);

    printf("\033[?1003h\n"); // Makes the terminal report mouse movement events

    while (!data->termination_flag)
    {
        int ch;

        /* get user input (critical section) */
        pthread_mutex_lock(&data->mut);
        ch = getch(); 
        pthread_mutex_unlock(&data->mut);
	
	while(data->haltFlag != 0);
        
	switch (ch)
        {
            case KEY_UP:
                /* move pad up when possible */
                data->paddle_pos_old = data->paddle_pos;
                if (data->paddle_pos > PADDLE_WIDTH / 2)
                    data->paddle_pos--;
                write(data->pipedes[1], KBD_TAG, TAG_SIZE);
                break;

            case KEY_DOWN:
                /* move pad down when possible */
                data->paddle_pos_old = data->paddle_pos;
                if (data->paddle_pos < data->bottom_row - PADDLE_WIDTH / 2)
                    data->paddle_pos++;
                write(data->pipedes[1], KBD_TAG, TAG_SIZE);
                break;

            case PLAY_KEY:
                /* set flag to play a new game */
                data->play_flag = 1;
                break;

            case QUIT_KEY:
                /* set flag asking for game termination */
                data->exit_flag = 1;
                /* dummy write to unlock the controller thread, waiting
                 * at the other pipe end */
                write(data->pipedes[1], QUIT_TAG, TAG_SIZE);
                break;

            default:
		{
			MEVENT event;
			
			if (getmouse(&event) == OK) 
			{
				/* move pad up when possible */  	
				data->paddle_pos_old = data->paddle_pos;
		                data->paddle_pos = event.y;
		                write(data->pipedes[1],  KBD_TAG, TAG_SIZE);
	
			}
		
		} 
                break;
        }
    }
    printf("\033[?1003l\n"); // Disable mouse movement events, as l = low
    
    return 0;
}

/*!
 * This procedure is responsible for ball movement. The ball position is 
 * updated every TIME_GAP_BALL microseconds, and then a message to the 
 * game main thread is sent throug the pipe.
 */
void *ball_handler(void *d)
{
    game_data *data = (game_data*) d;
    
    /* initialize */
    data->gameLevel = 0;
    data->hitCnt = 0;
	
    while (1)
    {
	while(data->haltFlag != 0);

        /* update ball coordinates */
        data->ball_y_old = data->ball_y;
        data->ball_x_old = data->ball_x;
        data->ball_y += data->ball_diry;
        data->ball_x += data->ball_dirx;

        /* reflect ball on field top and bottom */
        if (data->ball_y < FIELD_TOP || data->ball_y > data->bottom_row) 
        {
            data->ball_diry *= -1;
            data->ball_y += 2 * data->ball_diry;
        }

        /* reflect ball on player pad */
        if (data->ball_x == data->paddle_col)
        {
            if (abs(data->paddle_pos - data->ball_y - -data->ball_diry) 
                    <= PADDLE_WIDTH / 2)
            {
                /* ball is above the pad; consider one extra on length
                 * because the ball is moving diagonally */
                data->ball_dirx *= -1;
                data->ball_x += 2 * data->ball_dirx;

		if(data->hitCnt >= MAX_HITCNT)
		{
			data->gameLevel++;

			data->hitCnt = 0;
		        
            		print_level(stdscr, data->gameLevel);
			char c;
	
		        do { 
			    data->haltFlag =1;
		            c = getch();
		            if (c == QUIT_KEY)
		                /* safe because threads have not been created yet */
			    	termination_handler(); 
        		} while (c != ' ');
			clear();
			data->haltFlag=0;
            	}
		else
			data->hitCnt++;
			
		if(data->gameLevel > MAX_LEVEL)
		{
			/* ball is out */
	                data->play_flag = 0;
	
	                /* ai loses, player wins */
	                data->winner = 0;
	
	                /* dummy write to unlock the controller waiting
	                 * at the other pipe end */
	
	                write(data->pipedes[1], QUIT_TAG, TAG_SIZE);
	
	                /* thread termination */
               		return 0;
		}

            } else {
                /* ball is out */
                data->play_flag = 0;

                /* player loses, ai wins */
                data->winner = 1;

                /* dummy write to unlock the controller waiting
                 * at the other pipe end */
                write(data->pipedes[1], QUIT_TAG, TAG_SIZE);

                /* thread termination */
                return 0;
            }
        }

        /* reflect ball on AI pad */
        if (data->ball_x == data->ai_paddle_col)
        {
            if (abs(data->ai_paddle_pos - data->ball_y - -data->ball_diry) 
                    <= PADDLE_WIDTH / 2)
            {
                /* ball is above the pad; consider one extra on length
                 * because the ball is moving diagonally */
                data->ball_dirx *= -1;
                data->ball_x += 2 * data->ball_dirx;
            } else {
                /* ball is out */
                data->play_flag = 0;

                /* ai loses, player wins */
                data->winner = 0;

                /* dummy write to unlock the controller waiting
                 * at the other pipe end */

                write(data->pipedes[1], QUIT_TAG, TAG_SIZE);

                /* thread termination */
                return 0;
            }
        }

        write(data->pipedes[1], BALL_TAG, TAG_SIZE);

        usleep(TIME_GAP_BALL*(MAX_LEVEL-1 - data->gameLevel));
    }
}

/*!
 * This procedure controls the ai pad. Movements are generated every 
 * TIME_GAP_AI microseconds, and then a message is sent to the game main
 * thread throug the pipe.
 */
void *ai_handler(void *d)
{
    game_data *data = (game_data*) d;
    
    while (!data->termination_flag)
    {	
	while(data->haltFlag != 0);
        
        int diff = data->ball_y - data->ai_paddle_pos;
        int new = data->ai_paddle_pos + diff / (diff == 0 ? 1 : abs(diff));

        data->ai_paddle_pos_old = data->ai_paddle_pos;

        if (new >= PADDLE_WIDTH / 2 
                && new <= data->bottom_row - PADDLE_WIDTH / 2)
            data->ai_paddle_pos = new;

        write(data->pipedes[1], AI_TAG, TAG_SIZE);
        
        usleep(TIME_GAP_AI);
    }
    
    return 0;
}

/*!
 * This procedure cancels the pad from the previous position according
 * to the shared game_data structure. The second parameter permits to 
 * choose which pad (ai or player) will be deleted.
 */
void delete_paddle(game_data *data, char *tag)
{
    int i;
    int type = !strcmp(tag, KBD_TAG); /* 1 for player, 0 for ai */
    int row = (type ? data->paddle_pos_old : data->ai_paddle_pos_old)
        - PADDLE_WIDTH / 2; /* base row */

    /* delete all points from base row for all the paddle length */
    for (i = 0; i < PADDLE_WIDTH ; ++i)
    {
        mvaddch(
               row + i,
               type ? data->paddle_col : data->ai_paddle_col,
               ' ');
	 mvaddch(
               row + i,
               type ? data->paddle_col-1 : data->ai_paddle_col+1,
               ' ');
     }
}

/*!
 * This procedure draws the paddle in the current position provided by 
 * the shared game_data structure. The second parameter determines which pad 
 * will be drawn.
 */
void draw_paddle(game_data *data, char *tag)
{
    int i;
    int type = !strcmp(tag, KBD_TAG); /* 1 for player, 0 for ai */
    int row = (type ? data->paddle_pos : data->ai_paddle_pos) 
        - PADDLE_WIDTH / 2; /* base row */

    /* delete all points from base row for all the paddle length */
    for (i = 0; i < PADDLE_WIDTH ; ++i)
    {
        attron(COLOR_PAIR(type ? PADDLE_COLOR : AI_COLOR));
        mvaddch(
                row + i,
                type ? data->paddle_col : data->ai_paddle_col,
	                ' ');
	mvaddch(
                row + i,
                type ? data->paddle_col-1 : data->ai_paddle_col+1,
	                ' ');
        attroff(COLOR_PAIR(type ? PADDLE_COLOR : AI_COLOR));
    }
}

void delete_ball(game_data *data)
{
    mvaddch(data->ball_y_old, data->ball_x_old, ' '); 
}

void draw_ball(game_data *data)
{
    attron(COLOR_PAIR(BALL_COLOR));
    mvaddch(data->ball_y, data->ball_x, 'o');
    attroff(COLOR_PAIR(BALL_COLOR));
}

/*!
 * This procedure restores the xorg typematic settings as they were 
 * before the game start.
 */
void restore_key_rate()
{
    char command[25]; /* string to hold system commands */
    sprintf(command, "xset r rate %s %s", del, rate);
    system(command);
}

/*!
 * This procedure permits to handle signals for program kill or termination,
 * ensuring the keyboard system settings are restored and the ncurses
 * window is terminated before the program exit.
 */
void termination_handler()
{
    restore_key_rate();
    endwin();
    exit(1);
}

void print_intro_menu(WINDOW *win)
{
    /* print in the center of the window */
    int y = getmaxy(win) / 2;
    int x = getmaxx(win) / 2;
    const char *msg = "PONG";
    const char *msg2 = "use up and down arrow keys to control the pad";
    const char *msg3 = "press space to start, q to quit";

    attron(COLOR_PAIR(TITLE_COLOR));
    mvwaddstr(
            win,
            y,
            x - strlen(msg) / 2,
            msg);
    y++; /* newline */
    mvwaddstr(
            win,
            y,
            x - strlen(msg2) / 2,
            msg2);
    y++; /* newline */
    mvwaddstr(
            win,
            y,
            x - strlen(msg3) / 2,
            msg3);
    attroff(COLOR_PAIR(TITLE_COLOR));

    refresh();
}

void print_intra_menu(WINDOW *win, const char *msg)
{
    /* print in the center of the window */
    int x = getmaxx(win) / 2;
    int y = getmaxy(win) / 2;
    const char *msg2 = "press space to restart, q to quit";
    attron(COLOR_PAIR(TITLE_COLOR));
    mvwaddstr(
            win,
            y,
            x - strlen(msg) / 2,
            msg);
    y++; /* newline */
    mvwaddstr(
            win,
            y,
            x - strlen(msg2) / 2,
            msg2);
    attroff(COLOR_PAIR(TITLE_COLOR));
}

void print_level(WINDOW *win, int level)
{
    /* print in the center of the window */
    int x = 60;
    int y = 0;
    const char *msg2 = "press space to restart, q to quit";
    
    char buffer[40];
    size_t max_size = sizeof(buffer);
    snprintf(buffer, max_size, "Congratulation you have cleared level %d ", level);
    
    attron(COLOR_PAIR(TITLE_COLOR));
    mvwaddstr(
            win,
            y,
            x - strlen(buffer) / 2,
            buffer);
  	    y++; /* newline */
    mvwaddstr(
            win,
            y,
            x - strlen(msg2) / 2,
            msg2);
    attroff(COLOR_PAIR(TITLE_COLOR));
}
