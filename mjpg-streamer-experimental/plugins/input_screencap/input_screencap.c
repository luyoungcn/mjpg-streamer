/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"
#include "screencap.h"
#include "common.h"

#define INPUT_PLUGIN_NAME "SCREENCAP input plugin"

/* private functions and variables to this plugin */
static pthread_t   capscreen;
static pthread_t   encode;
static pthread_t   worker;
static globals     *pglobal;

void *worker_thread(void *);
void worker_cleanup(void *);
void help(void);

static int delay = 20;
static int plugin_number;
/*** plugin interface functions ***/
int input_init(input_parameter *param, int id)
{
    int i;
    plugin_number = id;

    param->argv[0] = INPUT_PLUGIN_NAME;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0},
            {"help", no_argument, 0, 0},
            {"d", required_argument, 0, 0},
            {"delay", required_argument, 0, 0},
            {0, 0, 0, 0}
        };

        c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);

        /* no more options to parse */
        if(c == -1) break;

        /* unrecognized option */
        if(c == '?') {
            help();
            return 1;
        }

        switch(option_index) {
            /* h, help */
        case 0:
        case 1:
            DBG("case 0,1\n");
            help();
            return 1;
            break;

            /* d, delay */
        case 2:
        case 3:
            DBG("case 2,3\n");
            delay = atoi(optarg);
            break;
        default:
            DBG("default case\n");
            help();
            return 1;
        }
    }

    pglobal = param->global;

    IPRINT("forced delay......: %i\n", delay);

    param->global->in[id].name = malloc((strlen(INPUT_PLUGIN_NAME) + 1) * sizeof(char));
    sprintf(param->global->in[id].name, INPUT_PLUGIN_NAME);

    return 0;
}

int input_stop(int id)
{
    DBG("will cancel input thread\n");
    // TODO
    // pthread_cancel(capscreen);
    // pthread_cancel(encode);
    // pthread_cancel(worker);
    return 0;
}

int input_run(int id)
{
    pglobal->in[id].buf = NULL;

    if(pthread_create(&capscreen, 0, captureScreen_thread, NULL) != 0) {
        free(pglobal->in[id].buf);
        fprintf(stderr, "could not start capture screen thread\n");
        exit(EXIT_FAILURE);
    }

    pthread_detach(capscreen);

    if(pthread_create(&encode, 0, encode_thread, NULL) != 0) {
        free(pglobal->in[id].buf);
        fprintf(stderr, "could not start encode thread\n");
        exit(EXIT_FAILURE);
    }

    pthread_detach(encode);

    if(pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        free(pglobal->in[id].buf);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }

    pthread_detach(worker);

    return 0;
}

/*** private functions for this plugin below ***/
void help(void)
{
    fprintf(stderr, "TODO\n");
#if 0
    fprintf(stderr, " ---------------------------------------------------------------\n" \
    " Help for input plugin..: "INPUT_PLUGIN_NAME"\n" \
    " ---------------------------------------------------------------\n" \
    " The following parameters can be passed to this plugin:\n\n" \
    " [-d | --delay ]........: delay to pause between frames\n" \
    " [-f | --folder ].......: folder to watch for new JPEG files\n" \
    " [-r | --remove ].......: remove/delete JPEG file after reading\n" \
    " [-n | --name ].........: ignore changes unless filename matches\n" \
    " [-e | --existing ].....: serve the existing *.jpg files from the specified directory\n" \
    " ---------------------------------------------------------------\n");
#endif
}

/* the single writer thread */
void *worker_thread(void *arg)
{
    struct timeval timestamp;

    DBG("#work_thread# start ...\n");

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);

    lastTime = getTimeMillis();

    while(!pglobal->stop) {
        uint64_t t_delay_send_start = getTimeMillis();

        /* copy frame from file to global buffer */
        pthread_mutex_lock(&pglobal->in[plugin_number].db);

        DBG("#work_thread# looping ...\n");

        /* allocate memory for frame */
        if(pglobal->in[plugin_number].buf != NULL)
            free(pglobal->in[plugin_number].buf);

        getEncodedBuf(&pglobal->in[plugin_number].buf, &pglobal->in[plugin_number].size);

        if(pglobal->in[plugin_number].buf == NULL) {
            fprintf(stderr, "captureScreen buff NULL\n");
            break;
        }

        gettimeofday(&timestamp, NULL);
        pglobal->in[plugin_number].timestamp = timestamp;
        DBG("new frame copied (size: %d)\n", pglobal->in[plugin_number].size);
        /* signal fresh_frame */
        pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);
        pthread_mutex_unlock(&pglobal->in[plugin_number].db);

        if(delay != 0)
            usleep(1000 * delay);

        uint64_t t_delay_send_end = getTimeMillis();

        printf("#performance# t_delay_send %" PRIu64 "\n" , t_delay_send_end - t_delay_send_start);

        fps();
    }


    DBG("leaving input thread, calling cleanup function now\n");
    /* call cleanup handler, signal with the parameter */
    pthread_cleanup_pop(1);

    return NULL;
}

void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;

    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }

    first_run = 0;
    DBG("cleaning up resources allocated by input thread\n");

    if(pglobal->in[plugin_number].buf != NULL) free(pglobal->in[plugin_number].buf);
}
