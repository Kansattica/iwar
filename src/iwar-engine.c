/*
** Copyright (C) 2005,2006 Softwink,  Inc. <beave@softwink.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"		/* From autoconf */
#endif

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <curses.h>
#include <sys/types.h>
#include <limits.h>

#include "iserial.h"
#include "version.h"
#include "iwar-defs.h"
#include "iwar.h"

int  portfd;
FILE *outfd;		/* logfile/output file */
FILE *banner;           /* banners file (system identification) */
FILE *blacklst;         /* blacklisted numbers  file */
FILE *iwarcfg;          /* iwar configuration file */
FILE *saveloadstate;    /* load/save state file */
FILE *userloadfile;

char sendstring[128] = { 0 };
char modemqueue[128] = { 0 };
char fileout[MAXPATH]="iwar.log";

char tmp[128] = { 0 };
char tmp2[128] = { 0 };
char *tmp3 = NULL;
char *tmp4 = NULL;

long long dialnum;
long long ss;
long long es;
long long num[9999];

int savestate=0;
int savestatecount=0;
int i;
int b;
int j=0;
int row=10;
int col=2;

/* Blacklist */

int blacklistcount;
long long blacklistnum[1000];

/* Pre-generated user list */

int userlistcount;
long long userlistnum[9999];

/***************************************************************************/
/*                         General Help/Usage                              */
/***************************************************************************/

void usage(void)
{
    printf("\niWar [Intelligent Wardialer] Version %s - By Da Beave (beave@softwink.com)\n\n",VERSION);
    printf("usage: iwar [parameters] -r [dial range]\n\n");
    printf("\t -h  :  Prints this screen\n");
    printf("\t -s  :  Speed/Baud rate [Serial default: 1200]\n");
    printf("\t -p  :  Parity (None/Even/Odd) \n\t\t[Serial default 'N'one]\n");
    printf("\t -d  :  Data bits [Serial default: 8]\n");
    printf("\t -t  :  TTY to use (modem) \n\t\t[Serial default /dev/ttyS0]\n");
    printf("\t -c  :  Use software handshaking (XON/XOFF)\n\t\t[Serial default is hardware flow control]\n");
    printf("\t -f  :  Output log file [Default: iwar.log]\n");
    printf("\t -e  :  Pre-dial string/NPA to scan [Optional]\n");
    printf("\t -g  :  Post-dial string [Optional]\n");
    printf("\t -a  :  Tone Location (Toneloc W; method) \n\t\t[Serial default: disabled]\n");
    printf("\t -r  :  Range to scan (ie - 5551212-5551313)\n");
    printf("\t -x  :  Sequential dialing [Default: Random]\n");
    printf("\t -F  :  Full logging (BUSY, NO CARRIER, Timeouts, Skipped, etc)\n");
    printf("\t -b  :  Disable banners check \n\t\t[Serial Default: enabled]\n");
    printf("\t -o  :  Disable recording banner data \n\t\t[Serial default: enabled].\n");
    printf("\t -L  :  Load numbers to dial from file.\n");
    printf("\t -l  :  Load 'saved state' file (previously dialed numbers)\n");

    printf("\n");

};

/***********************************************************************/
/* closetty() - This is for trapping signals and/or errors.            */
/***********************************************************************/

void closetty(int sig )
{

    switch(sig)
        {
        case 1:
            ninfo("Error reading TTY.  Aborting!", 1);
            endwin();
            exit(1);
            break;

        case -2:
            ninfo("Write error! Closing serial and aborting!", 1);
            endwin();
            exit(1);
            break;

        /* Hrmph.   This isn't really used - We just keep trying over and over */

        case -3:
            fprintf(stderr, "[No Dialtone! Closing serial/output!]\n");
            ninfo("No dial tone! Aborting.....", 1);
            exit(1);
            break;

        case -4:

            break;

        default:

            m_dtrtoggle(portfd,2);   /* DTR Hangup */
            sendmodem("\r");
            plushangup(4);

            snprintf(tmp, sizeof(tmp), "Signal %d received! Aborting.....", sig);
            dialnum=0;
            loginfo(0, tmp, "", "");
            ninfo(tmp, 1);
            endwin();
            exit(1);
            break;

            m_restorestate(portfd);        /* Restore state from "savestate" */
            close(portfd);  	       /* Close serial/output file */
            exit(0);
        }
}

/***********************************************************************/
/* sendmodem() -  How to send data to the modem                        */
/***********************************************************************/

void sendmodem(const char *sendstring)
{

    int len;

    /* modemqueue is used for things like volume changes,  speaker off, etc */

    /* No commands should start with 13 (return). If the sendstring is
       only 13,  then we are probably hanging up the line.  As in,
       you've sent ATDT5551212,  waited and nothing happened.  So,  to
       hang up the line you send 13.   If thats the case,  we send the
       13 _first_ then send whatever is in the modemqueue.  */

    len=sendstring[0];

    if (len == 13 )
        {
            write(portfd, "\r", 1);
            sleep(1);                  /* Let the modem return NO CARRIER */
        }

    if ( modemqueue[0] != '\0' )
        {
            NCURSES_Status("Sending Queued Commands...");
            write(portfd, modemqueue, strlen(modemqueue));
            modemqueue[0] = '\0';			 /* clear queue */
            sleep(1);
        }

    /* If it's not just a return,  do whatever you where originally
       instructed to do .... */

    if (len != 13)
        {
            write(portfd, sendstring, strlen(sendstring));
        }

}

/**********************************************************************/
/* getnum() - Gets a sequential/random number for dialing.  Also      */
/* check against blacklist numbers and pre-dialed numbers (loaded     */
/* from a file                                                        */
/**********************************************************************/

int getnum(int dialtype, int mysqllog, int tonedetect, const char *predial, const char *postdial)
{

    /*********************************************************/
    /* We've loaded the numbers for a user generated file... */
    /*********************************************************/

    int flag=1, k;

    if (userlistcount > 0)
        {
            if (j == userlistcount)
                {
                    dialnum=0;
                    loginfo(0, "User Generated Scan Completed!", "", "");
                    ninfo("User Generated Scan Completed!", 1);
                    endwin();
                    printf("User Generated Scan Completed!\n");
                    exit(0);
                }

            dialnum=userlistnum[j];

            /* Blacklist check */

            while (flag != 0)
                {
                    for (k=0; k<blacklistcount; k++)
                        {
                            if (blacklistnum[k] == dialnum)
                                {
                                    NCURSES_Status("Blacklisted Number - Skipping...");
                                    loginfo(mysqllog, "BLACKLISTED", "", "");
                                    attron( COLOR_PAIR(15) | A_REVERSE);
                                    NCURSES_Plot(dialnum, row, col);
                                    attroff(COLOR_PAIR(15) | A_REVERSE);
                                    rowcolcheck();
                                    sleep(1);
                                    j++;
                                    dialnum=userlistnum[j];
                                    flag=1;
                                    break;
                                }
                            else
                                {
                                    flag=0;
                                }
                        }
                }

            ncount(userlistcount-j);
            if ( tonedetect==1)
                {
                    snprintf(sendstring, sizeof(sendstring), "ATDT%s%lld%sW;\r", predial, dialnum, postdial);
                }
            else
                {
                    snprintf(sendstring, sizeof(sendstring), "ATDT%s%lld%s\r", predial, dialnum, postdial);
                }


            j++;
            return 0;
        }


    /***********************/
    /* Sequential dialing  */
    /***********************/


    if (dialtype == 1)
        {

            if (ss >= es+1)
                {
                    dialnum=0;
                    loginfo(0, "Sequential Scan Completed!", "", "");
                    ninfo("Sequential Scan Completed!",1);
                    endwin();
                    printf("Sequential Scan Completed!\n");
                    exit(0);
                }

            dialnum=ss;

            /* Blacklist check */

            while (flag != 0)
                {
                    for (k=0; k<blacklistcount; k++)
                        {
                            if (blacklistnum[k] == dialnum)
                                {
                                    NCURSES_Status("Blacklisted Number - Skipping...");
                                    loginfo(mysqllog, "BLACKLISTED", "", "");
                                    attron( COLOR_PAIR(15) | A_REVERSE);
                                    NCURSES_Plot(dialnum, row, col);
                                    attroff(COLOR_PAIR(15) | A_REVERSE);
                                    rowcolcheck();
                                    sleep(1);
                                    ss++;
                                    dialnum=ss;
                                    flag=1;
                                    break;
                                }
                            else
                                {
                                    flag=0;
                                }
                        }
                }

            /* If this is loaded from a previous state file, make sure we haven't
               already dialed the number previously */

            flag=1;

            while (savestate == 1 && flag != 0)
                {
                    /* k=3 because 0-2 are for dialtype, start scan and end scan */

                    for (k=3; k<savestatecount; k++)
                        {
                            if (num[k] == dialnum)
                                {
                                    ss++;
                                    dialnum=ss;
                                    j++;
                                }
                            else
                                {
                                    flag=0;
                                }
                        }
                }

            /* Build our dial string.  Are we doing tone location? */

            if ( tonedetect==1 )
                {
                    /* Can't do old school tone location with a post dial string,
                       so we ignore it.  */

                    snprintf(sendstring, sizeof(sendstring), "ATDT%s%lldW;\r", predial, ss); /* Tone */
                }
            else
                {
                    snprintf(sendstring, sizeof(sendstring), "ATDT%s%lld%s\r", predial, ss, postdial);
                }

            num[j]=ss;
            j++;
            ss++;
            ncount(es-ss+1);
            return 0;
        }

    /***********************************/
    /* Random Dialing                  */
    /***********************************/

    /* If loaded from a saved state, the first 3 values are for dialtype,
       startscan, endscan.  If not from a saved state,  start at the front
       of the array */

    if (savestate == 1)
        {
            k=3;    /* Saved state or new ? */
        }
    else
        {
            k=0;
        }

    while (j < es-ss+k+1 )
        {

            dialnum = (long long)((double)rand() / ((double)RAND_MAX + 1) * es+1);

            if (dialnum >= ss )   /* Usable number? */
                {

                    if (savestate == 0)
                        {

                            for (i=0; i < j; i++) if (num[i] == dialnum) flag=1;
                        }
                    else
                        {
                            for (i=3; i < j; i++) if (num[i] == dialnum) flag=1;
                        }

                    /* Check blacklist for number */

                    for (i=0; i<blacklistcount; i++)
                        {
                            if (blacklistnum[i] == dialnum)
                                {
                                    NCURSES_Status("Blacklisted Number - Skipping...");
                                    attron( COLOR_PAIR(15) | A_REVERSE);
                                    NCURSES_Plot(dialnum, row, col);
                                    attroff(COLOR_PAIR(15) | A_REVERSE);
                                    rowcolcheck();
                                    loginfo(mysqllog, "BLACKLISTED", "", "");
                                    sleep(1);
                                    num[j] = dialnum;
                                    j++;
                                    flag=1;
                                    break;
                                }
                        }


                    if (flag==0)
                        {
                            num[j] = dialnum;
                            j++;
                            flag=0;
                            if (tonedetect == 1 )              /* Tone location */
                                {
                                    snprintf(sendstring, sizeof(sendstring), "ATDT%s%lldW;\r", predial, dialnum);
                                }
                            else
                                {
                                    snprintf(sendstring, sizeof(sendstring), "ATDT%s%lld%s\r", predial, dialnum, postdial);
                                }


                            ncount(es-ss-j+k+1);  /* k is for saved state or not */
                            return 0;
                        }
                    flag=0;
                }
        }   /* End while */

    /* If it's random and there is nothing left to do,  we've completed our
       mission */

    dialnum=0;
    loginfo(0, "Random Scan Completed!", "", "");
    ninfo("Random Scan Completed!",1);
    endwin();
    printf("Random Scan Completed!\n");
    exit(0);
}


/***********************************************************************/
/* rowcolcheck() -  Checks to make make sure we are not trying to      */
/* information outside of the screen                                   */
/***********************************************************************/

void rowcolcheck( void )
{
    int maxrow;
    int maxcol;

    getmaxyx(stdscr,maxrow,maxcol);  /* Current screen attributes */
    row++;

    snprintf(tmp, sizeof(tmp), "%lld", dialnum);

    if ( row > maxrow-10)
        {
            row=10;
            col = col + strlen(tmp) + 2;
        }

    /* If we're outside our range,   wipe the screen.   There's
       probably a better way to do this */

    if (col + strlen(tmp) >= maxcol)
        {
            NCURSES_Status("Screen full: Clearing...");
            col=2;
            attron(COLOR_PAIR(10) | A_NORMAL);
            for (i=10; i < maxrow-9; i++)
                {
                    move(i,0);
                    for (b=0; b < maxcol-1; b++)
                        {
                            addch(' ');		/* wipe it away.... */
                        }
                }
            attroff(COLOR_PAIR(10) | A_NORMAL);
        }


}

/**********************************************************************/
/* loginfo() - Function takes care of logging information to a ASCII  */
/* flat file and a MySQL database.  As it stands,  we always log to   */
/* a ASCII flat file,  MySQL is optional                              */
/**********************************************************************/

void loginfo(int mysqllog, const char *response, const char *ident, const char *recordbuf )
{

    time_t t;
    struct tm *now;

    /* Standard ASCII flat file */

    if ((outfd = fopen(fileout, "a")) == NULL)
        {
            fprintf(stderr, "\nERROR: Can't open %s for output\n\n", fileout);
            usage();
            exit(1);
        }

    t = time(NULL);
    now=localtime(&t);
    strftime(tmp2, sizeof(tmp2), "%H:%M:%S",  now);

    /* Data is formated based on the information we need to log */

    if (dialnum == 0)
        {
            fprintf(outfd, "%s %s\n", tmp2, response);
        }
    else
        {
            if (strlen(ident) == 0)
                {
                    fprintf(outfd, "%s %lld %s %s\n", tmp2, dialnum, response, recordbuf);
                }
            else
                {
                    if (strlen(recordbuf) != 0 )
                        {
                            fprintf(outfd, "%s %lld  %s [ %s ] \n------------------------------------------------------------------------------\n%s\n------------------------------------------------------------------------------\n", tmp2, dialnum, response, ident, recordbuf);
                        }
                    else
                        {
                            fprintf(outfd, "%s %lld  %s [ %s ] \n", tmp2, dialnum, response, ident);
                        }

                }
        }

    fflush(outfd);		/* flush! so if you're tail -f the log file! */
    fclose(outfd);

}

/**********************************************************************/
/* plushangup() - This is incase the modem won't hang up via DTR drop */
/* If you have to use this function,  prehaps you should get a better */
/* modem or fix your crap.  Try and _NOT_ use this.                   */
/**********************************************************************/

void plushangup(int plushangupsleep)
{
    snprintf(sendstring, sizeof(sendstring), "+++");
    NCURSES_Status("+++");
    sendmodem(sendstring);
    sleep(plushangupsleep);
    NCURSES_Status("ATH");
    snprintf(sendstring, sizeof(sendstring), "ATH\r");
    sendmodem(sendstring);
    sleep(plushangupsleep);
}

/*********************************************************************/
/* dtrreinit() - This is for modems that treat DTR drops like        */
/* sending "ATZ" to the modem.  In this case,  the DTR causes the    */
/* modem to "forget" the init string that we used!   This just       */
/* re-informs the modem.   This is useful for USR Couriers           */
/*********************************************************************/

void dtrreinit(const char *modeminit, int volume)
{

    strlcpy(tmp, modeminit, sizeof(tmp));
    sendmodem(tmp);
    sleep(1);
    if (volume == 0) sendmodem("ATM0\r");
    if (volume == 1) sendmodem("ATM1L1\r");
    if (volume == 2) sendmodem("ATM1L2\r");
    if (volume == 3) sendmodem("ATM1L3\r");
    sleep(1);
}

/*********************************************************************/
/* exitscreen() - A bit of information after we destroy all windows  */
/* and we're back at the *nix prompt                                 */
/*********************************************************************/

void exitscreen(int connect, int nocarrier, int voice, int busy, int tonesilence,  int mark, int skip)
{

    printf("================================================================================\n");
    printf("CONNECT: %d | NO CARRIER: %d | VOICE: %d | BUSY: %d | TONE: %d | MARK: %d | SKIP: %d\n", connect, nocarrier, voice, busy, tonesilence, mark, skip);
    printf("================================================================================\n");
    printf("Written Da Beave | beave@softwink.com | Version : %s\n\n", VERSION);
    printf("Exiting iWar ......\n");
    exit(0);

}

/**********************************************************************/
/* drawinfo().  Draws the main screen.   This is called on startup    */
/* and when the user re-sizes the screen                              */
/**********************************************************************/

void drawinfo( const char *baudrate,
               const char *bits,
               const char *parity,
               const char *tty,
               const char *numbersfile,
               const char *predial,
               const char *postdial,
               int logtype,
               int connect,
               int nocarrier,
               int busy,
               int voice,
               int tonesilence,
               int dialtype,
               int timeout )
{

    NCURSES_Mainscreen();

    /* If the dialtype is 0/1 (Ran. / Seq. ) */

    if ( dialtype==0 )
        {
            strlcpy(tmp, "Random", sizeof(tmp));
            j=savestatecount;
        }
    else
        {
            strlcpy(tmp, "Seq.", sizeof(tmp));
        }

    /* Don't care what dialtype is if user generated */

    if ( numbersfile[0] != '\0' )
        {
            strlcpy(tmp, "User Gen.", sizeof(tmp));
        }

    move(1,20);
    attron(COLOR_PAIR(1));
    printw("%s,%s,%s (%s) [%s]",baudrate,bits,parity,tty,tmp);

    if ( numbersfile[0] != '\0' )
        {
            move(2,20);
            printw("%lld - %lld [%d]", userlistnum[0], userlistnum[userlistcount-1], userlistcount);
        }
    else
        {
            move(2,20);
            printw("%lld - %lld [%d]", ss, es, es-ss);
        }

    if ( predial[0] == '\0' )
        {
            strlcpy(tmp, "[None]", sizeof(tmp));
        }
    else
        {
            strlcpy(tmp, predial, sizeof(tmp));
        }

    if ( postdial[0] == '\0' )
        {
            strlcpy(tmp2, "[None]", sizeof(tmp2));
        }
    else
        {
            strlcpy(tmp2, postdial, sizeof(tmp2));
        }

    move(3,20);
    printw("%s / %s ", tmp, tmp2);

    if (logtype==1)
        {
            strlcpy(tmp2, "[F]", sizeof(tmp2));
        }
    else
        {
            strlcpy(tmp2, "[N]", sizeof(tmp2));
        }

    move(4,20);
    printw("%s %s", fileout, tmp2);
    nright(1,connect);
    nright(2,nocarrier);
    nright(3,busy);
    nright(4,voice);
    nright(5,tonesilence);
    nright(6,timeout);

}

/**********************************************************************/
/* Welcome to main().  Where the magic and fun takes place            */
/**********************************************************************/

int main(int argc,  char **argv)
{


    WINDOW *modemstatus;

    struct banner_cfg
    {
        char search_string[40];
        char os_type[128];
    } bannercfg[1000];

    char *iwar_option;
    char *iwar_value;

    int serialtimeout=60;
    int ringtimeout=30;
    int bannertimeout=30;
    int bannermaxcount=2048;
    int bannersendcrtimeout=10;
    int bannersendcr=3;
    int bannercount=0;
    int conredial=5;
    int redial=3;
    int plushang=0;
    int sendcr=0;
    int record=1;		     /* Record remove system banners */
    int dtrsec=0;
    char recordbuf[10240] = { 0 };    /* Record buffer */

    int key;                     /* move tom main */
    int bitcount=0;

    int c;	                     /* c for getopt */
    char ch;

    char numbersfile[MAXPATH] = { 0 };

    int connect=0;
    int nocarrier=0;
    int busy=0;
    int voice=0;
    int tonedetect=0;
    int tonesilence=0;
    int mark=0;
    int skip=0;
    int timeout=0;
    int dtrinit=0;
    int volume=3;
    int ok=0;
    int plushangupsleep=4;

    /* Setup some default port settings */

    char tty[20]		= "/dev/ttyS0";
    char baudrate[7]		= "1200";
    char parity[2]		= "N";
    char bits[2]		= "8";
    char predial[41] = { 0 };
    char postdial[41] = { 0 };

    char modeminit[200] = { 0 };

    int dialtype=0;		/*  1 == sequential */
    int buflen;
    int waitin=0;
    int b;

    int maxrow;
    int maxcol;

    int oldmaxrow;
    int oldmaxcol;

    time_t t;
    struct tm *now;

    char bannerfile[MAXPATH]=BANNER_FILE_PATH;
    char iwarconf[MAXPATH]=CONFIG_FILE_PATH;
    char blacklistfile[MAXPATH]=BLACKLIST_FILE_PATH;
    char statefile[MAXPATH] = { 0 };
    char tmpscanbuf[128] = { 0 };
    char scanbuf[128] = { 0 };

    int logtype=0;
    int ring=0;
    int remotering=0;
    int bannercheck=1;
    int beepflag=0;
    int mysqllog=0;
    char bannerbuf[1024] = { 0 };
    char iwarbuf[1024] = { 0 };
    int connectflag=0;

    int hwhandshake=1;
    int swhandshake=0;

    char tmpscanrange[31] = { 0 };
    char startscan[20] = { 0 };
    char endscan[20] = { 0 };

    char buf[128] = { 0 };
    char buf2[123] = { 0 };

    fd_set fds;
    struct timeval tv;

    /**************************************************************************/
    /* Get options from the iwar configuration file                           */
    /**************************************************************************/

    if ((iwarcfg = fopen(iwarconf, "r")) == NULL)
        {
            fprintf(stderr, "\nERROR: Cannot open configuration file (%s)\n\n",iwarconf);
            usage();
            exit(1);
        }

    while (fgets(iwarbuf, 1024, iwarcfg) != NULL)
        {
            if (iwarbuf[0] == '#') continue;		         /* Comments */
            if (iwarbuf[0] == 10 ) continue;

            iwar_option = strtok(iwarbuf, " ");
            iwar_value  = strtok(NULL, " ");

            if (!strcmp(iwar_option, "port"))
                {
                    strlcpy(tty, iwar_value, strlen(iwar_value));
                }

            if (!strcmp(iwar_option, "speed"))
                {
                    strlcpy(baudrate, iwar_value, strlen(iwar_value));
                }

            if (!strcmp(iwar_option, "parity"))
                {
                    strlcpy(parity, iwar_value, strlen(iwar_value));
                }

            if (!strcmp(iwar_option, "databits"))
                {
                    strlcpy(bits, iwar_value, sizeof(bits));
                }

            if (!strcmp(iwar_option, "init"))
                {
                    snprintf(modeminit, sizeof(modeminit), "%s\r", iwar_value);
                }

            if (!strcmp(iwar_option, "banner_file"))
                {
                    strlcpy(bannerfile, iwar_value, strlen(iwar_value));
                }


            if (!strcmp(iwar_option, "blacklistfile"))
                {
                    strlcpy(blacklistfile, iwar_value, strlen(iwar_value));
                }

            if (!strcmp(iwar_option, "serial_timeout"))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    serialtimeout=atoi(tmp);
                }

            if (!strcmp(iwar_option, "remote_ring"))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    remotering=atoi(tmp);
                }

            if (!strcmp(iwar_option, "ring_timeout"))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    ringtimeout=atoi(tmp);
                }

            if (!strcmp(iwar_option, "tone_detect")) tonedetect=1;

            if (!strcmp(iwar_option, "dtrinit") &&  tonedetect != 1) dtrinit=1;

            if (!strcmp(iwar_option, "banner_timeout"))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    bannertimeout=atoi(tmp);
                }

            if (!strcmp(iwar_option, "dtrsec" ))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    dtrsec=atoi(tmp);
                }

            if (!strcmp(iwar_option, "banner_maxcount"))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    bannermaxcount=atoi(tmp);
                    if (bannermaxcount > 10240)  /* Larger than our record buffer */
                        {
                            printf("ERROR in iwar.conf:  banner_maxcount is larger than 10k!\n\n");
                            exit(1);
                        }
                }

            if (!strcmp(iwar_option, "banner_send_cr"))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    bannersendcrtimeout=atoi(tmp);
                }

            if (!strcmp(iwar_option, "banner_cr"))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    bannersendcr=atoi(tmp);
                }

            if (!strcmp(iwar_option, "redial"))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    redial=atoi(tmp);
                }

            if (!strcmp(iwar_option, "connect_redial"))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    conredial=atoi(tmp);
                }

            if (!strcmp(iwar_option, "beep")) beepflag=1;
            if (!strcmp(iwar_option, "plushangup")) plushang=1;

            if (!strcmp(iwar_option, "plushangupsleep"))
                {
                    strlcpy(tmp, iwar_value, sizeof(tmp));
                    plushangupsleep=atoi(tmp);
                }
        }

    fclose(iwarcfg);

    /**************************************************************************/
    /* Get command line options now                                           */
    /**************************************************************************/

    while ((c = getopt (argc, argv, "r:s:p:l:L:d:t:e:g:f:r:hacxFobm")) != EOF)
        {
            switch(c)
                {
                case 'h':
                    usage();
                    exit(0);
                    break;
                case 's':

                    /*
                    Verify this is a valid rate they are passing to
                    us.
                    */

                    if (!strcmp(optarg, "110") || !strcmp(optarg,"300") ||
                            !strcmp(optarg, "1200") || !strcmp(optarg,"2400")    ||
                            !strcmp(optarg, "4800") || !strcmp(optarg,"9600")    ||
                            !strcmp(optarg, "19200")|| !strcmp(optarg,"38400")   ||
                            !strcmp(optarg, "57600")|| !strcmp(optarg,"115200"))
                        {
                            strlcpy(baudrate, optarg, sizeof(baudrate));
                            break;
                        }
                    fprintf(stderr, "\nERROR: Invalid port speed.\n\n");
                    usage();
                    exit(1);
                    break;
                case 'p':
                    if (!strcmp(optarg, "N") || !strcmp(optarg,"E") ||
                            !strcmp(optarg,"O"))
                        {
                            strlcpy(parity, optarg, 1);
                            parity[sizeof(parity)-1] = '\0';
                            break;
                        }
                    fprintf(stderr, "\nERROR: Invalid Parity.\n\n");
                    usage();
                    exit(1);
                    break;

                /*
                Databits - Valid are 5,6,7,8
                */

                case 'd':
                    if (!strcmp(optarg, "5") || !strcmp(optarg,"6") ||
                            !strcmp(optarg,"7") || !strcmp(optarg,"8"))
                        {
                            strlcpy(bits, optarg, sizeof(bits));
                            break;
                        }

                    fprintf(stderr, "\nERROR: Invalid Databits\n\n");
                    usage();
                    exit(1);
                    break;
                case 't':
                    if (strlen(optarg) > 20)
                        {
                            fprintf(stderr, "\nERROR: TTY is to long\n\n");
                            usage();
                            exit(1);
                        }
                    strlcpy(tty, optarg, sizeof(tty));
                    break;
                case 'l':
                    if (strlen(optarg) > MAXPATH)
                        {
                            fprintf(stderr, "\nERROR: State filename to long!\n\n");
                            usage();
                            exit(1);
                        }
                    strlcpy(statefile, optarg, sizeof(statefile));
                    break;
                case 'L':
                    if (strlen(optarg) > MAXPATH)
                        {
                            fprintf(stderr, "\nERROR: Number filename to long!\n\n");
                            usage();
                            exit(1);
                        }
                    strlcpy(numbersfile, optarg, sizeof(numbersfile));
                    break;
                case 'f':
                    if (strlen(optarg)> MAXPATH)
                        {
                            fprintf(stderr, "\nERROR: Log file to long\n\n");
                            usage();
                            exit(1);
                        }
                    strlcpy(fileout, optarg, sizeof(fileout));
                    break;
                case 'c':
                    swhandshake=1;
                    hwhandshake=0;
                    break;
                case 'C':
                    if (strlen(optarg)> MAXPATH)
                        {
                            fprintf(stderr, "\nERROR: Configuration file path to long\n\n");
                            usage();
                            exit(1);
                        }
                    strlcpy(iwarconf, optarg, sizeof(iwarconf));
                    break;
                case 'r':
                    if (strlen(optarg) > 40)
                        {
                            fprintf(stderr, "\nERROR: Scan range to long!\n\n");
                            usage();
                            exit(1);
                        }
                    strlcpy(tmpscanrange, optarg, sizeof(tmpscanrange));
                    break;
                case 'e':
                    if (strlen(optarg) > 40)
                        {
                            fprintf(stderr, "\nError: Pre-dial to long\n\n");
                        }
                    strlcpy(predial, optarg, sizeof(predial));
                    break;
                case 'g':
                    if (strlen(optarg) > 40)
                        {
                            fprintf(stderr, "\nError: Post-dial to long\n\n");
                            exit(1);
                        }
                    strlcpy(postdial, optarg, sizeof(postdial));
                    break;
                case 'a':
                    tonedetect=1;
                    break;
                case 'x':
                    dialtype=1;
                    break;
                case 'F':
                    logtype=1;
                    break;
                case 'b':
                    bannercheck=0;
                    break;
                case 'o':
                    record=0;
                    break;
                case 'm':
                default:
                    usage();
                    exit(0);
                    break;
                }
        }


    /* If we're loading numbers from a user generated file or a */
    /* previous state,  we can skip a lot of the number         */
    /* verification                                             */

    if ( statefile[0] == '\0' && numbersfile[0] == '\0' )
        {

            /* Verify we have some sort of range.  (Cygwin _really_ needs this) */

            if ( tmpscanrange[0] == '\0' )
                {
                    printf("ERROR: No range specified! (See the -r option)\n");
                    usage();
                    exit(1);
                }

            /* convert user input into something usable */

            tmp3 = strtok(tmpscanrange, "-");
            snprintf(startscan, sizeof(startscan), "%s", tmp3);
            ss=atoll(startscan);

            tmp4  = strtok(NULL, "-");
            snprintf(endscan, sizeof(endscan), "%s", tmp4);
            es=atoll(endscan);

            /* Make sure user input is valid */

            if (es == 0  || ss == es)
                {
                    fprintf(stderr, "\nERROR: Invalid dial range specified!\n\n");
                    usage();
                    exit(1);
                }

            if ( ss > es )
                {
                    fprintf(stderr, "\nERROR: Start is GREATER than the ending!\n\n");
                    usage();
                    exit(1);
                }

            if ( es > RAND_MAX && dialtype == 0)
                {
                    fprintf(stderr, "\nERROR: Largest random number is %d.  Try using the predial string.\n\n", RAND_MAX);
                    usage();
                    exit(1);
                }

            if ( es-ss > 100 )
                {
                    fprintf(stderr, "\nWARNING : You're dialing a 100 numbers or more!\n\n");
                }

        }


    /**********************************/
    /* Load system banners into array */
    /**********************************/

    printf("===============================================================================\n");
    if (bannercheck==1)
        {
            if ((banner = fopen(bannerfile, "r")) == NULL)
                {
                    fprintf(stderr, "\nERROR: Cannot load %s!\n\n", bannerfile);
                    exit(1);
                }

            bannercount=0;
            while (fgets(bannerbuf,1024,banner) != NULL)
                {
                    if (bannerbuf[0] == '#') continue;
                    if (bannerbuf[0] == 10 ) continue;

                    /* Simple test to see if the banner lines are correct,  under */
                    /* Cygwin and possibly Mac OSX,  if the banner file is        */
                    /* incomplete (missing a |),  it causes a seg. fault          */

                    if (!strstr(bannerbuf, "|"))
                        {
                            fprintf(stderr, "ERROR: %s is incomplete/corrupt (missing |)\n\n", BANNER_FILE_PATH);
                            exit(1);
                        }

                    tmp3 = strtok(bannerbuf, "|");
                    snprintf(bannercfg[bannercount].search_string, sizeof(bannercfg[bannercount].search_string), "%s", tmp3);
                    tmp3 = strtok(NULL, "|");
                    snprintf(tmp, sizeof(tmp), "%s", tmp3);
                    strlcpy(bannercfg[bannercount].os_type, tmp, sizeof(bannercfg[bannercount].os_type));
                    bannercount++;
                }

            printf("Remote identification banners loaded : %d\n", bannercount);
            fclose(banner);
        }

    /**************************************/
    /* Load blacklisted number into array */
    /**************************************/

    if ((blacklst = fopen(blacklistfile, "r"))==NULL)
        {
            fprintf(stderr, "\nERROR: Cannot load blacklist file (%s)!\n\n", blacklistfile);
            exit(1);
        }

    blacklistcount=0;
    while (fgets(buf2,sizeof(buf2),blacklst) != NULL)
        {
            if (buf2[0] == '#') continue;
            if (buf2[0] == 10 ) continue;

            blacklistnum[blacklistcount]=atoll(buf2);
            blacklistcount++;
        }

    fclose(blacklst);
    printf("Blacklisted phone numbers            : %d\n", blacklistcount);

    /***************************************/
    /* Loading user generated number lists */
    /***************************************/

    if ( numbersfile[0] != '\0' )
        {
            printf("Loading user generated numbers list  : ");

            if (( userloadfile = fopen(numbersfile, "r" )) == NULL)
                {
                    printf("Can't read %s!\n", numbersfile);
                    exit(1);
                }

            userlistcount=0;
            while(fgets(buf2,sizeof(buf2),userloadfile) != NULL)
                {
                    if (buf2[0] == '#') continue;
                    if (buf2[0] == 10 ) continue;
                    userlistnum[userlistcount] = atoll(buf2);
                    userlistcount++;
                }
            fclose(userloadfile);
            printf("%d numbers loaded\n", userlistcount);
        }

    /***************************************/
    /* Load from a previously generated    */
    /* state file                          */
    /***************************************/

    /* You can't load a state file _and_ a numbers list! */

    if ( statefile[0] != '\0' && numbersfile[0] == '\0' )
        {
            printf("Loading previous state file          : ");

            if ((saveloadstate = fopen(statefile, "r")) == NULL)
                {
                    printf("Can't read %s!\n", statefile);
                    exit(1);
                }

            while(fgets(buf2,sizeof(buf2),saveloadstate) != NULL)
                {
                    if (buf2[0] == '#') continue;
                    if (buf2[0] == 10 ) continue;
                    num[savestatecount] = atoll(buf2);
                    savestatecount++;
                }

            if ( savestatecount < 2)
                {
                    printf("File appears to be incorrect [%d]\n", j);
                    exit(1);
                }

            savestate=1;

            dialtype=num[0];                /* dial type */
            ss=num[1];
            es=num[2];

            /* In case they save state again */

            snprintf(startscan, sizeof(startscan), "%lld", ss);
            snprintf(endscan, sizeof(endscan), "%lld", es);

            if ( ss == 0 || es == 0)
                {
                    printf("File loaded,  but data is incorrect.\n");
                    exit(1);
                }

            printf("%d numbers loaded\n", savestatecount-3);
            fclose(saveloadstate);
        }

    printf("===============================================================================\n");

    printf("Firing up iwar version %s! ......\n\n", VERSION);

    /* Type of dialtype.... Random/Seq (0/1) */

    if ( dialtype==0 )
        {
            strlcpy(tmp, "Random", sizeof(tmp));
        }
    else
        {
            strlcpy(tmp, "Seq.", sizeof(tmp));
        }

    /* If loaded from a previous state file,  we don't care about the dialtype */

    if ( numbersfile[0] != '\0' )
        {
            strlcpy(tmp, "User Gen.", sizeof(tmp));
        }


    sleep(2);

    /* Dump to the logfile out scan information */

    if ((outfd = fopen(fileout, "a")) == NULL)
        {
            fprintf(stderr, "\nERROR: Can't open %s for output\n\n", fileout);
            usage();
            exit(1);
        }


    /********************************************/
    /* Dump dialing information to our log file */
    /********************************************/

    fprintf(outfd, "\n\n------------------------------------------------------------------------------\n");
    fprintf(outfd, "= iWar version %s - By Da Beave (beave@softwink.com)\n", VERSION);

    fprintf(outfd, "= Port Settings : %s,%s,%s (%s)\n", baudrate, bits, parity, tty);
    fprintf(outfd, "= HW Handshaking: %d  | Tone location : %d\n", hwhandshake, tonedetect);

    if ( numbersfile[0] != '\0' )
        {
            fprintf(outfd, "= Start of scan: %lld | End of scan: %lld (Total Numbers: %d)\n", userlistnum[0], userlistnum[userlistcount-1], userlistcount);
        }
    else
        {
            fprintf(outfd, "= Start of scan: %lld | End of scan: %lld (Total Numbers: %lld)\n", ss,es,es-ss);
        }

    if ( predial[0] == '\0' )
        {
            fprintf(outfd, "= Pre dial: [None] | ");
        }
    else
        {
            fprintf(outfd, "= Pre dial: %s | ", predial);
        }

    if ( postdial[0] == '\0' )
        {
            fprintf(outfd, "Post Dial: [None]\n");
        }
    else
        {
            fprintf(outfd, "Post Dial: %s\n", postdial);
        }

    fprintf(outfd, "= Dial Type: %s | Logfile: %s\n",tmp, fileout);

    t = time(NULL);
    now=localtime(&t);
    strftime(tmp2, sizeof(tmp2), "%H:%M:%S %F",  now);

    fprintf(outfd, "= Scan Start: %s\n", tmp2);
    fprintf(outfd, "------------------------------------------------------------------------------\n");
    fflush(outfd);
    fclose(outfd);

    /**************************************************************/
    /* Open serial port,  unless IAX2 is being used               */
    /**************************************************************/

    portfd = open(tty, O_RDWR);
    if  (portfd == -1)
        {
            fprintf(stderr, "ERROR: Can't open %s.\n", tty);
            exit(1);
        }

    m_savestate(portfd); 	/* Save current state & setup the port */
    m_setparms(portfd,baudrate,parity,bits,hwhandshake,swhandshake);
    m_nohang(portfd);       /* Do we need to do this? */
    m_hupcl(portfd, 1);
    m_flush(portfd);        /* Flush any old data out before we start */

    /* What to do in the event of a signal */

    signal (SIGHUP,  &closetty);
    signal (SIGINT,  &closetty);
    signal (SIGQUIT, &closetty);
    signal (SIGTERM, &closetty);
    signal (SIGABRT, &closetty);

    /* Draw main screen/window.... */

    initscr();
    noecho();
    cbreak();
    timeout(0);                       /* Always process user input,  don't wait */
    start_color();
    clear();

    NCURSES_Mainscreen();

    /* Incase user resizes the screen down the road */

    getmaxyx(stdscr,maxrow,maxcol);
    oldmaxrow=maxrow;
    oldmaxcol=maxcol;

    drawinfo(baudrate,bits,parity,tty,numbersfile,predial,postdial,logtype,
             connect,nocarrier,busy,voice,tonesilence,dialtype,timeout);

    modemstatus = newwin(6, maxcol-5, maxrow-7,2);  /* Our terminal screen */
    scrollok(modemstatus, TRUE);
    wrefresh(modemstatus);

    /* Seed random number generator */

    srand( (unsigned int)time( NULL ) );

    NCURSES_Intro();   /* Hi,  I'm Beave and I wrote this program.... */

    if ( modeminit[0] != '\0' )
        {
            sendmodem(modeminit);
            NCURSES_Status("Initializing Modem....");
            ninfo("Initializing Modem....", 2);
            sleep(1);  /* Give's me a warm fuzzy */
            ok=1;
        }

    getnum(dialtype, mysqllog, tonedetect, predial, postdial);

    NCURSES_Status(sendstring);
    sendmodem(sendstring);


    /* If we are loading this from a previous state/file,   go ahead and print */
    /* the numbers in COLOR_PAIR(14) that have already been dialed             */

    if (savestate == 1)
        {
            for (i=3; i<savestatecount; i++)
                {
                    attron( COLOR_PAIR(14) | A_NORMAL );
                    NCURSES_Plot(num[i], row, col);
                    attroff(COLOR_PAIR(14) | A_NORMAL );
                    rowcolcheck();
                }
        }

    /* We now enter the terminal loop */

    while(1)
        {
            tv.tv_sec=1;			/* 1 second */
            tv.tv_usec=0;

            FD_ZERO(&fds );
            FD_SET(portfd, &fds);

            if (select(portfd+1, &fds, NULL, NULL, &tv) > 0 )
                {
                    buflen = read(portfd, buf, 1);
                    if (buflen == -1) closetty(-1);
                    waitin=0;              /* Got data, reset out nodata counter */

                    for (i=0; i<strlen(buf); i++)
                        {
                            snprintf(tmpscanbuf, sizeof(tmpscanbuf), "%c", buf[i]);
                            ch=buf[i];
                            if (record == 1) strlcat(recordbuf, tmpscanbuf, sizeof(recordbuf));

                            if (ch == 10)
                                {
                                    waddch(modemstatus, 10);
                                    wrefresh(modemstatus);
                                }

                            /* Cheap modems upon DTR drop fill the buffer with
                               garbage.  Namely -128.  We filter this out and
                            some other control characters to keep the
                             terminal window semi-clean */

                            if (iscntrl(ch) || ch == -128 )
                                {
                                    tmpscanbuf[0] = '\0';
                                    scanbuf[0] = '\0';
                                }
                            else
                                {
                                    waddch(modemstatus, ch);
                                    wrefresh(modemstatus);
                                    strlcat(scanbuf, tmpscanbuf, sizeof(scanbuf));
                                }

                            /* if connected,  count the bytes */


                            if (connectflag==1 && bannercheck == 1 ) bitcount++;

                            if (bitcount > bannermaxcount)
                                {
                                    NCURSES_Status("Max banner data received");
                                    loginfo(mysqllog, "CONNECT", "Not Identified (Max. banner data received)", recordbuf);
                                    if ( plushang == 1)
                                        {
                                            plushangup(plushangupsleep);
                                        }
                                    else
                                        {
                                            m_dtrtoggle(portfd,dtrsec);
                                            if (dtrinit == 1) dtrreinit(modeminit, volume);
                                        }


                                    connectflag=0;
                                    sleep(conredial);

                                    recordbuf[0] = '\0';
                                    scanbuf[0] = '\0';
                                    tmpscanbuf[0] = '\0';

                                    getnum(dialtype, mysqllog, tonedetect, predial,  postdial);
                                    waddch(modemstatus, 10);
                                    NCURSES_Status(sendstring);
                                    sendmodem(sendstring);
                                    bitcount=0;
                                    ring=0;
                                }

                            /* USR Courier send "NO DIAL TONE, other modems send "NO DIALTONE" */

                            if (strstr(scanbuf, "NO DIALTONE") || strstr(scanbuf, "NO DIAL TONE"))
                                {
                                    NCURSES_Status("No Dialtone!");
                                    ninfo("NO DIALTONE! Waiting to retry......",2);
                                    touchwin(modemstatus);
                                    wrefresh(modemstatus);

                                    /* we resend the same number we attempted */
                                    snprintf(tmp, sizeof(tmp), "Redialing: %lld", dialnum);
                                    NCURSES_Status(tmp);
                                    sleep(redial);
                                    NCURSES_Status(sendstring);
                                    sendmodem(sendstring);
                                    scanbuf[0] = '\0';
                                    tmpscanbuf[0] = '\0';
                                }

                            if (!strcmp(scanbuf, "CONNECT"))
                                {
                                    NCURSES_Status("CONNECTED!");
                                    if ( beepflag == 1 ) beep();
                                    attron( COLOR_PAIR(11) | A_BLINK );
                                    NCURSES_Plot(dialnum, row, col);
                                    attroff( COLOR_PAIR(11) | A_BLINK );
                                    rowcolcheck();
                                    connect++;
                                    nright(1, connect);
                                    if (bannercheck == 0 && record != 1)
                                        {
                                            loginfo(mysqllog, "CONNECT", "", recordbuf);
                                            recordbuf[0] = '\0';

                                            if ( plushang == 1)
                                                {
                                                    plushangup(plushangupsleep);
                                                }
                                            else
                                                {
                                                    m_dtrtoggle(portfd,dtrsec); /* DTR Hangup please */
                                                    if (dtrinit == 1) dtrreinit(modeminit, volume);
                                                }

                                            connectflag=0;
                                            sleep(conredial);

                                            scanbuf[0] = '\0';
                                            tmpscanbuf[0] = '\0';
                                            getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                                            NCURSES_Status(sendstring);
                                            sendmodem(sendstring);
                                        }
                                    else
                                        {
                                            connectflag=1;
                                        }
                                }

                            if ( ( !strcmp(scanbuf, "NO CARRIER") && waitin > 5 ) ||
                                  ( !strcmp(scanbuf, "NO ANSWER")  && waitin > 5 ) ||
                                  ( !strcmp(scanbuf, "NO CARRIER") && connectflag == 1 ) ||
                                  (  !strcmp(scanbuf, "NO ANSWER")  && connectflag == 1 ) )

                                {
                                    sendcr=0;
                                    ring=0;
                                    if (connectflag==1)   /* We'll get a NO CARRIER once
		                              a connection drops */
                                        {
                                            connectflag=0;
                                            loginfo(mysqllog, "CONNECT", "Not Identified (disconnected)", recordbuf);
                                            recordbuf[0] = '\0';
                                            NCURSES_Status("Connection Dropped.");
                                            sleep(redial);
                                            getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                                            NCURSES_Status(sendstring);
                                            sendmodem(sendstring);
                                            scanbuf[0] = '\0';
                                            tmpscanbuf[0] = '\0';
                                        }
                                    else
                                        {
                                            NCURSES_Status("NO CARRIER");
                                            attron( COLOR_PAIR(10) | A_NORMAL );
                                            NCURSES_Plot(dialnum, row, col);
                                            attroff( COLOR_PAIR(10) | A_NORMAL );
                                            rowcolcheck();
                                            nocarrier++;
                                            nright(2, nocarrier);
                                            if (logtype==1)
                                                {
                                                    loginfo(mysqllog, "NO CARRIER", "", "");
                                                }

                                            sleep(redial);
                                            scanbuf[0] = '\0';
                                            tmpscanbuf[0] = '\0';
                                            recordbuf[0] = '\0';


                                            getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                                            NCURSES_Status(sendstring);
                                            sendmodem(sendstring);
                                        }

                                    if (connectflag==2)
                                        {

                                            scanbuf[0] = '\0';
                                            tmpscanbuf[0] = '\0';
                                            recordbuf[0] = '\0';

                                            connectflag=0;
                                        }
                                }

                            /* Count the number of RINGING it silence detections */
                            /* is enabled...                                     */

                            if (!strcmp(scanbuf, "RINGING") && remotering != 0 )
                                {
                                    ring++;
                                    snprintf(tmp, sizeof(tmp), "RINGING (# %d)", ring);
                                    recordbuf[0] = '\0';
                                    NCURSES_Status(tmp);
                                }

                            /* No silence detections..  Just report RINGING */

                            if (!strcmp(scanbuf, "RINGING") && remotering == 0)
                                {
                                    NCURSES_Status("RINGING....");
                                    recordbuf[0] = '\0';
                                }

                            /* If we are doing Toneloc style tone detection,  we look
                            for a "OK" after the ATDT5551212W;  */

                            if (!strcmp(scanbuf, "OK") && tonedetect==1 && ok == 0)
                                {
                                    if ( beepflag == 1) beep();
                                    loginfo(mysqllog, "TONE", "", "");
                                    tonesilence++;
                                    nright(5, tonesilence);
                                    NCURSES_Status("Found Tone!");
                                    attron( COLOR_PAIR(13) | A_STANDOUT );
                                    NCURSES_Plot(dialnum, row, col);
                                    attroff( COLOR_PAIR(13) | A_STANDOUT );
                                    rowcolcheck();
                                    getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                                    waddch(modemstatus, 10);
                                    sendmodem("ATH0\r");
                                    sleep(redial);
                                    NCURSES_Status(sendstring);
                                    sendmodem(sendstring);
                                    scanbuf[0] = '\0';
                                    tmpscanbuf[0] = '\0';
                                    recordbuf[0] = '\0';
                                    ring=0;
                                    ok = 1;
                                }

                            /* If we've done something like,  re-init'ed the modem
                            or change the volume,  we don't want to confuse
                            tone location,  so we ignore it on these events */

                            if (!strcmp(scanbuf, "OK") && tonedetect==1 && ok == 1 )
                                {
                                    ok = 0;
                                }

                            if (!strcmp(scanbuf, "BUSY"))
                                {
                                    NCURSES_Status("BUSY");
                                    attron( COLOR_PAIR(12) | A_BOLD );
                                    NCURSES_Plot(dialnum, row, col);
                                    attroff( COLOR_PAIR(12) | A_BOLD );
                                    rowcolcheck();
                                    busy++;
                                    nright(3, busy);
                                    if (logtype==1)
                                        {
                                            loginfo(mysqllog, "BUSY", "", "");
                                        }

                                    recordbuf[0] = '\0';
                                    sleep(redial);
                                    getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                                    NCURSES_Status(sendstring);
                                    sendmodem(sendstring);
                                    scanbuf[0] = '\0';
                                    tmpscanbuf[0] = '\0';

                                    ring=0;  /* yes,  weird */
                                }

                            /* Legend has it that some modems will return a "TONE"
                            when encountering a.. well.. tone...  I haven't been
                            able to lay hands on one of these modems,  but why
                            not support it anyways */

                            if (!strcmp(scanbuf, "TONE"))
                                {
                                    tonesilence++;
                                    NCURSES_Status("TONE");
                                    attron( COLOR_PAIR(13) | A_STANDOUT );
                                    NCURSES_Plot(dialnum, row, col);
                                    attroff( COLOR_PAIR(13) | A_STANDOUT);
                                    rowcolcheck();
                                    getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                                    waddch(modemstatus, 10);
                                    sleep(redial);
                                    NCURSES_Status(sendstring);
                                    sendmodem(sendstring);
                                    scanbuf[0] = '\0';
                                    tmpscanbuf[0] = '\0';
                                    recordbuf[0] = '\0';
                                    ring=0;
                                }

                            if (!strcmp(scanbuf, "VOICE"))
                                {
                                    NCURSES_Status("VOICE");
                                    attron( COLOR_PAIR(13) | A_UNDERLINE );
                                    NCURSES_Plot(dialnum, row, col);
                                    attroff(COLOR_PAIR(13) | A_UNDERLINE );
                                    rowcolcheck();
                                    voice++;
                                    nright(4, voice);
                                    if (logtype==1)
                                        {
                                            loginfo(mysqllog, "VOICE", "", "");
                                        }

                                    recordbuf[0] = '\0';
                                    sleep(redial);
                                    getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                                    NCURSES_Status(sendstring);
                                    sendmodem(sendstring);
                                    scanbuf[0] = '\0';
                                    tmpscanbuf[0] = '\0';
                                    recordbuf[0] = '\0';
                                    ring=0;
                                }

                            /* Error,  inform the user but keep trying */

                            if (!strcmp(scanbuf, "ERROR"))
                                {
                                    ninfo("Modem ERROR", 1);
                                    touchwin(modemstatus);
                                    wrefresh(modemstatus);
                                    scanbuf[0] = '\0';
                                    tmpscanbuf[0] = '\0';
                                    recordbuf[0] = '\0';
                                }

                        }
                }

            /* Allows user to tail -f IAX2 debug file in real time.  Other wise,
               you have to wait till the buffer is full or you exit */

            /* Deal with connected/carriers and identification.  Watch the bannercount */
            /* make sure we don't go over the limit.  Also keep a eye out for the      */
            /* system type.   We do all sorts of stuff here ........                   */

            if (connectflag==1)
                {
                    for (b=0; b<bannercount; b++)
                        {
                            if (strstr(scanbuf, bannercfg[b].search_string))
                                {
                                    loginfo(mysqllog, "CONNECT", bannercfg[b].os_type, recordbuf);
                                    recordbuf[0] = '\0';
                                    NCURSES_Status("System Identified!");
                                    if ( beepflag == 1 )
                                        {
                                            beep();
                                            beep();
                                        }
                                    if ( plushang == 1)
                                        {
                                            plushangup(plushangupsleep);
                                        }
                                    else
                                        {
                                            m_dtrtoggle(portfd,dtrsec);
                                            if ( dtrinit == 1 ) dtrreinit(modeminit, volume);
                                        }

                                    sleep(conredial);
                                    scanbuf[0] = '\0';
                                    tmpscanbuf[0] = '\0';
                                    recordbuf[0] = '\0';

                                    getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                                    NCURSES_Status(sendstring);
                                    sendmodem(sendstring);
                                    connectflag=0;
                                    bitcount=0;
                                }
                        }

                    if (waitin == bannersendcrtimeout && sendcr == 0)
                        {
                            for (i=0; i<bannersendcr; i++)
                                {
                                    snprintf(tmp, sizeof(tmp), "\r");
                                    sendmodem(tmp);
                                    bitcount=0;
                                    sendcr=1;		/* Avoid re-sending CR's */
                                }
                            NCURSES_Status("Sent CRs [Waiting for response]");
                        }

                    /* Timer timed out.  We stopped receiving data from the remote system */

                    if (waitin == bannertimeout)
                        {
                            if (bannercheck == 1)
                                {
                                    loginfo(mysqllog, "CONNECT", "Not Identified (stalled)", recordbuf);
                                }
                            else
                                {
                                    loginfo(mysqllog, "CONNECT", "Banner detection disabled", recordbuf);
                                }

                            if (bannercheck == 1)
                                {
                                    NCURSES_Status("Data Trans. Stalled.");
                                }
                            else
                                {
                                    NCURSES_Status("Banner recorded,  hanging up...");
                                }

                            if ( plushang == 1)
                                {
                                    plushangup(plushangupsleep);
                                }
                            else
                                {
                                    m_dtrtoggle(portfd,dtrsec);
                                    if (dtrinit == 1)  dtrreinit(modeminit, volume);
                                }

                            sleep(conredial);

                            scanbuf[0] = '\0';
                            tmpscanbuf[0] = '\0';
                            recordbuf[0] = '\0';

                            connectflag=0;
                            getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                            NCURSES_Status(sendstring);
                            sendmodem(sendstring);
                            waitin=0;
                        }
                }

            /* Either we waited and got nothing,  or we hit the max number of rings */
            /* This is for silence detections */

            NCURSES_Timer(waitin);
            wrefresh(modemstatus);
            waitin++;

            if (waitin == ringtimeout && ring < remotering && ring != 0)
                {
                    if ( beepflag == 1)
                        {
                            beep();
                        }

                    loginfo(mysqllog, "Possible Interesting Number", "", "");
                    sendmodem("\r");
                    NCURSES_Status("Possible Interesting Number");
                    tonesilence++;
                    nright(5, tonesilence);
                    attron( COLOR_PAIR(13) | A_STANDOUT );
                    NCURSES_Plot(dialnum, row, col);
                    attroff( COLOR_PAIR(13) | A_STANDOUT );
                    rowcolcheck();
                    sleep(conredial);

                    scanbuf[0] = '\0';
                    tmpscanbuf[0] = '\0';
                    recordbuf[0] = '\0';

                    getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                    NCURSES_Status(sendstring);
                    sendmodem(sendstring);
                    waitin=0;
                    ring=0;
                }

            /* We saw X number of rings,  we can safely assume */
            /* nothing is going to answer                      */

            if (remotering != 0 && ring >= remotering )
                {
                    NCURSES_Status("Max. Rings Received");
                    sendmodem("\r");
                    waddch(modemstatus, 10);
                    timeout++;   /* Should make this it's own item one day */
                    nright(6,timeout);
                    waitin=0;
                    ring=0;
                    sleep(redial);
                    attron( COLOR_PAIR(10) | A_NORMAL );
                    NCURSES_Plot(dialnum, row, col);
                    attroff( COLOR_PAIR(10) | A_NORMAL );
                    rowcolcheck();
                    getnum(dialtype, mysqllog, tonedetect, predial, postdial);

                    scanbuf[0] = '\0';
                    tmpscanbuf[0] = '\0';
                    recordbuf[0] = '\0';

                    NCURSES_Status(sendstring);
                    sendmodem(sendstring);
                }

            /* We waited,  and nothing happened.  */

            if (waitin == serialtimeout)
                {
                    NCURSES_Status("Timeout.");
                    if (logtype==1)
                        {
                            loginfo(mysqllog, "Timeout", "", "");
                        }

                    timeout++;

                    sendmodem("\r");

                    nright(6,timeout);
                    attron( COLOR_PAIR(10) | A_NORMAL );
                    NCURSES_Plot(dialnum, row, col);
                    attroff( COLOR_PAIR(10) | A_NORMAL );
                    rowcolcheck();
                    waddch(modemstatus, 10);
                    waitin=0;
                    sleep(conredial);

                    scanbuf[0] = '\0';
                    tmpscanbuf[0] = '\0';
                    recordbuf[0] = '\0';

                    getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                    NCURSES_Status(sendstring);
                    sendmodem(sendstring);
                }


            /* Did the user resize the screen at any point?  If so,  redraw */
            /* the information                                              */

            getmaxyx(stdscr,maxrow,maxcol);
            if (oldmaxrow != maxrow || oldmaxcol != maxcol)
                {
                    clear();
                    NCURSES_Mainscreen();
                    oldmaxrow=maxrow;
                    oldmaxcol=maxcol;
                    delwin(modemstatus);
                    modemstatus = newwin(6, maxcol-5, maxrow-7,2);
                    wrefresh(modemstatus);
                    drawinfo(baudrate,bits,parity,tty,numbersfile,predial,postdial,logtype,
                             connect,nocarrier,busy,voice,tonesilence,dialtype,timeout);
                }


            /**************************************************************************/
            /* Get keyboard input.  This allows us to do things like mark numbers,    */
            /* toggle settings,  etc... etc..                                         */
            /**************************************************************************/

            key = tolower(getch());

            if (key != ERR)
                {

                    if ( key == (int)'a' )
                        {
                            dialnum=0;
                            loginfo(0, "User Abort", "", "");
                            sendmodem("\r");
                            NCURSES_Status("User Abort!");
                            ninfo("User Abort!",2);
                            clear();
                            endwin();
                            exitscreen(connect, nocarrier, voice, busy, tonesilence, mark, skip);
                            fflush(stderr);
                            fclose(stderr);
                        }

                    if ( key == (int)'b' )
                        {
                            if ( beepflag == 0 )
                                {
                                    beepflag=1;
                                    ninfo("Beep Enabled", 2);
                                }
                            else
                                {
                                    beepflag=0;
                                    ninfo("Beep Disabled",2);
                                }
                            touchwin(modemstatus);
                            wrefresh(modemstatus);
                        }

                    if ( key == (int)' ' )
                        {
                            NCURSES_Status("Skipping number....");

                            sendmodem("\r");

                            waitin=0;
                            connectflag=0;   /* In case we are connected */
                            skip++;
                            waddch(modemstatus, 10);  /* Go to next line */
                            if (logtype==1)
                                {
                                    loginfo(mysqllog, "Number Skipped", "", "");
                                }

                            attron( COLOR_PAIR(16) | A_NORMAL );
                            NCURSES_Plot(dialnum, row, col);
                            attroff( COLOR_PAIR(16) | A_NORMAL );
                            rowcolcheck();

                            scanbuf[0] = '\0';
                            tmpscanbuf[0] = '\0';

                            if (dtrinit == 1)
                                {
                                    dtrreinit(modeminit, volume);
                                }

                            sleep(redial);
                            getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                            NCURSES_Status(sendstring);
                            sendmodem(sendstring);
                        }

                    if ( key == (int)'m' ||
                            key == (int)'c' ||
                            key == (int)'f' ||
                            key == (int)'t' ||
                            key == (int)'v' ||
                            key == (int)'x' )
                        {

                            if ( key == (int)'m' )
                                {
                                    strlcpy(tmp, "Marked (Quick)", sizeof(tmp));
                                    strlcpy(tmp2, "Marked as interesting", sizeof(tmp2));
                                }
                            if ( key == (int)'c' )
                                {
                                    strlcpy(tmp, "Marked (CARRIER)", sizeof(tmp));
                                    strlcpy(tmp2, "Marked - CARRIER", sizeof(tmp2));
                                }
                            if ( key == (int)'f' )
                                {
                                    strlcpy(tmp, "Marked (FAX)", sizeof(tmp));
                                    strlcpy(tmp2, "Marked - FAX", sizeof(tmp2));
                                }
                            if ( key == (int)'t' )
                                {
                                    strlcpy(tmp, "Marked (TELCO/TONE)", sizeof(tmp));
                                    strlcpy(tmp2, "Marked - TELCO/TONE", sizeof(tmp2));
                                }
                            if ( key == (int)'v' )
                                {
                                    strlcpy(tmp, "Marked (VOICE MAIL SYSTEM)", sizeof(tmp));
                                    strlcpy(tmp2, "Marked - VOICE MAIL SYSTEM", sizeof(tmp2));
                                }
                            if ( key == (int)'x' )
                                {
                                    strlcpy(tmp, "Marked (PBX)", sizeof(tmp));
                                    strlcpy(tmp2, "Marked - PBX", sizeof(tmp2));
                                }

                            if (connectflag == 1)
                                {
                                    if (plushang)
                                        {
                                            plushangup(plushangupsleep);
                                        }
                                    else
                                        {
                                            m_dtrtoggle(portfd,dtrsec);
                                            if (dtrinit == 1 ) dtrreinit(modeminit, volume);
                                        }
                                }

                            sendmodem("\r");

                            waitin=0;
                            mark++;
                            ninfo(tmp,2);
                            loginfo(mysqllog, "MARK", tmp, "");
                            attron( COLOR_PAIR(11) | A_STANDOUT );
                            NCURSES_Plot(dialnum, row, col);
                            attroff( COLOR_PAIR(11) | A_STANDOUT );
                            rowcolcheck();

                            touchwin(modemstatus);
                            wrefresh(modemstatus);

                            touchwin(modemstatus);
                            waddch(modemstatus,10);
                            wrefresh(modemstatus);

                            sleep(redial);
                            getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                            NCURSES_Status(sendstring);

                            sendmodem(sendstring);
                        }


                    if ( key == (int)'0' )
                        {
                            ninfo("Modem speaker off",2);
                            NCURSES_Status("Sending ATM0 Next Pass....");
                            volume=0;
                            touchwin(modemstatus);    /* touchwin the terminal window */
                            wrefresh(modemstatus);
                            strlcpy(modemqueue, "ATM0\r", sizeof(modemqueue));
                        }

                    if ( key == (int)'1' )
                        {
                            ninfo("Modem speaker volume 1", 2);
                            NCURSES_Status("Sending ATM1L1 Next Pass....");
                            volume=1;
                            touchwin(modemstatus);
                            wrefresh(modemstatus);
                            strlcpy(modemqueue, "ATM1L1\r", sizeof(modemqueue));
                        }

                    if ( key == (int)'2' )
                        {
                            ninfo("Modem speaker volume 2", 2);
                            NCURSES_Status("Sending ATM1L2 Next Pass....");
                            volume=2;
                            touchwin(modemstatus);
                            wrefresh(modemstatus);
                            strlcpy(modemqueue, "ATM1L2\r", sizeof(modemqueue));
                        }

                    if ( key == (int)'3' )
                        {
                            ninfo("Modem speaker volume 3/Max.", 2);
                            NCURSES_Status("Sending ATM1L3 Next Pass....");
                            volume=3;
                            touchwin(modemstatus);
                            wrefresh(modemstatus);
                            strlcpy(modemqueue, "ATM1L3\r", sizeof(modemqueue));
                        }

                    if ( key == (int)'+' )
                        {
                            serialtimeout=serialtimeout+5;
                            snprintf(tmp, sizeof(tmp), "Added 5 secs. [Now: %d]", serialtimeout);
                            NCURSES_Status(tmp);
                        }

                    if ( key == (int)'-' )
                        {
                            serialtimeout=serialtimeout-5;
                            snprintf(tmp, sizeof(tmp), "Subtract 5 secs. [Now: %d]", serialtimeout);
                            NCURSES_Status(tmp);
                        }

                    if ( key == (int)'k' )
                        {
                            if (connectflag == 1)
                                {
                                    if (plushang == 1 )
                                        {
                                            plushangup(plushangupsleep);
                                        }
                                    else
                                        {
                                            m_dtrtoggle(portfd,dtrsec);
                                            if (dtrinit == 1 ) dtrreinit(modeminit, volume);
                                        }
                                }

                            sendmodem("\r");

                            waitin=0;
                            mark++;
                            strlcpy(tmp, nsimpleform(), sizeof(tmp));
                            loginfo(mysqllog, "MARK", tmp, "");
                            touchwin(modemstatus);
                            wrefresh(modemstatus);

                            ninfo("Marked (Custom Input)", 2);

                            touchwin(modemstatus);
                            wrefresh(modemstatus);

                            attron( COLOR_PAIR(11) | A_STANDOUT );
                            NCURSES_Plot(dialnum, row, col);
                            attroff( COLOR_PAIR(11) | A_STANDOUT );
                            rowcolcheck();

                            touchwin(modemstatus);
                            wrefresh(modemstatus);

                            getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                            NCURSES_Status(sendstring);
                            waddch(modemstatus, 10);
                            sendmodem(sendstring);
                        }

                    if ( key == (int)'s' )
                        {
                            NCURSES_Filename(tmp, sizeof(tmp));
                            touchwin(modemstatus);
                            wrefresh(modemstatus);

                            if ( tmp[0] != '\0' )
                                {
                                    if ((saveloadstate = fopen(tmp, "w")) == NULL)
                                        {
                                            ninfo("Cant save state",1);
                                            touchwin(modemstatus);
                                            waddch(modemstatus,10);
                                            wrefresh(modemstatus);
                                        }

                                    /* Note:  This need to become it's own function */

                                    t = time(NULL);
                                    now = localtime(&t);
                                    strftime(tmp2, sizeof(tmp2), "[ %H:%M:%S ]",  now);

                                    fprintf(saveloadstate, "# iWar version %s.  Date: %s\n", VERSION, tmp2);
                                    fprintf(saveloadstate, "# 1 - dialtype (1 Seq/0 Rand)\n");
                                    fprintf(saveloadstate, "# 2 - startscan\n");
                                    fprintf(saveloadstate, "# 3 - endscan \n");
                                    fprintf(saveloadstate, "#     The rest are numbers dialed\n");
                                    fprintf(saveloadstate, "%d\n", dialtype);
                                    fprintf(saveloadstate, "%s\n", startscan);
                                    fprintf(saveloadstate, "%s\n", endscan);

                                    if (savestate == 1)
                                        {
                                            i=3;
                                        }
                                    else
                                        {
                                            i=0;
                                        }
                                    for (b=i; b<j; b++)
                                        {
                                            fprintf(saveloadstate, "%lld\n", num[b]);
                                        }
                                    fclose(saveloadstate);
                                    snprintf(tmp2, sizeof(tmp2), "State saved to %s", tmp);
                                    touchwin(modemstatus);
                                    wrefresh(modemstatus);
                                    ninfo(tmp2, 2);
                                    tmp[0] = '\0';
                                    touchwin(modemstatus);
                                    wrefresh(modemstatus);

                                }
                            else
                                {

                                    ninfo("Nothing saved.  Scan Continued.", 2);
                                    touchwin(modemstatus);
                                    wrefresh(modemstatus);
                                }
                        }

                    if ( key == (int)'q' )
                        {
                            NCURSES_Filename(tmp, sizeof(tmp));
                            touchwin(modemstatus);
                            wrefresh(modemstatus);

                            if ( tmp[0] != '\0' )
                                {

                                    while ((saveloadstate = fopen(tmp, "w")) == NULL)
                                        {
                                            ninfo("Can't save state file, try again", 1);
                                            touchwin(modemstatus);
                                            wrefresh(modemstatus);
                                            NCURSES_Filename(tmp, sizeof(tmp));
                                        }

                                    t = time(NULL);
                                    now = localtime(&t);
                                    strftime(tmp2, sizeof(tmp2), "[ %H:%M:%S ]",  now);

                                    fprintf(saveloadstate, "# iWar version %s.  Date: %s\n", VERSION, tmp2);

                                    fprintf(saveloadstate, "# 1 - dialtype (1 Seq/0 Rand)\n");
                                    fprintf(saveloadstate, "# 2 - startscan\n");
                                    fprintf(saveloadstate, "# 3 - endscan \n");
                                    fprintf(saveloadstate, "#     The rest are numbers dialed\n");
                                    fprintf(saveloadstate, "%d\n", dialtype);
                                    fprintf(saveloadstate, "%s\n", startscan);
                                    fprintf(saveloadstate, "%s\n", endscan);

                                    if (savestate == 1)
                                        {
                                            i=3;
                                        }
                                    else
                                        {
                                            i=0;
                                        }
                                    for (b=i; b<j; b++)
                                        {
                                            fprintf(saveloadstate, "%lld\n", num[b]);
                                        }
                                    fclose(saveloadstate);
                                    touchwin(modemstatus);
                                    waddch(modemstatus,10);
                                    wrefresh(modemstatus);

                                    snprintf(tmp2, sizeof(tmp2), "State save to %s", tmp);
                                    ninfo(tmp2, 2);
                                    touchwin(modemstatus);
                                    waddch(modemstatus,10);
                                    wrefresh(modemstatus);
                                    ninfo("Exiting iWar!  Bye....",1);
                                    touchwin(modemstatus);
                                    waddch(modemstatus,10);
                                    wrefresh(modemstatus);
                                    endwin();
                                    exitscreen(connect, nocarrier, voice, busy, tonesilence, mark, skip);
                                    fflush(stderr);
                                    fclose(stderr);
                                    exit(0);
                                }
                            else
                                {

                                    ninfo("Nothing saved.  Scan Continued.", 2);
                                    touchwin(modemstatus);
                                    wrefresh(modemstatus);
                                }
                        }


                    if ( key == (int)'p' || key == (int)'[' )
                        {
                            if (connectflag == 1)
                                {
                                    if (plushang == 1 )
                                        {
                                            plushangup(plushangupsleep);
                                        }
                                    else
                                        {
                                            m_dtrtoggle(portfd,dtrsec);
                                            if (dtrinit == 1 ) dtrreinit(modeminit, volume);
                                        }
                                }

                            sendmodem("\r");

                            if ( key == (int)'p')
                                {
                                    NCURSES_Pause(0);
                                }

                            if ( key == (int)'[')
                                {
                                    loginfo(mysqllog, "Paused and marked as interesting", "", recordbuf);
                                    NCURSES_Pause(1);
                                }

                            waitin=0;
                            touchwin(modemstatus);
                            waddch(modemstatus,10);
                            wrefresh(modemstatus);
                            getnum(dialtype, mysqllog, tonedetect, predial, postdial);
                            NCURSES_Status(sendstring);
                            sendmodem(sendstring);
                        }
                }
        }
}  /* End of main(),  sucker */
