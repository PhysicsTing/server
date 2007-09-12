/* Copyright (C) 2006,2007 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/*
  WL#3071 Maria checkpoint
  First version written by Guilhem Bichot on 2006-04-27.
*/

/* Here is the implementation of this module */

/**
   @todo RECOVERY BUG this is unreviewed code, but used in safe conditions:
   ha_maria takes a checkpoint at end of recovery and one at clean shutdown,
   that's all. So there never are open tables, dirty pages, transactions.
*/
/*
  Summary:
  checkpoints are done either by a background thread (checkpoint every Nth
  second) or by a client.
  In ha_maria, it's not made available to clients, and will soon be done by a
  background thread (periodically taking checkpoints and flushing dirty
  pages).
*/

#include "maria_def.h"
#include "ma_pagecache.h"
#include "trnman.h"
#include "ma_blockrec.h"
#include "ma_checkpoint.h"
#include "ma_loghandler_lsn.h"


/*
  Checkpoints currently happen only at ha_maria's startup (after recovery) and
  at shutdown, always when there is no open tables.
  Background page flushing is not used.
  So, needed pagecache functions for doing this flushing are not yet pushed.
*/
#define flush_pagecache_blocks_with_filter(A,B,C,D,E) (((int)D) * 0)
/**
   filter has to return 0, 1 or 2: 0 means "don't flush this page", 1 means
   "flush it", 2 means "don't flush this page and following pages".
   Will move to ma_pagecache.h
*/
typedef int (*PAGECACHE_FILTER)(enum pagecache_page_type type,
                                pgcache_page_no_t page,
                                LSN rec_lsn, void *arg);


/** @brief type of checkpoint currently running */
static CHECKPOINT_LEVEL checkpoint_in_progress= CHECKPOINT_NONE;
/** @brief protects checkpoint_in_progress */
static pthread_mutex_t LOCK_checkpoint;
/** @brief for killing the background checkpoint thread */
static pthread_cond_t  COND_checkpoint;
/** @brief if checkpoint module was inited or not */
static my_bool checkpoint_inited= FALSE;
/** @brief 'kill' flag for the background checkpoint thread */
static int checkpoint_thread_die;
/* is ulong like pagecache->blocks_changed */
static ulong pages_to_flush_before_next_checkpoint;
static PAGECACHE_FILE *dfiles, /**< data files to flush in background */
  *dfiles_end; /**< list of data files ends here */
static PAGECACHE_FILE *kfiles, /**< index files to flush in background */
  *kfiles_end; /**< list of index files ends here */
/* those two statistics below could serve in SHOW GLOBAL STATUS */
static uint checkpoints_total= 0, /**< all checkpoint requests made */
  checkpoints_ok_total= 0; /**< all checkpoints which succeeded */

struct st_filter_param
{
  my_bool is_data_file; /**< is the file about data or index */
  LSN up_to_lsn; /**< only pages with rec_lsn < this LSN */
  ulong pages_covered_by_bitmap; /**< to know which page is a bitmap page */
  uint max_pages; /**< stop after flushing this number pages */
}; /**< information to determine which dirty pages should be flushed */

static int filter_flush_data_file_medium(enum pagecache_page_type type,
                                         pgcache_page_no_t page,
                                         LSN rec_lsn, void *arg);
static int filter_flush_data_file_full(enum pagecache_page_type type,
                                       pgcache_page_no_t page,
                                       LSN rec_lsn, void *arg);
static int filter_flush_data_file_indirect(enum pagecache_page_type type,
                                           pgcache_page_no_t page,
                                           LSN rec_lsn, void *arg);
static int filter_flush_data_file_evenly(enum pagecache_page_type type,
                                         pgcache_page_no_t pageno,
                                         LSN rec_lsn, void *arg);
static int really_execute_checkpoint();
pthread_handler_t ma_checkpoint_background(void *arg);
static int collect_tables();

/**
   @brief Does a checkpoint

   @param  level               what level of checkpoint to do
   @param  no_wait             if another checkpoint of same or stronger level
                               is already running, consider our job done

   @note In ha_maria, there can never be two threads trying a checkpoint at
   the same time.

   @return Operation status
    @retval 0 ok
    @retval !=0 error
*/

int ma_checkpoint_execute(CHECKPOINT_LEVEL level, my_bool no_wait)
{
  int result= 0;
  DBUG_ENTER("ma_checkpoint_execute");

  DBUG_ASSERT(checkpoint_inited);
  DBUG_ASSERT(level > CHECKPOINT_NONE);

  /* look for already running checkpoints */
  pthread_mutex_lock(&LOCK_checkpoint);
  while (checkpoint_in_progress != CHECKPOINT_NONE)
  {
    if (no_wait && (checkpoint_in_progress >= level))
    {
      /*
        If we are the checkpoint background thread, we don't wait (it's
        smarter to flush pages instead of waiting here while the other thread
        finishes its checkpoint).
      */
      pthread_mutex_unlock(&LOCK_checkpoint);
      goto end;
    }
    pthread_cond_wait(&COND_checkpoint, &LOCK_checkpoint);
  }

  checkpoint_in_progress= level;
  pthread_mutex_unlock(&LOCK_checkpoint);
  /* from then on, we are sure to be and stay the only checkpointer */

  result= really_execute_checkpoint();
  pthread_cond_broadcast(&COND_checkpoint);
end:
  DBUG_RETURN(result);
}


/**
   @brief Does a checkpoint, really; expects no other checkpoints
   running.

   Checkpoint level requested is read from checkpoint_in_progress.

   @return Operation status
    @retval 0   ok
    @retval !=0 error
*/

static int really_execute_checkpoint()
{
  uint i, error= 0;
  /** @brief checkpoint_start_log_horizon will be stored there */
  char *ptr;
  LEX_STRING record_pieces[4]; /**< only malloc-ed pieces */
  LSN min_page_rec_lsn, min_trn_rec_lsn, min_first_undo_lsn;
  TRANSLOG_ADDRESS checkpoint_start_log_horizon;
  uchar checkpoint_start_log_horizon_char[LSN_STORE_SIZE];
  DBUG_ENTER("really_execute_checkpoint");
  bzero(&record_pieces, sizeof(record_pieces));

  /*
    STEP 1: record current end-of-log position using log's lock. It is
    critical for the correctness of Checkpoint (related to memory visibility
    rules, the log's lock is a mutex).
    "Horizon" is a lower bound of the LSN of the next log record.
  */
  /**
     @todo RECOVERY BUG
     this is an horizon, but it is used as a LSN (REDO phase may start from
     there! probably log handler would refuse to read then;
     Sanja proposed to make a loghandler's function which finds the LSN after
     this horizon.
  */
  checkpoint_start_log_horizon= translog_get_horizon();
#define LSN_IN_HEX(L) (ulong)LSN_FILE_NO(L),(ulong)LSN_OFFSET(L)
  DBUG_PRINT("info",("checkpoint_start_log_horizon (%lu,0x%lx)",
                     LSN_IN_HEX(checkpoint_start_log_horizon)));
  lsn_store(checkpoint_start_log_horizon_char, checkpoint_start_log_horizon);


  /*
    STEP 2: fetch information about transactions.
    We must fetch transactions before dirty pages. Indeed, a transaction
    first sets its rec_lsn then sets the page's rec_lsn then sets its rec_lsn
    to 0. If we fetched pages first, we may see no dirty page yet, then we
    fetch transactions but the transaction has already reset its rec_lsn to 0
    so we miss rec_lsn again.
    For a similar reason (over-allocated bitmap pages) we have to fetch
    transactions before flushing bitmap pages.

    min_trn_rec_lsn will serve to lower the starting point of the REDO phase
    (down from checkpoint_start_log_horizon).
 */
  if (unlikely(trnman_collect_transactions(&record_pieces[0],
                                           &record_pieces[1],
                                           &min_trn_rec_lsn,
                                           &min_first_undo_lsn)))
    goto err;


  /* STEP 3: fetch information about table files */
  if (unlikely(collect_tables(&record_pieces[2])))
    goto err;


  /* STEP 4: fetch information about dirty pages */
  /*
    It's better to do it _after_ having flushed some data pages (which
    collect_tables() may have done), because those are now non-dirty and so we
    have a more up-to-date dirty pages list to put into the checkpoint record,
    and thus we will have less work at Recovery.
  */
  /* Using default pagecache for now */
  if (unlikely(pagecache_collect_changed_blocks_with_lsn(maria_pagecache,
                                                         &record_pieces[3],
                                                         &min_page_rec_lsn)))
    goto err;


  /* LAST STEP: now write the checkpoint log record */
  {
    LSN lsn;
    uint total_rec_length;
    /*
      the log handler is allowed to modify "str" and "length" (but not "*str")
      of its argument, so we must not pass it record_pieces directly,
      otherwise we would later not know what memory pieces to my_free().
    */
    LEX_STRING log_array[TRANSLOG_INTERNAL_PARTS + 5];
    log_array[TRANSLOG_INTERNAL_PARTS + 0].str=
      checkpoint_start_log_horizon_char;
    log_array[TRANSLOG_INTERNAL_PARTS + 0].length= total_rec_length=
      sizeof(checkpoint_start_log_horizon_char);
    for (i= 0; i < (sizeof(record_pieces)/sizeof(record_pieces[0])); i++)
    {
      log_array[TRANSLOG_INTERNAL_PARTS + 1 + i]= record_pieces[i];
      total_rec_length+= record_pieces[i].length;
    }

    if (unlikely(translog_write_record(&lsn, LOGREC_CHECKPOINT,
                                       &dummy_transaction_object, NULL,
                                       total_rec_length,
                                       sizeof(log_array)/sizeof(log_array[0]),
                                       log_array, NULL) ||
                 translog_flush(lsn)))
      goto err;

    translog_lock();
    /*
      This cannot be done as a inwrite_rec_hook of LOGREC_CHECKPOINT, because
      such hook would be called before translog_flush (and we must be sure
      that log was flushed before we write to the control file).
    */
    if (unlikely(ma_control_file_write_and_force(lsn, FILENO_IMPOSSIBLE,
                                                 CONTROL_FILE_UPDATE_ONLY_LSN)))
    {
      translog_unlock();
      goto err;
    }
    translog_unlock();
  }

  /*
    Note that we should not alter memory structures until we have successfully
    written the checkpoint record and control file.
  */
  /* checkpoint succeeded */
  ptr= record_pieces[3].str;
  pages_to_flush_before_next_checkpoint= uint4korr(ptr);
  DBUG_PRINT("info",("%u pages to flush before next checkpoint",
                     (uint)pages_to_flush_before_next_checkpoint));

  /* compute log's low-water mark */
  TRANSLOG_ADDRESS log_low_water_mark= min_page_rec_lsn;
  set_if_smaller(log_low_water_mark, min_trn_rec_lsn);
  set_if_smaller(log_low_water_mark, min_first_undo_lsn);
  set_if_smaller(log_low_water_mark, checkpoint_start_log_horizon);
  /**
     Now purge unneeded logs.
     As some systems have an unreliable fsync (drive lying), we could try to
     be robust against that: remember a few previous checkpoints in the
     control file, and not purge logs immediately... Think about it.
  */
#if 0 /* purging/keeping will be an option */
  if (translog_purge(log_low_water_mark))
    fprintf(stderr, "Maria engine: log purge failed\n"); /* not deadly */
#endif

  goto end;

err:
  error= 1;
  fprintf(stderr, "Maria engine: checkpoint failed\n"); /* TODO: improve ;) */
  /* we were possibly not able to determine what pages to flush */
  pages_to_flush_before_next_checkpoint= 0;

end:
  for (i= 0; i < (sizeof(record_pieces)/sizeof(record_pieces[0])); i++)
    my_free(record_pieces[i].str, MYF(MY_ALLOW_ZERO_PTR));
  pthread_mutex_lock(&LOCK_checkpoint);
  checkpoint_in_progress= CHECKPOINT_NONE;
  checkpoints_total++;
  checkpoints_ok_total+= !error;
  pthread_mutex_unlock(&LOCK_checkpoint);
  DBUG_RETURN(error);
}


/**
   @brief Initializes the checkpoint module

   @param  create_background_thread If one wants the module to now create a
                                    thread which will periodically do
                                    checkpoints, and flush dirty pages, in the
                                    background.

   @return Operation status
    @retval 0   ok
    @retval !=0 error
*/

int ma_checkpoint_init(my_bool create_background_thread)
{
  pthread_t th;
  int res= 0;
  DBUG_ENTER("ma_checkpoint_init");
  checkpoint_inited= TRUE;
  checkpoint_thread_die= 2; /* not yet born == dead */
  if (pthread_mutex_init(&LOCK_checkpoint, MY_MUTEX_INIT_SLOW) ||
      pthread_cond_init(&COND_checkpoint, 0))
    res= 1;
  else if (create_background_thread)
  {
    if (!(res= pthread_create(&th, NULL, ma_checkpoint_background, NULL)))
      checkpoint_thread_die= 0; /* thread lives, will have to be killed */
  }
  DBUG_RETURN(res);
}


/**
   @brief Destroys the checkpoint module
*/

void ma_checkpoint_end()
{
  DBUG_ENTER("ma_checkpoint_end");
  if (checkpoint_inited)
  {
    pthread_mutex_lock(&LOCK_checkpoint);
    if (checkpoint_thread_die != 2) /* thread was started ok */
    {
      DBUG_PRINT("info",("killing Maria background checkpoint thread"));
      checkpoint_thread_die= 1; /* kill it */
      do /* and wait for it to be dead */
      {
        /* wake it up if it was in a sleep */
        pthread_cond_broadcast(&COND_checkpoint);
        DBUG_PRINT("info",("waiting for Maria background checkpoint thread"
                           " to die"));
        pthread_cond_wait(&COND_checkpoint, &LOCK_checkpoint);
      }
      while (checkpoint_thread_die != 2);
    }
    pthread_mutex_unlock(&LOCK_checkpoint);
    my_free((uchar *)dfiles, MYF(MY_ALLOW_ZERO_PTR));
    my_free((uchar *)kfiles, MYF(MY_ALLOW_ZERO_PTR));
    pthread_mutex_destroy(&LOCK_checkpoint);
    pthread_cond_destroy(&COND_checkpoint);
    checkpoint_inited= FALSE;
  }
  DBUG_VOID_RETURN;
}


/**
   @brief dirty-page filtering criteria for MEDIUM checkpoint.

   We flush data/index pages which have been dirty since the previous
   checkpoint (this is the two-checkpoint rule: the REDO phase will not have
   to start from earlier than the next-to-last checkpoint), and all dirty
   bitmap pages.

   @param  type                Page's type
   @param  pageno              Page's number
   @param  rec_lsn             Page's rec_lsn
   @param  arg                 filter_param

   @return Operation status
    @retval 0   don't flush the page
    @retval 1   flush the page
*/

static int filter_flush_data_file_medium(enum pagecache_page_type type,
                                         pgcache_page_no_t pageno,
                                         LSN rec_lsn, void *arg)
{
  struct st_filter_param *param= (struct st_filter_param *)arg;
  return ((type == PAGECACHE_LSN_PAGE) &&
          (cmp_translog_addr(rec_lsn, param->up_to_lsn) <= 0)) ||
    (param->is_data_file &&
     ((pageno % param->pages_covered_by_bitmap) == 0));
}


/**
   @brief dirty-page filtering criteria for FULL checkpoint.

   We flush all dirty data/index pages and all dirty bitmap pages.

   @param  type                Page's type
   @param  pageno              Page's number
   @param  rec_lsn             Page's rec_lsn
   @param  arg                 filter_param

   @return Operation status
    @retval 0   don't flush the page
    @retval 1   flush the page
*/

static int filter_flush_data_file_full(enum pagecache_page_type type,
                                       pgcache_page_no_t pageno,
                                       LSN rec_lsn
                                       __attribute__ ((unused)),
                                       void *arg)
{
  struct st_filter_param *param= (struct st_filter_param *)arg;
  return (type == PAGECACHE_LSN_PAGE) ||
    (param->is_data_file &&
     ((pageno % param->pages_covered_by_bitmap) == 0));
}


/**
   @brief dirty-page filtering criteria for INDIRECT checkpoint.

   We flush all dirty bitmap pages.

   @param  type                Page's type
   @param  pageno              Page's number
   @param  rec_lsn             Page's rec_lsn
   @param  arg                 filter_param

   @return Operation status
    @retval 0   don't flush the page
    @retval 1   flush the page
*/

static int filter_flush_data_file_indirect(enum pagecache_page_type type
                                           __attribute__ ((unused)),
                                           pgcache_page_no_t pageno,
                                           LSN rec_lsn
                                           __attribute__ ((unused)),
                                           void *arg)
{
  struct st_filter_param *param= (struct st_filter_param *)arg;
  return
    (param->is_data_file &&
     ((pageno % param->pages_covered_by_bitmap) == 0));
}


/**
   @brief dirty-page filtering criteria for background flushing thread.

   We flush data pages which have been dirty since the previous checkpoint
   (this is the two-checkpoint rule: the REDO phase will not have to start
   from earlier than the next-to-last checkpoint), and all dirty bitmap
   pages. But we flush no more than a certain number of pages (to have an
   even flushing, no write burst).

   @param  type                Page's type
   @param  pageno              Page's number
   @param  rec_lsn             Page's rec_lsn
   @param  arg                 filter_param

   @return Operation status
    @retval 0   don't flush the page
    @retval 1   flush the page
    @retval 2   don't flush the page and following pages
*/

static int filter_flush_data_file_evenly(enum pagecache_page_type type,
                                         pgcache_page_no_t pageno
                                         __attribute__ ((unused)),
                                         LSN rec_lsn, void *arg)
{
  struct st_filter_param *param= (struct st_filter_param *)arg;
  if (unlikely(param->max_pages == 0)) /* all flushed already */
    return 2;
  if ((type == PAGECACHE_LSN_PAGE) &&
      (cmp_translog_addr(rec_lsn, param->up_to_lsn) <= 0))
  {
    param->max_pages--;
    return 1;
  }
  return 0;
}


/**
   @brief Background thread which does checkpoints and flushes periodically.

   Takes a checkpoint every 30th second. After taking a checkpoint, all pages
   dirty at the time of that checkpoint are flushed evenly until it is time to
   take another checkpoint (30 seconds later). This ensures that the REDO
   phase starts at earliest (in LSN time) at the next-to-last checkpoint
   record ("two-checkpoint rule").

   @note MikaelR questioned why the same thread does two different jobs, the
   risk could be that while a checkpoint happens no LRD flushing happens.

   @note MikaelR noted that he observed that Linux's file cache may never
   fsync to  disk until this cache is full, at which point it decides to empty
   the cache, making the machine very slow. A solution was to fsync after
   writing 2 MB.
*/

pthread_handler_t ma_checkpoint_background(void *arg __attribute__((unused)))
{
  const uint sleep_unit= 1 /* 1 second */,
    time_between_checkpoints= 30; /* 30 sleep units */
  uint sleeps= 0;

  my_thread_init();
  DBUG_PRINT("info",("Maria background checkpoint thread starts"));
  for(;;)
  {
#if 0 /* good for testing, to do a lot of checkpoints, finds a lot of bugs */
    sleeps=0;
#endif
    uint pages_bunch_size;
    struct st_filter_param filter_param;
    PAGECACHE_FILE *dfile; /**< data file currently being flushed */
    PAGECACHE_FILE *kfile; /**< index file currently being flushed */
    TRANSLOG_ADDRESS log_horizon_at_last_checkpoint= LSN_IMPOSSIBLE;
    ulonglong pagecache_flushes_at_last_checkpoint= 0;
    struct timespec abstime;
    switch((sleeps++) % time_between_checkpoints)
    {
    case 0:
      /*
        With background flushing evenly distributed over the time
        between two checkpoints, we should have only little flushing to do
        in the checkpoint.
      */
      /*
        No checkpoint if no work of interest for recovery was done
        since last checkpoint. Such work includes log writing (lengthens
        recovery, checkpoint would shorten it), page flushing (checkpoint
        would decrease the amount of read pages in recovery).
      */
      if ((translog_get_horizon() == log_horizon_at_last_checkpoint) &&
          (pagecache_flushes_at_last_checkpoint ==
           maria_pagecache->global_cache_write))
      {
        /* safety against errors during flush by this thread: */
        pages_to_flush_before_next_checkpoint= 0;
        break;
      }
      ma_checkpoint_execute(CHECKPOINT_MEDIUM, TRUE);
      /*
        Snapshot this kind of "state" of the engine. Note that the value below
        is possibly greater than last_checkpoint_lsn.
      */
      log_horizon_at_last_checkpoint= translog_get_horizon();
      pagecache_flushes_at_last_checkpoint=
        maria_pagecache->global_cache_write;
      /*
        If the checkpoint above succeeded it has set d|kfiles and
        d|kfiles_end. If is has failed, it has set
        pages_to_flush_before_next_checkpoint to 0 so we will skip flushing
        and sleep until the next checkpoint.
      */
      break;
    case 1:
      /* set up parameters for background page flushing */
      filter_param.up_to_lsn= last_checkpoint_lsn;
      pages_bunch_size= pages_to_flush_before_next_checkpoint /
        time_between_checkpoints;
      dfile= dfiles;
      kfile= kfiles;
      /* fall through */
    default:
      if (pages_bunch_size > 0)
      {
        /* flush a bunch of dirty pages */
        filter_param.max_pages= pages_bunch_size;
        filter_param.is_data_file= TRUE;
        while (dfile != dfiles_end)
        {
          int res=
            flush_pagecache_blocks_with_filter(maria_pagecache,
                                               dfile, FLUSH_KEEP,
                                               filter_flush_data_file_evenly,
                                               &filter_param);
          /* note that it may just be a pinned page */
          if (unlikely(res))
            fprintf(stderr, "Maria engine: warning - background page flush"
                    " failed\n");
          if (filter_param.max_pages == 0) /* bunch all flushed, sleep */
            break; /* and we will continue with the same file */
          dfile++; /* otherwise all this file is flushed, move to next file */
        }
        filter_param.is_data_file= FALSE;
        while (kfile != kfiles_end)
        {
          int res=
            flush_pagecache_blocks_with_filter(maria_pagecache,
                                               dfile, FLUSH_KEEP,
                                               filter_flush_data_file_evenly,
                                               &filter_param);
          if (unlikely(res))
            fprintf(stderr, "Maria engine: warning - background page flush"
                    " failed\n");
          if (filter_param.max_pages == 0) /* bunch all flushed, sleep */
            break; /* and we will continue with the same file */
          kfile++; /* otherwise all this file is flushed, move to next file */
        }
      }
    }
    pthread_mutex_lock(&LOCK_checkpoint);
    if (checkpoint_thread_die == 1)
      break;
#if 0 /* good for testing, to do a lot of checkpoints, finds a lot of bugs */
    pthread_mutex_unlock(&LOCK_checkpoint);
    my_sleep(100000); // a tenth of a second
    pthread_mutex_lock(&LOCK_checkpoint);
#else
    /* To have a killable sleep, we use timedwait like our SQL GET_LOCK() */
    set_timespec(abstime, sleep_unit);
    pthread_cond_timedwait(&COND_checkpoint, &LOCK_checkpoint, &abstime);
#endif
    if (checkpoint_thread_die == 1)
      break;
    pthread_mutex_unlock(&LOCK_checkpoint);
  }
  pthread_mutex_unlock(&LOCK_checkpoint);
  DBUG_PRINT("info",("Maria background checkpoint thread ends"));
  /*
    A last checkpoint, now that all tables should be closed; to have instant
    recovery later. We always do it, because the test above about number of
    log records or flushed pages is only approximative. For example, some log
    records may have been written while ma_checkpoint_execute() above was
    running, or some pages may have been flushed during this time. Thus it
    could be that, while nothing has changed since that checkpoint's *end*, if
    we recovered from that checkpoint we would have a non-empty dirty pages
    list, REDOs to execute, and we don't want that, we want a clean shutdown
    to have an empty recovery (simplifies upgrade/backups: one can just do a
    clean shutdown, copy its tables to another system without copying the log
    or control file and it will work because recovery will not need those).
    Another reason why it's approximative is that a log record may have been
    written above between ma_checkpoint_execute() and the
    tranlog_get_horizon() which follows.
    So, we have at least two checkpoints per start/stop of the engine, and
    only two if the engine stays idle.
  */
  ma_checkpoint_execute(CHECKPOINT_FULL, FALSE);
  pthread_mutex_lock(&LOCK_checkpoint);
  checkpoint_thread_die= 2; /* indicate that we are dead */
  /* wake up ma_checkpoint_end() which may be waiting for our death */
  pthread_cond_broadcast(&COND_checkpoint);
  /* broadcast was inside unlock because ma_checkpoint_end() destroys mutex */
  pthread_mutex_unlock(&LOCK_checkpoint);
  my_thread_end();
  return 0;
}


/**
   @brief Allocates buffer and stores in it some info about open tables,
   does some flushing on those.

   Does the allocation because the caller cannot know the size itself.
   Memory freeing is to be done by the caller (if the "str" member of the
   LEX_STRING is not NULL).
   The caller is taking a checkpoint.

   @param[out]  str        pointer to where the allocated buffer,
                           and its size, will be put; buffer will be filled
                           with info about open tables
   @param       checkpoint_start_log_horizon  Of the in-progress checkpoint
                                              record.

   @return Operation status
     @retval 0      OK
     @retval 1      Error
*/

static int collect_tables(LEX_STRING *str, LSN checkpoint_start_log_horizon)
{
  MARIA_SHARE **distinct_shares= NULL;
  char *ptr;
  uint error= 1, sync_error= 0, nb, nb_stored, i;
  my_bool unmark_tables= TRUE;
  uint total_names_length;
  LIST *pos; /**< to iterate over open tables */
  struct st_state_copy {
    uint index;
    MARIA_STATE_INFO state;
  };
  struct st_state_copy *state_copies= NULL, /**< fixed-size cache of states */
    *state_copies_end, /**< cache ends here */
    *state_copy; /**< iterator in cache */
  TRANSLOG_ADDRESS state_copies_horizon; /**< horizon of states' _copies_ */
  DBUG_ENTER("collect_tables");

  /* let's make a list of distinct shares */
  pthread_mutex_lock(&THR_LOCK_maria);
  for (nb= 0, pos= maria_open_list; pos; pos= pos->next)
  {
    MARIA_HA *info= (MARIA_HA*)pos->data;
    MARIA_SHARE *share= info->s;
    /* the first three variables below can never change */
    if (share->base.born_transactional && !share->temporary &&
        share->mode != O_RDONLY &&
        !(share->in_checkpoint & MARIA_CHECKPOINT_SEEN_IN_LOOP))
    {
      /*
        Why we didn't take intern_lock above: table had in_checkpoint==0 so no
        thread could set in_checkpoint. And no thread needs to know that we
        are setting in_checkpoint, because only maria_close() needs it and
        cannot run now as we hold THR_LOCK_maria.
      */
      /*
        This table is relevant for checkpoint and not already seen. Mark it,
        so that it is not seen again in the loop.
      */
      nb++;
      DBUG_ASSERT(share->in_checkpoint == 0);
      /* This flag ensures that we count only _distinct_ shares. */
      share->in_checkpoint= MARIA_CHECKPOINT_SEEN_IN_LOOP;
    }
  }
  if (unlikely((distinct_shares=
                (MARIA_SHARE **)my_malloc(nb * sizeof(MARIA_SHARE *),
                                          MYF(MY_WME))) == NULL))
    goto err;
  for (total_names_length= 0, i= 0, pos= maria_open_list; pos; pos= pos->next)
  {
    MARIA_HA *info= (MARIA_HA*)pos->data;
    MARIA_SHARE *share= info->s;
    if (share->in_checkpoint & MARIA_CHECKPOINT_SEEN_IN_LOOP)
    {
      distinct_shares[i++]= share;
      /*
        With this we prevent the share from going away while we later flush
        and force it without holding THR_LOCK_maria. For example if the share
        could be my_free()d by maria_close() we would have a problem when we
        access it to flush the table. We "pin" the share pointer.
        And we also take down MARIA_CHECKPOINT_SEEN_IN_LOOP, so that it is
        not seen again in the loop.
      */
      share->in_checkpoint= MARIA_CHECKPOINT_LOOKS_AT_ME;
      /** @todo avoid strlen() */
      total_names_length+= strlen(share->open_file_name);
    }
  }

  DBUG_ASSERT(i == nb);
  pthread_mutex_unlock(&THR_LOCK_maria);
  DBUG_PRINT("info",("found %u table shares", nb));

  str->length=
    4 +               /* number of tables */
    (2 +              /* short id */
     4 +              /* kfile */
     4 +              /* dfile */
     LSN_STORE_SIZE + /* first_log_write_at_lsn */
     1                /* end-of-name 0 */
     ) * nb + total_names_length;
  if (unlikely((str->str= my_malloc(str->length, MYF(MY_WME))) == NULL))
    goto err;

  ptr= str->str;
  ptr+= 4; /* real number of stored tables is not yet know */

  struct st_filter_param filter_param;
  /* only possible checkpointer, so can do the read below without mutex */
  filter_param.up_to_lsn= last_checkpoint_lsn;
  PAGECACHE_FILTER filter;
  switch(checkpoint_in_progress)
  {
  case CHECKPOINT_MEDIUM:
    filter= &filter_flush_data_file_medium;
    break;
  case CHECKPOINT_FULL:
    filter= &filter_flush_data_file_full;
    break;
  case CHECKPOINT_INDIRECT:
    filter= &filter_flush_data_file_indirect;
    break;
  default:
    DBUG_ASSERT(0);
    goto err;
  }

  /*
    The principle of reading/writing the state below is explained in
    ma_recovery.c, look for "Recovery of the state".
  */
#define STATE_COPIES 1024
  state_copies= (struct st_state_copy *)
    my_malloc(STATE_COPIES * sizeof(struct st_state_copy), MYF(MY_WME));
  dfiles= (PAGECACHE_FILE *)my_realloc((uchar *)dfiles,
                                       /* avoid size of 0 for my_realloc */
                                       max(1, nb) * sizeof(PAGECACHE_FILE),
                                       MYF(MY_WME));
  kfiles= (PAGECACHE_FILE *)my_realloc((uchar *)kfiles,
                                       /* avoid size of 0 for my_realloc */
                                       max(1, nb) * sizeof(PAGECACHE_FILE),
                                       MYF(MY_WME));
  if (unlikely((state_copies == NULL) ||
               (dfiles == NULL) || (kfiles == NULL)))
    goto err;
  state_copy= state_copies_end= NULL;
  dfiles_end= dfiles;
  kfiles_end= kfiles;

  for (nb_stored= 0, i= 0; i < nb; i++)
  {
    MARIA_SHARE *share= distinct_shares[i];
    PAGECACHE_FILE kfile, dfile;
    if (!(share->in_checkpoint & MARIA_CHECKPOINT_LOOKS_AT_ME))
    {
      /* No need for a mutex to read the above, only us can write this flag */
      continue;
    }
    DBUG_PRINT("info",("looking at table '%s'", share->open_file_name));
    if (state_copy == state_copies_end) /* we have no more cached states */
    {
      /*
        Collect and cache a bunch of states. We do this for many states at a
        time, to not lock/unlock the log's lock too often.
      */
      uint j, bound= min(nb, i + STATE_COPIES);
      state_copy= state_copies;
      /* part of the state is protected by log's lock */
      translog_lock();
      state_copies_horizon= translog_get_horizon_no_lock();
      for (j= i; j < bound; j++)
      {
        MARIA_SHARE *share2= distinct_shares[j];
        if (!(share2->in_checkpoint & MARIA_CHECKPOINT_LOOKS_AT_ME))
          continue;
        state_copy->index= j;
        state_copy->state= share2->state; /* we copy the state */
        state_copy++;
        /*
          data_file_length is not updated under log's lock by the bitmap
          code, but writing a wrong data_file_length is ok: a next
          maria_close() will correct it; if we crash before, Recovery will
          set it to the true physical size.
        */
      }
      translog_unlock();
      state_copies_end= state_copy;
      state_copy= state_copies;
      /* so now we have cached states */
    }

    /* locate our state among these cached ones */
    for ( ; state_copy->index != i; state_copy++)
      DBUG_ASSERT(state_copy < state_copies_end);

    filter_param.pages_covered_by_bitmap= share->bitmap.pages_covered;
    /* OS file descriptors are ints which we stored in 4 bytes */
    compile_time_assert(sizeof(int) == 4);
    pthread_mutex_lock(&share->intern_lock);
    /*
      Tables in a normal state have their two file descriptors open.
      In some rare cases like REPAIR, some descriptor may be closed or even
      -1. If that happened, the _ma_state_info_write() may fail. This is
      prevented by enclosing all all places which close/change kfile.file with
      intern_lock.
    */
    kfile= share->kfile;
    dfile= share->bitmap.file;
    /*
      Ignore table which has no logged writes (all its future log records will
      be found naturally by Recovery). Ignore obsolete shares (_before_
      setting themselves to last_version=0 they already did all flush and
      sync; if we flush their state now we may be flushing an obsolete state
      onto a newer one (assuming the table has been reopened with a different
      share but of course same physical index file).
    */
    if ((share->id != 0) && (share->last_version != 0))
    {
      /** @todo avoid strlen */
      uint open_file_name_len= strlen(share->open_file_name) + 1;
      /* remember the descriptors for background flush */
      *(dfiles_end++)= dfile;
      *(kfiles_end++)= kfile;
      /* we will store this table in the record */
      nb_stored++;
      int2store(ptr, share->id);
      ptr+= 2;
      /*
        We must store the OS file descriptors, because the pagecache, which
        tells us the list of dirty pages, refers to these pages by OS file
        descriptors. An alternative is to make the page cache aware of the
        2-byte id and of the location of a page ("is it a data file page or an
        index file page?").
        If one descriptor is -1, normally there should be no dirty pages
        collected for this file, it's ok to store -1, it will not be used.
      */
      int4store(ptr, kfile.file);
      ptr+= 4;
      int4store(ptr, dfile.file);
      ptr+= 4;
      lsn_store(ptr, share->lsn_of_file_id);
      ptr+= LSN_STORE_SIZE;
      /*
        first_bitmap_with_space is not updated under log's lock, and is
        important. We would need the bitmap's lock to get it right. Recovery
        of this is not clear, so we just play safe: write it out as
        unknown: if crash, _ma_bitmap_init() at next open (for example in
        Recovery) will convert it to 0 and thus the first insertion will
        search for free space from the file's first bitmap (0) -
        under-optimal but safe.
        If no crash, maria_close() will write the exact value.
      */
      state_copy->state.first_bitmap_with_space= ~(ulonglong)0;
      memcpy(ptr, share->open_file_name, open_file_name_len);
      ptr+= open_file_name_len;
      if (cmp_translog_addr(share->state.is_of_horizon,
                            checkpoint_start_log_horizon) >= 0)
      {
        /*
          State was flushed recently, it does not hold down the log's
          low-water mark and will not give avoidable work to Recovery. So we
          needn't flush it. Also, it is possible that while we copied the
          state above (under log's lock, without intern_lock) it was being
          modified in memory or flushed to disk (without log's lock, under
          intern_lock, like in maria_extra()), so our copy may be incorrect
          and we should not flush it.
          It may also be a share which got last_version==0 since we checked
          last_version; in this case, it flushed its state and the LSN test
          above will catch it.
        */
      }
      else
      {
        /*
          We could do the state flush only if share->changed, but it's
          tricky.
          Consider a maria_write() which has written REDO,UNDO, and before it
          calls _ma_writeinfo() (setting share->changed=1), checkpoint
          happens and sees share->changed=0, does not flush state. It is
          possible that Recovery does not start from before the REDO and thus
          the state is not recovered. A solution may be to set
          share->changed=1 under log mutex when writing log records.
          But as anyway we have another problem below, this optimization would
          be of little use.
        */
        /** @todo flush state only if changed since last checkpoint */
        DBUG_ASSERT(share->last_version != 0);
        state_copy->state.is_of_horizon= share->state.is_of_horizon=
          state_copies_horizon;
        if (kfile.file >= 0)
          sync_error|=
            _ma_state_info_write_sub(kfile.file, &state_copy->state, 1);
        /*
          We don't set share->changed=0 because it may interfere with a
          concurrent _ma_writeinfo() doing share->changed=1 (cancel its
          effect). The sad consequence is that we will flush the same state at
          each checkpoint if the table was once written and then not anymore.
        */
      }
      sync_error|=
        _ma_flush_bitmap(share); /* after that, all is in page cache */
      DBUG_ASSERT(share->pagecache == maria_pagecache);
    }
    if (share->in_checkpoint & MARIA_CHECKPOINT_SHOULD_FREE_ME)
    {
      /* maria_close() left us to free the share */
      pthread_mutex_unlock(&share->intern_lock);
      pthread_mutex_destroy(&share->intern_lock);
      my_free((uchar *)share, MYF(0));
    }
    else
    {
      /* share goes back to normal state */
      share->in_checkpoint= 0;
      pthread_mutex_unlock(&share->intern_lock);
    }

    /*
      We do the big disk writes out of intern_lock to not block other
      users of this table (intern_lock is taken at the start and end of
      every statement). This means that file descriptors may be invalid
      (files may have been closed for example by HA_EXTRA_PREPARE_FOR_*
      under Windows, or REPAIR). This should not be a problem as we use
      MY_IGNORE_BADFD. Descriptors may even point to other files but then
      the old blocks (of before the close) must have been flushed for sure,
      so our flush will flush new blocks (of after the latest open) and that
      should do no harm.
    */
    /*
      If CHECKPOINT_MEDIUM, this big flush below may result in a
      serious write burst. Realize that all pages dirtied between the
      last checkpoint and the one we are doing now, will be flushed at
      next checkpoint, except those evicted by LRU eviction (depending on
      the size of the page cache compared to the size of the working data
      set, eviction may be rare or frequent).
      We avoid that burst by anticipating: those pages are flushed
      in bunches spanned regularly over the time interval between now and
      the next checkpoint, by a background thread. Thus the next checkpoint
      will have only little flushing to do (CHECKPOINT_MEDIUM should thus be
      only a little slower than CHECKPOINT_INDIRECT).
    */

    /**
       @todo we ignore the error because it may be just due a pinned page;
       we should rather fix the function below to distinguish between
       pinned page and write error. Then we can turn the warning into an
       error.
    */
    if (((filter_param.is_data_file= TRUE),
         flush_pagecache_blocks_with_filter(maria_pagecache,
                                            &dfile, FLUSH_KEEP,
                                            filter, &filter_param)) ||
        ((filter_param.is_data_file= FALSE),
         flush_pagecache_blocks_with_filter(maria_pagecache,
                                            &kfile, FLUSH_KEEP,
                                            filter, &filter_param)))
      fprintf(stderr, "Maria engine: warning - checkpoint page flush"
              " failed\n"); /** @todo improve */
      /*
        fsyncs the fd, that's the loooong operation (e.g. max 150 fsync
        per second, so if you have touched 1000 files it's 7 seconds).
      */
    sync_error|=
      my_sync(dfile.file, MYF(MY_WME | MY_IGNORE_BADFD)) |
      my_sync(kfile.file, MYF(MY_WME | MY_IGNORE_BADFD));
    /*
      in case of error, we continue because writing other tables to disk is
      still useful.
    */
  }

  if (sync_error)
    goto err;
  /* We maybe over-estimated (due to share->id==0 or last_version==0) */
  DBUG_ASSERT(str->length >= (uint)(ptr - str->str));
  str->length= (uint)(ptr - str->str);
  /*
    As we support max 65k tables open at a time (2-byte short id), we
    assume uint is enough for the cumulated length of table names; and
    LEX_STRING::length is uint.
  */
  int4store(str->str, nb_stored);
  error= unmark_tables= 0;

err:
  if (unlikely(unmark_tables))
  {
    /* maria_close() uses THR_LOCK_maria from start to end */
    pthread_mutex_lock(&THR_LOCK_maria);
    for (i= 0; i < nb; i++)
    {
      MARIA_SHARE *share= distinct_shares[i];
      if (share->in_checkpoint & MARIA_CHECKPOINT_SHOULD_FREE_ME)
      {
        /* maria_close() left us to free the share */
        pthread_mutex_destroy(&share->intern_lock);
        my_free((uchar *)share, MYF(0));
      }
      else
      {
        /* share goes back to normal state */
        share->in_checkpoint= 0;
      }
    }
    pthread_mutex_unlock(&THR_LOCK_maria);
  }
  my_free((uchar *)distinct_shares, MYF(MY_ALLOW_ZERO_PTR));
  my_free((uchar *)state_copies, MYF(MY_ALLOW_ZERO_PTR));
  DBUG_RETURN(error);
}
