/* -*- tab-width: 4; indent-tabs-mode: nil -*- */
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <ctype.h>
#include <libgen.h>

#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/ipc.h>
#include <sys/msg.h>

#include <sys/ioctl.h>
#include "pt1_ioctl.h"

#include "config.h"
#include "decoder.h"
#include "recpt1core.h"
#include "recpt1.h"
#include "mkpath.h"

#include "tssplitter_lite.h"

/* maximum write length at once */
#define SIZE_CHANK 1316

/* ipc message size */
#define MSGSZ     255

/* globals */
extern boolean f_exit;


/* will be ipc message receive thread */
void *
mq_recv(void *t)
{
    thread_data *tdata = (thread_data *)t;
    message_buf rbuf;
    char channel[16];
    int recsec = 0, time_to_add = 0;

    while(1) {
        if(msgrcv(tdata->msqid, &rbuf, MSGSZ, 1, 0) < 0) {
            return NULL;
        }

        sscanf(rbuf.mtext, "ch=%s t=%d e=%d", channel, &recsec, &time_to_add);

        if(strcmp(channel, tdata->table->parm_freq)) {
            int current_type = tdata->table->type;
            ISDB_T_FREQ_CONV_TABLE *table = searchrecoff(channel);
            if (table == NULL) {
                fprintf(stderr, "Invalid Channel: %s\n", channel);
                goto CHECK_TIME_TO_ADD;
            }
            tdata->table = table;

            /* stop stream */
            ioctl(tdata->tfd, STOP_REC, 0);

            /* wait for remainder */
            while(tdata->queue->num_used > 0) {
                usleep(10000);
            }

            if (tdata->table->type != current_type) {
                /* re-open device */
                if(close_tuner(tdata) != 0)
                    return NULL;

                tune(channel, tdata, NULL);
            } else {
                /* SET_CHANNEL only */
                const FREQUENCY freq = {
                  .frequencyno = tdata->table->set_freq,
                  .slot = tdata->table->add_freq,
                };
                if(ioctl(tdata->tfd, SET_CHANNEL, &freq) < 0) {
                    fprintf(stderr, "Cannot tune to the specified channel\n");
                    goto CHECK_TIME_TO_ADD;
                }
                calc_cn(tdata->tfd, tdata->table->type, FALSE);
            }
            /* restart recording */
            if(ioctl(tdata->tfd, START_REC, 0) < 0) {
                fprintf(stderr, "Tuner cannot start recording\n");
                return NULL;
            }
        }

CHECK_TIME_TO_ADD:
        if(time_to_add) {
            tdata->recsec += time_to_add;
            fprintf(stderr, "Extended %d sec\n", time_to_add);
        }

        if(recsec) {
            time_t cur_time;
            time(&cur_time);
            if(cur_time - tdata->start_time > recsec) {
                f_exit = TRUE;
            }
            else {
                tdata->recsec = recsec;
                fprintf(stderr, "Total recording time = %d sec\n", recsec);
            }
        }

        if(f_exit)
            return NULL;
    }
}


QUEUE_T *
create_queue(size_t size)
{
    QUEUE_T *p_queue;
    int memsize = sizeof(QUEUE_T) + size * sizeof(BUFSZ*);

    p_queue = (QUEUE_T*)calloc(memsize, sizeof(char));

    if(p_queue != NULL) {
        p_queue->size = size;
        p_queue->num_avail = size;
        p_queue->num_used = 0;
        pthread_mutex_init(&p_queue->mutex, NULL);
        pthread_cond_init(&p_queue->cond_avail, NULL);
        pthread_cond_init(&p_queue->cond_used, NULL);
    }

    return p_queue;
}

void
destroy_queue(QUEUE_T *p_queue)
{
    if(!p_queue)
        return;

    pthread_mutex_destroy(&p_queue->mutex);
    pthread_cond_destroy(&p_queue->cond_avail);
    pthread_cond_destroy(&p_queue->cond_used);
    free(p_queue);
}

/* enqueue data. this function will block if queue is full. */
void
enqueue(QUEUE_T *p_queue, BUFSZ *data)
{
    struct timeval now;
    struct timespec spec;
    int retry_count = 0;

    pthread_mutex_lock(&p_queue->mutex);
    /* entered critical section */

    /* wait while queue is full */
    while(p_queue->num_avail == 0) {

        gettimeofday(&now, NULL);
        spec.tv_sec = now.tv_sec + 1;
        spec.tv_nsec = now.tv_usec * 1000;

        pthread_cond_timedwait(&p_queue->cond_avail,
                               &p_queue->mutex, &spec);
        retry_count++;
        if(retry_count > 60) {
            f_exit = TRUE;
        }
        if(f_exit) {
            pthread_mutex_unlock(&p_queue->mutex);
            return;
        }
    }

    p_queue->buffer[p_queue->in] = data;

    /* move position marker for input to next position */
    p_queue->in++;
    p_queue->in %= p_queue->size;

    /* update counters */
    p_queue->num_avail--;
    p_queue->num_used++;

    /* leaving critical section */
    pthread_mutex_unlock(&p_queue->mutex);
    pthread_cond_signal(&p_queue->cond_used);
}

/* dequeue data. this function will block if queue is empty. */
BUFSZ *
dequeue(QUEUE_T *p_queue)
{
    struct timeval now;
    struct timespec spec;
    BUFSZ *buffer;
    int retry_count = 0;

    pthread_mutex_lock(&p_queue->mutex);
    /* entered the critical section*/

    /* wait while queue is empty */
    while(p_queue->num_used == 0) {

        gettimeofday(&now, NULL);
        spec.tv_sec = now.tv_sec + 1;
        spec.tv_nsec = now.tv_usec * 1000;

        pthread_cond_timedwait(&p_queue->cond_used,
                               &p_queue->mutex, &spec);
        retry_count++;
        if(retry_count > 60) {
            f_exit = TRUE;
        }
        if(f_exit) {
            pthread_mutex_unlock(&p_queue->mutex);
            return NULL;
        }
    }

    /* take buffer address */
    buffer = p_queue->buffer[p_queue->out];

    /* move position marker for output to next position */
    p_queue->out++;
    p_queue->out %= p_queue->size;

    /* update counters */
    p_queue->num_avail++;
    p_queue->num_used--;

    /* leaving the critical section */
    pthread_mutex_unlock(&p_queue->mutex);
    pthread_cond_signal(&p_queue->cond_avail);

    return buffer;
}

/* this function will be reader thread */
void *
reader_func(void *p)
{
    thread_data *tdata = (thread_data *)p;
    QUEUE_T *p_queue = tdata->queue;
    decoder *dec = tdata->decoder;
    splitter *splitter = tdata->splitter;
    int wfd = tdata->wfd;
    boolean use_b25 = dec ? TRUE : FALSE;
    boolean use_udp = tdata->sock_data ? TRUE : FALSE;
    boolean fileless = FALSE;
    boolean use_splitter = splitter ? TRUE : FALSE;
    int sfd = -1;
    pthread_t signal_thread = tdata->signal_thread;
    struct sockaddr_in *addr = NULL;
    BUFSZ *qbuf;
    static splitbuf_t splitbuf;
    ARIB_STD_B25_BUFFER sbuf, dbuf, buf;
    int code;
    int split_select_finish = TSS_ERROR;

    buf.size = 0;
    buf.data = NULL;
    splitbuf.buffer_size = 0;
    splitbuf.buffer = NULL;

    if(wfd == -1)
        fileless = TRUE;

    if(use_udp) {
        sfd = tdata->sock_data->sfd;
        addr = &tdata->sock_data->addr;
    }

    while(1) {
        ssize_t wc = 0;
        int file_err = 0;
        qbuf = dequeue(p_queue);
        /* no entry in the queue */
        if(qbuf == NULL) {
            break;
        }

        sbuf.data = qbuf->buffer;
        sbuf.size = qbuf->size;

        buf = sbuf; /* default */

        if(use_b25) {
            code = b25_decode(dec, &sbuf, &dbuf);
            if(code < 0) {
                fprintf(stderr, "b25_decode failed (code=%d). fall back to encrypted recording.\n", code);
                use_b25 = FALSE;
            }
            else
                buf = dbuf;
        }


        if(use_splitter) {
            splitbuf.buffer_filled = 0;

            /* allocate split buffer */
            if(splitbuf.buffer_size < buf.size && buf.size > 0) {
                splitbuf.buffer = realloc(splitbuf.buffer, buf.size);
                if(splitbuf.buffer == NULL) {
                    fprintf(stderr, "split buffer allocation failed\n");
                    use_splitter = FALSE;
                    goto fin;
                }
            }

            while(buf.size) {
                /* $BJ,N%BP>](BPID$B$NCj=P(B */
                if(split_select_finish != TSS_SUCCESS) {
                    split_select_finish = split_select(splitter, &buf);
                    if(split_select_finish == TSS_NULL) {
                        /* malloc$B%(%i!<H/@8(B */
                        fprintf(stderr, "split_select malloc failed\n");
                        use_splitter = FALSE;
                        goto fin;
                    }
                    else if(split_select_finish != TSS_SUCCESS) {
                        /* $BJ,N%BP>](BPID$B$,40A4$KCj=P$G$-$k$^$G=PNO$7$J$$(B
                         * 1$BICDxEYM>M5$r8+$k$H$$$$$+$b(B
                         */
                        time_t cur_time;
                        time(&cur_time);
                        if(cur_time - tdata->start_time > 4) {
                            use_splitter = FALSE;
                            goto fin;
                        }
                        break;
                    }
                }

                /* $BJ,N%BP>]0J30$r$U$k$$Mn$H$9(B */
                code = split_ts(splitter, &buf, &splitbuf);
                if(code == TSS_NULL) {
                    fprintf(stderr, "PMT reading..\n");
                }
                else if(code != TSS_SUCCESS) {
                    fprintf(stderr, "split_ts failed\n");
                    break;
                }

                break;
            } /* while */

            buf.size = splitbuf.buffer_filled;
            buf.data = splitbuf.buffer;
        fin:
            ;
        } /* if */


        if(!fileless) {
            /* write data to output file */
            int size_remain = buf.size;
            int offset = 0;

            while(size_remain > 0) {
                int ws = size_remain < SIZE_CHANK ? size_remain : SIZE_CHANK;

                wc = write(wfd, buf.data + offset, ws);
                if(wc < 0) {
                    perror("write");
                    file_err = 1;
                    pthread_kill(signal_thread,
                                 errno == EPIPE ? SIGPIPE : SIGUSR2);
                    break;
                }
                size_remain -= wc;
                offset += wc;
            }
        }

        if(use_udp && sfd != -1) {
            /* write data to socket */
            int size_remain = buf.size;
            int offset = 0;
            while(size_remain > 0) {
                int ws = size_remain < SIZE_CHANK ? size_remain : SIZE_CHANK;
                wc = write(sfd, buf.data + offset, ws);
                if(wc < 0) {
                    if(errno == EPIPE)
                        pthread_kill(signal_thread, SIGPIPE);
                    break;
                }
                size_remain -= wc;
                offset += wc;
            }
        }

        free(qbuf);
        qbuf = NULL;

        /* normal exit */
        if((f_exit && !p_queue->num_used) || file_err) {

            buf = sbuf; /* default */

            if(use_b25) {
                code = b25_finish(dec, &sbuf, &dbuf);
                if(code < 0)
                    fprintf(stderr, "b25_finish failed\n");
                else
                    buf = dbuf;
            }

            if(use_splitter) {
                /* $BJ,N%BP>]0J30$r$U$k$$Mn$H$9(B */
                code = split_ts(splitter, &buf, &splitbuf);
                if(code == TSS_NULL) {
                    split_select_finish = TSS_ERROR;
                    fprintf(stderr, "PMT reading..\n");
                }
                else if(code != TSS_SUCCESS) {
                    fprintf(stderr, "split_ts failed\n");
                    break;
                }

                buf.data = splitbuf.buffer;
                buf.size = splitbuf.buffer_size;
            }

            if(!fileless && !file_err) {
                wc = write(wfd, buf.data, buf.size);
                if(wc < 0) {
                    perror("write");
                    file_err = 1;
                    pthread_kill(signal_thread,
                                 errno == EPIPE ? SIGPIPE : SIGUSR2);
                }
            }

            if(use_udp && sfd != -1) {
                wc = write(sfd, buf.data, buf.size);
                if(wc < 0) {
                    if(errno == EPIPE)
                        pthread_kill(signal_thread, SIGPIPE);
                }
            }

            if(use_splitter) {
                free(splitbuf.buffer);
                splitbuf.buffer = NULL;
                splitbuf.buffer_size = 0;
            }

            break;
        }
    }

    time_t cur_time;
    time(&cur_time);
    fprintf(stderr, "Recorded %dsec\n",
            (int)(cur_time - tdata->start_time));

    return NULL;
}

void
show_usage(char *cmd)
{
#ifdef HAVE_LIBARIB25
    fprintf(stderr, "Usage: \n%s [--b25 [--round N] [--strip] [--EMM]] [--udp [--addr hostname --port portnumber]] [--device devicefile] [--lnb voltage] [--sid SID1,SID2] channel rectime destfile\n", cmd);
#else
    fprintf(stderr, "Usage: \n%s [--strip] [--EMM]] [--udp [--addr hostname --port portnumber]] [--device devicefile] [--lnb voltage] [--sid SID1,SID2] channel rectime destfile\n", cmd);
#endif
    fprintf(stderr, "\n");
    fprintf(stderr, "Remarks:\n");
    fprintf(stderr, "if rectime  is '-', records indefinitely.\n");
    fprintf(stderr, "if destfile is '-', stdout is used for output.\n");
}

void
show_options(void)
{
    fprintf(stderr, "Options:\n");
#ifdef HAVE_LIBARIB25
    fprintf(stderr, "--b25:               Decrypt using BCAS card\n");
    fprintf(stderr, "  --round N:         Specify round number\n");
    fprintf(stderr, "  --strip:           Strip null stream\n");
    fprintf(stderr, "  --EMM:             Instruct EMM operation\n");
#endif
    fprintf(stderr, "--udp:               Turn on udp broadcasting\n");
    fprintf(stderr, "  --addr hostname:   Hostname or address to connect\n");
    fprintf(stderr, "  --port portnumber: Port number to connect\n");
    fprintf(stderr, "--device devicefile: Specify devicefile to use\n");
    fprintf(stderr, "--lnb voltage:       Specify LNB voltage (0, 11, 15)\n");
    fprintf(stderr, "--sid SID1,SID2,...: Specify SID number in CSV format (101,102,...)\n");
    fprintf(stderr, "--help:              Show this help\n");
    fprintf(stderr, "--version:           Show version\n");
    fprintf(stderr, "--list:              Show channel list\n");
}

void
cleanup(thread_data *tdata)
{
    /* stop recording */
    ioctl(tdata->tfd, STOP_REC, 0);

    f_exit = TRUE;

    pthread_cond_signal(&tdata->queue->cond_avail);
    pthread_cond_signal(&tdata->queue->cond_used);
}

/* will be signal handler thread */
void *
process_signals(void *t)
{
    sigset_t waitset;
    int sig;
    thread_data *tdata = (thread_data *)t;

    sigemptyset(&waitset);
    sigaddset(&waitset, SIGPIPE);
    sigaddset(&waitset, SIGINT);
    sigaddset(&waitset, SIGTERM);
    sigaddset(&waitset, SIGUSR1);
    sigaddset(&waitset, SIGUSR2);

    sigwait(&waitset, &sig);

    switch(sig) {
    case SIGPIPE:
        fprintf(stderr, "\nSIGPIPE received. cleaning up...\n");
        cleanup(tdata);
        break;
    case SIGINT:
        fprintf(stderr, "\nSIGINT received. cleaning up...\n");
        cleanup(tdata);
        break;
    case SIGTERM:
        fprintf(stderr, "\nSIGTERM received. cleaning up...\n");
        cleanup(tdata);
        break;
    case SIGUSR1: /* normal exit*/
        cleanup(tdata);
        break;
    case SIGUSR2: /* error */
        fprintf(stderr, "Detected an error. cleaning up...\n");
        cleanup(tdata);
        break;
    }

    return NULL; /* dummy */
}

void
init_signal_handlers(pthread_t *signal_thread, thread_data *tdata)
{
    sigset_t blockset;

    sigemptyset(&blockset);
    sigaddset(&blockset, SIGPIPE);
    sigaddset(&blockset, SIGINT);
    sigaddset(&blockset, SIGTERM);
    sigaddset(&blockset, SIGUSR1);
    sigaddset(&blockset, SIGUSR2);

    if(pthread_sigmask(SIG_BLOCK, &blockset, NULL))
        fprintf(stderr, "pthread_sigmask() failed.\n");

    pthread_create(signal_thread, NULL, process_signals, tdata);
}

int
main(int argc, char **argv)
{
    time_t cur_time;
    pthread_t signal_thread;
    pthread_t reader_thread;
    pthread_t ipc_thread;
    QUEUE_T *p_queue = create_queue(MAX_QUEUE);
    BUFSZ   *bufptr;
    decoder *decoder = NULL;
    splitter *splitter = NULL;
    static thread_data tdata;
    decoder_options dopt = {
        4,  /* round */
        0,  /* strip */
        0   /* emm */
    };
    tdata.dopt = &dopt;
    tdata.lnb = 0;

    int result;
    int option_index;
    struct option long_options[] = {
#ifdef HAVE_LIBARIB25
        { "b25",       0, NULL, 'b'},
        { "B25",       0, NULL, 'b'},
        { "round",     1, NULL, 'r'},
        { "strip",     0, NULL, 's'},
        { "emm",       0, NULL, 'm'},
        { "EMM",       0, NULL, 'm'},
#endif
        { "LNB",       1, NULL, 'n'},
        { "lnb",       1, NULL, 'n'},
        { "udp",       0, NULL, 'u'},
        { "addr",      1, NULL, 'a'},
        { "port",      1, NULL, 'p'},
        { "device",    1, NULL, 'd'},
        { "help",      0, NULL, 'h'},
        { "version",   0, NULL, 'v'},
        { "list",      0, NULL, 'l'},
        { "sid",       1, NULL, 'i'},
        {0, 0, NULL, 0} /* terminate */
    };

    boolean use_b25 = FALSE;
    boolean use_udp = FALSE;
    boolean fileless = FALSE;
    boolean use_stdout = FALSE;
    boolean use_splitter = FALSE;
    char *host_to = NULL;
    int port_to = 1234;
    sock_data *sockdata = NULL;
    char *device = NULL;
    int val;
    char *voltage[] = {"0V", "11V", "15V"};
    char *sid_list = NULL;

    while((result = getopt_long(argc, argv, "br:smn:ua:p:d:hvli:",
                                long_options, &option_index)) != -1) {
        switch(result) {
        case 'b':
            use_b25 = TRUE;
            fprintf(stderr, "using B25...\n");
            break;
        case 's':
            dopt.strip = TRUE;
            fprintf(stderr, "enable B25 strip\n");
            break;
        case 'm':
            dopt.emm = TRUE;
            fprintf(stderr, "enable B25 emm processing\n");
            break;
        case 'u':
            use_udp = TRUE;
            host_to = "localhost";
            fprintf(stderr, "enable UDP broadcasting\n");
            break;
        case 'h':
            fprintf(stderr, "\n");
            show_usage(argv[0]);
            fprintf(stderr, "\n");
            show_options();
            fprintf(stderr, "\n");
            show_channels();
            fprintf(stderr, "\n");
            exit(0);
            break;
        case 'v':
            fprintf(stderr, "%s %s\n", argv[0], version);
            fprintf(stderr, "recorder command for PT1/2 digital tuner.\n");
            exit(0);
            break;
        case 'l':
            show_channels();
            exit(0);
            break;
        /* following options require argument */
        case 'n':
            val = atoi(optarg);
            switch(val) {
            case 11:
                tdata.lnb = 1;
                break;
            case 15:
                tdata.lnb = 2;
                break;
            default:
                tdata.lnb = 0;
                break;
            }
            fprintf(stderr, "LNB = %s\n", voltage[tdata.lnb]);
            break;
        case 'r':
            dopt.round = atoi(optarg);
            fprintf(stderr, "set round %d\n", dopt.round);
            break;
        case 'a':
            use_udp = TRUE;
            host_to = optarg;
            fprintf(stderr, "UDP destination address: %s\n", host_to);
            break;
        case 'p':
            port_to = atoi(optarg);
            fprintf(stderr, "UDP port: %d\n", port_to);
            break;
        case 'd':
            device = optarg;
            fprintf(stderr, "using device: %s\n", device);
            break;
        case 'i':
            use_splitter = TRUE;
            sid_list = optarg;
            break;
        }
    }

    if(argc - optind < 3) {
        if(argc - optind == 2 && use_udp) {
            fprintf(stderr, "Fileless UDP broadcasting\n");
            fileless = TRUE;
            tdata.wfd = -1;
        }
        else {
            fprintf(stderr, "Arguments are necessary!\n");
            fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
            return 1;
        }
    }

    fprintf(stderr, "pid = %d\n", getpid());

    /* tune */
    if(tune(argv[optind], &tdata, device) != 0)
        return 1;

    /* set recsec */
    if(parse_time(argv[optind + 1], &tdata.recsec) != 0) // no other thread --yaz
        return 1;

    if(tdata.recsec == -1)
        tdata.indefinite = TRUE;

    /* open output file */
    char *destfile = argv[optind + 2];
    if(destfile && !strcmp("-", destfile)) {
        use_stdout = TRUE;
        tdata.wfd = 1; /* stdout */
    }
    else {
        if(!fileless) {
            int status;
            char *path = strdup(argv[optind + 2]);
            char *dir = dirname(path);
            status = mkpath(dir, 0777);
            if(status == -1)
                perror("mkpath");
            free(path);

            tdata.wfd = open(argv[optind + 2], (O_RDWR | O_CREAT | O_TRUNC), 0666);
            if(tdata.wfd < 0) {
                fprintf(stderr, "Cannot open output file: %s\n",
                        argv[optind + 2]);
                return 1;
            }
        }
    }

    /* initialize decoder */
    if(use_b25) {
        decoder = b25_startup(&dopt);
        if(!decoder) {
            fprintf(stderr, "Cannot start b25 decoder\n");
            fprintf(stderr, "Fall back to encrypted recording\n");
            use_b25 = FALSE;
        }
    }
    /* initialize splitter */
    if(use_splitter) {
        splitter = split_startup(sid_list);
        if(splitter->sid_list == NULL) {
            fprintf(stderr, "Cannot start TS splitter\n");
            return 1;
        }
    }

    /* initialize udp connection */
    if(use_udp) {
      sockdata = calloc(1, sizeof(sock_data));
      struct in_addr ia;
      ia.s_addr = inet_addr(host_to);
      if(ia.s_addr == INADDR_NONE) {
            struct hostent *hoste = gethostbyname(host_to);
            if(!hoste) {
                perror("gethostbyname");
                return 1;
            }
            ia.s_addr = *(in_addr_t*) (hoste->h_addr_list[0]);
        }
        if((sockdata->sfd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
            perror("socket");
            return 1;
        }

        sockdata->addr.sin_family = AF_INET;
        sockdata->addr.sin_port = htons (port_to);
        sockdata->addr.sin_addr.s_addr = ia.s_addr;

        if(connect(sockdata->sfd, (struct sockaddr *)&sockdata->addr,
                   sizeof(sockdata->addr)) < 0) {
            perror("connect");
            return 1;
        }
    }

    /* prepare thread data */
    tdata.queue = p_queue;
    tdata.decoder = decoder;
    tdata.splitter = splitter;
    tdata.sock_data = sockdata;
    tdata.tune_persistent = FALSE;

    /* spawn signal handler thread */
    init_signal_handlers(&signal_thread, &tdata);

    /* spawn reader thread */
    tdata.signal_thread = signal_thread;
    pthread_create(&reader_thread, NULL, reader_func, &tdata);

    /* spawn ipc thread */
    key_t key;
    key = (key_t)getpid();

    if ((tdata.msqid = msgget(key, IPC_CREAT | 0666)) < 0) {
        perror("msgget");
    }
    pthread_create(&ipc_thread, NULL, mq_recv, &tdata);

    /* start recording */
    if(ioctl(tdata.tfd, START_REC, 0) < 0) {
        fprintf(stderr, "Tuner cannot start recording\n");
        return 1;
    }

    fprintf(stderr, "\nRecording...\n");

    time(&tdata.start_time);

    /* read from tuner */
    while(1) {
        if(f_exit)
            break;

        time(&cur_time);
        bufptr = malloc(sizeof(BUFSZ));
        if(!bufptr) {
            f_exit = TRUE;
            break;
        }
        bufptr->size = read(tdata.tfd, bufptr->buffer, MAX_READ_SIZE);
        if(bufptr->size <= 0) {
            if((cur_time - tdata.start_time) >= tdata.recsec && !tdata.indefinite) {
                f_exit = TRUE;
                enqueue(p_queue, NULL);
                break;
            }
            else {
                free(bufptr);
                continue;
            }
        }
        enqueue(p_queue, bufptr);

        /* stop recording */
        time(&cur_time);
        if((cur_time - tdata.start_time) >= tdata.recsec && !tdata.indefinite) {
            ioctl(tdata.tfd, STOP_REC, 0);
            /* read remaining data */
            while(1) {
                bufptr = malloc(sizeof(BUFSZ));
                if(!bufptr) {
                    f_exit = TRUE;
                    break;
                }
                bufptr->size = read(tdata.tfd, bufptr->buffer, MAX_READ_SIZE);
                if(bufptr->size <= 0) {
                    f_exit = TRUE;
                    enqueue(p_queue, NULL);
                    break;
                }
                enqueue(p_queue, bufptr);
            }
            break;
        }
    }

    /* delete message queue*/
    msgctl(tdata.msqid, IPC_RMID, NULL);

    pthread_kill(signal_thread, SIGUSR1);

    /* wait for threads */
    pthread_join(reader_thread, NULL);
    pthread_join(signal_thread, NULL);
    pthread_join(ipc_thread, NULL);

    /* close tuner */
    if(close_tuner(&tdata) != 0)
        return 1;

    /* release queue */
    destroy_queue(p_queue);

    /* close output file */
    if(!use_stdout)
        close(tdata.wfd);

    /* free socket data */
    if(use_udp) {
        close(sockdata->sfd);
        free(sockdata);
    }

    /* release decoder */
    if(use_b25) {
        b25_shutdown(decoder);
    }
    if(use_splitter) {
        split_shutdown(splitter);
    }

    return 0;
}
