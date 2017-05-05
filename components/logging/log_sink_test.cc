/* Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02111-1307  USA */

#include "log_service_imp.h"

#include <assert.h>
#include "my_compiler.h"
#include <mysqld_error.h> // ER_*

#include <mysql/components/services/log_builtins.h>
#include <mysql/components/services/log_builtins_filter.h>

#define MY_NAME "log_sink_test"

extern REQUIRES_SERVICE_PLACEHOLDER(registry);


static my_h_service                bls=        nullptr;
static bool                        inited=     false;
static bool                        failed=     false;
static bool                        run_tests=  true;
SERVICE_TYPE(log_builtins)        *log_bi=     nullptr;
SERVICE_TYPE(log_builtins_filter) *log_bf=     nullptr;
SERVICE_TYPE(log_builtins_string) *log_bs=     nullptr;


/**
  Variable listener. This is a temporary solution until we have
  per-component system variables. "check" is where our component
  can veto.

  @param   ll  a log_line with a list-item describing the variable
               (name, new value)
  @retval   0  for allow (including when we don't feel the event is for us),
  @retval  -1  for deny (malformed input, caller broken)
  @retval   1  for deny (wrong data-type, or invalid value submitted by user)
*/
DEFINE_METHOD(int, log_service_imp::variable_check,
              (log_line *ll MY_ATTRIBUTE((unused))))
{
  return 0;
}


/**
  Variable listener. This is a temporary solution until we have
  per-component system variables. "update" is where we're told
  to update our state (if the variable concerns us to begin with).

  @param  ll  a log_line with a list-item describing the variable
              (name, new value)
  @retval  0  event is not for us
  @retval -1  for failure (invalid input that wasn't caught in variable_check)
  @retval >0  for success (at least one variable was processed successfully)
*/
DEFINE_METHOD(int, log_service_imp::variable_update,
              (log_line *ll MY_ATTRIBUTE((unused))))
{
  return 0;
}


/**
  delete a rule from the given rule-set

  @param   rs     the rule-set
  @param   t      type of match-item
  @param   key    key  of the match-item if generic (ignored otherwise)
  @param   cond   the condition-code
  @param   action the action-verb

  @retval  false  no problems, rule deleted
  @retval  true   error / no rule deleted
*/
static bool rule_delete(log_filter_ruleset *rs,
                        log_item_type t,
                        log_item_key key,
                        log_filter_cond cond,
                        log_filter_verb action)

{
  size_t              rn;
  log_filter_rule    *r;

  for (rn= 0; (rn < rs->count); rn++)
  {
    r= &rs->rule[rn];

    if ((r->match.type == t) &&
        (!log_bi->item_generic_type(t) ||
         (0 == log_bs->compare(key, r->match.key, 0, false))) &&
        (r->verb       == action) &&
        (r->cond       == cond) &&
        (r->flags      &  LOG_FILTER_FLAG_SYNTHETIC))
    {
      // found our rule. remove it.
      size_t rt;

      if (r->match.alloc & LOG_ITEM_FREE_KEY)
        log_bs->free((void *) r->match.key);
      if ((r->match.alloc & LOG_ITEM_FREE_VALUE) &&
          (r->match.item_class == LOG_LEX_STRING))
        log_bs->free((void *) r->match.data.data_string.str);

      if (r->aux.alloc & LOG_ITEM_FREE_KEY)
        log_bs->free((void *) r->aux.key);
      if ((r->aux.alloc & LOG_ITEM_FREE_VALUE) &&
          (r->aux.item_class == LOG_LEX_STRING))
        log_bs->free((void *) r->aux.data.data_string.str);

      rs->count--;
      for (rt= rn; rt < rs->count; rt++)
      {
        rs->rule[rt]= rs->rule[rt + 1];
      }

      return false;
    }
  }

  return true;
}

#define KEY_PRIO_CHANGE "prio_change"
#define VAL_PRIO_CHANGE "yes"

#define KEY_DEL_ITEM    "bark"
#define VAL_DEL_ITEM    "delete_by_rule"

#define KEY_ADD_ITEM    "far"
#define VAL_ADD_ITEM    "added_by_rule"

#define KEY_PRS_ITEM    "meow"

static void test_add_item_log_me(log_filter_ruleset *rs)
{
  LogEvent().type(LOG_TYPE_ERROR)
            .string_value(KEY_PRS_ITEM, "test_me_for_presence")
            .string_value(KEY_DEL_ITEM, "delete_me_by_rule")
            .string_value(KEY_PRIO_CHANGE, VAL_PRIO_CHANGE)
            .source_file(MY_NAME)
            .message("filter_rules: %d", rs->count);
}

/**
  Show that adding key/value pairs actually works.

  @retval  0  success
  @retval -1  could not acquire ruleset (to add throttle)
  @retval -2  could not initialize new rule
  @retval -3  could not acquire ruleset (to delete throttle)
*/
static int test_add_item()
{
  log_filter_ruleset *rs;
  log_filter_rule    *r;
  int                 rr= -99;

  if ((rs= (log_filter_ruleset *)
       log_bf->filter_ruleset_get(LOG_BUILTINS_LOCK_EXCLUSIVE)) == nullptr)
    return -1;

  if ((r= (log_filter_rule *) log_bf->filter_rule_init()) == nullptr)
  {
    rr= -2;
    goto done;
  }
  else
  // set up a demo prio change
  {
    log_item_data *d;
    // we don't really need to copy here, but let's cover that code path
    char          *s= log_bs->strndup(KEY_PRIO_CHANGE,
                                      log_bs->length(KEY_PRIO_CHANGE));

    if (s != nullptr)
    {
      // condition/comparator: equal (string)
      r->cond=  LOG_FILTER_COND_EQ;
      // match information: type MySQL error code, class integer
      d= log_bi->item_set_with_key(&r->match, LOG_ITEM_GEN_LEX_STRING, s,
                                   LOG_ITEM_FREE_KEY|LOG_ITEM_FREE_VALUE);

      if (d != nullptr)
      {
        s= log_bs->strndup(VAL_PRIO_CHANGE, log_bs->length(VAL_PRIO_CHANGE));
        if (s != nullptr)
        {
          d->data_string.str= s;
          d->data_string.length= log_bs->length(s);
        }
        else
          d->data_string.str= nullptr;
      }

      // action/verb: prio, absolute
      r->verb=  LOG_FILTER_PRIO_REL;
      // auxiliary information: new priority
      log_bi->item_set(&r->aux, LOG_ITEM_GEN_INTEGER)->data_integer= 1;

      // not requested by the user
      r->flags= LOG_FILTER_FLAG_SYNTHETIC;
      // rule complete, be counted
      rs->count++;
    }
  }

  if ((r= (log_filter_rule *) log_bf->filter_rule_init()) == nullptr)
  {
    rr= -2;
    goto done;
  }
  else
  // set up a demo item delete
  {
    log_item_data *d;
    // we don't really need to copy here, but let's cover that code path
    char          *s= log_bs->strndup(KEY_DEL_ITEM,
                                      log_bs->length(KEY_DEL_ITEM));

    if (s != nullptr)
    {
      // condition/comparator: not equal (string)
      r->cond=  LOG_FILTER_COND_NE;
      // match information: type MySQL error code, class integer
      d= log_bi->item_set_with_key(&r->match, LOG_ITEM_GEN_LEX_STRING, s,
                                   LOG_ITEM_FREE_KEY|LOG_ITEM_FREE_VALUE);

      if (d != nullptr)
      {
        s= log_bs->strndup(VAL_DEL_ITEM, log_bs->length(VAL_DEL_ITEM));
        if (s != nullptr)
        {
          d->data_string.str= s;
          d->data_string.length= log_bs->length(s);
        }
        else
          d->data_string.str= nullptr;
      }

      // action/verb: delete item
      r->verb=  LOG_FILTER_ITEM_DEL;
      // auxiliary information: delete this item
      r->aux.key= nullptr; // delete uses same item as in cond

      // not requested by the user
      r->flags= LOG_FILTER_FLAG_SYNTHETIC;
      // rule complete, be counted
      rs->count++;
    }
  }

  if ((r= (log_filter_rule *) log_bf->filter_rule_init()) == nullptr)
  {
    rr= -2;
    goto done;
  }
  else
  // set up a demo item add
  {
    log_item_data *d;
    // we don't really need to copy here, but let's cover that code path
    char          *s= log_bs->strndup(KEY_PRS_ITEM,
                                      log_bs->length(KEY_PRS_ITEM));

    if (s != nullptr)
    {
      // condition/comparator: not equal (string)
      r->cond=  LOG_FILTER_COND_PRESENT;
      // match information: type MySQL error code, class integer
      d= log_bi->item_set_with_key(&r->match, LOG_ITEM_GEN_LEX_STRING, s,
                                   LOG_ITEM_FREE_KEY);

      // action/verb: add item
      r->verb=  LOG_FILTER_ITEM_ADD;
      // auxiliary information: new priority
      d= log_bi->item_set_with_key(&r->aux, LOG_ITEM_GEN_LEX_STRING,
                                   KEY_ADD_ITEM, LOG_ITEM_FREE_NONE);
      if (d != nullptr)
      {
        d->data_string.str= VAL_ADD_ITEM;
        d->data_string.length= log_bs->length(VAL_ADD_ITEM);
      }

      // not requested by the user
      r->flags= LOG_FILTER_FLAG_SYNTHETIC;
      // rule complete, be counted
      rs->count++;
    }
  }

  log_bf->filter_ruleset_release();

  // modify and log event
  test_add_item_log_me(rs);

  if ((rs= (log_filter_ruleset *)
       log_bf->filter_ruleset_get(LOG_BUILTINS_LOCK_EXCLUSIVE)) == nullptr)
    return -3;

  assert(!rule_delete(rs, LOG_ITEM_GEN_LEX_STRING, KEY_PRIO_CHANGE,
                      LOG_FILTER_COND_EQ, LOG_FILTER_PRIO_REL));
  assert(!rule_delete(rs, LOG_ITEM_GEN_LEX_STRING, KEY_DEL_ITEM,
                      LOG_FILTER_COND_NE, LOG_FILTER_ITEM_DEL));
  assert(!rule_delete(rs, LOG_ITEM_GEN_LEX_STRING, KEY_PRS_ITEM,
                      LOG_FILTER_COND_PRESENT, LOG_FILTER_ITEM_ADD));

  rr= 0;

done:
  rule_delete(rs, LOG_ITEM_GEN_LEX_STRING, KEY_PRIO_CHANGE,
                      LOG_FILTER_COND_EQ, LOG_FILTER_PRIO_REL);
  rule_delete(rs, LOG_ITEM_GEN_LEX_STRING, KEY_DEL_ITEM,
                      LOG_FILTER_COND_NE, LOG_FILTER_ITEM_DEL);
  rule_delete(rs, LOG_ITEM_GEN_LEX_STRING, KEY_PRS_ITEM,
                      LOG_FILTER_COND_PRESENT, LOG_FILTER_ITEM_ADD);

  log_bf->filter_ruleset_release();

  // log unchanged event
  test_add_item_log_me(rs);

  return rr;
}


/**
  Get coverage for some of the built-ins.

  @retval -1  could not acquire ruleset (to add throttle)
  @retval -2  could not initialize new rule
  @retval -3  could not acquire ruleset (to delete throttle)
*/
static int test_builtins()
{
  // test classifiers
  assert( log_bi->item_numeric_class(LOG_INTEGER));
  assert( log_bi->item_numeric_class(LOG_FLOAT));
  assert(!log_bi->item_numeric_class(LOG_LEX_STRING));
  assert(!log_bi->item_numeric_class(LOG_CSTRING));

  assert(!log_bi->item_string_class(LOG_INTEGER));
  assert(!log_bi->item_string_class(LOG_FLOAT));
  assert( log_bi->item_string_class(LOG_LEX_STRING));
  assert( log_bi->item_string_class(LOG_CSTRING));

  // test functions for wellknowns
  int wellknown= log_bi->wellknown_by_type(LOG_ITEM_LOG_LABEL);
  assert(LOG_ITEM_LOG_LABEL == log_bi->wellknown_get_type(wellknown));

  wellknown= log_bi->wellknown_by_type(LOG_ITEM_GEN_INTEGER);
  const char *wk= log_bi->wellknown_get_name(wellknown);
  assert(LOG_ITEM_TYPE_RESERVED ==
         log_bi->wellknown_by_name(wk, log_bs->length(wk)));

  // make a bag, then create a key/value pair on it
  log_line *ll= log_bi->line_init();
  assert(log_bi->line_item_count(ll) == 0);

  log_item_data *d= log_bi->line_item_set(ll, LOG_ITEM_LOG_LABEL);
  assert(d != nullptr);
  assert(log_bi->line_item_count(ll) == 1);

  // setters (woof)
  assert(!log_bi->item_set_float(      d, 3.1415926927));
  assert(!log_bi->item_set_int(        d, 31415926927));
  assert(!log_bi->item_set_cstring(    d, "pi==3.14"));
  assert(!log_bi->item_set_lexstring(  d, "pi", 2));

  // find our item in the bag
  log_item_iter *it;
  log_item      *li;
  assert((it= log_bi->line_item_iter_acquire(ll)) != nullptr);
  assert((li= log_bi->line_item_iter_first(it))   != nullptr);

  // break item, then detect brokeness
  li->item_class= LOG_FLOAT;
  assert(log_bi->item_inconsistent(li) < 0);

  // release iter
  log_bi->line_item_iter_release(it);

  // try to log it anyway
  log_bi->line_submit(ll);

  // release line
  log_bi->line_exit(ll);

  return 0;
}


/**
  Show that the rate-limiter actually works.

  @retval  0  success!
  @retval -1  could not acquire ruleset (to add throttle)
  @retval -2  could not initialize new rule
  @retval -3  could not acquire ruleset (to delete throttle)
*/
static int test_throttle()
{
  log_filter_ruleset *rs;
  log_filter_rule    *r;
  int                 rr= -99;

  LogEvent().type(LOG_TYPE_ERROR)
            .prio(INFORMATION_LEVEL)
            .source_line(__LINE__)
            .source_file(MY_NAME)
            .message("below: 3*yes per writer == correct.  "
                     ">3*yes per writer == filter fail. "
                     "0*yes == " MY_NAME " fail.");

  if ((rs= (log_filter_ruleset *)
       log_bf->filter_ruleset_get(LOG_BUILTINS_LOCK_EXCLUSIVE)) == nullptr)
    return -1;

  if ((r= (log_filter_rule *) log_bf->filter_rule_init()) == nullptr)
  {
    rr= -2;
    goto done;
  }

  // set up a demo rate-limiter
  {
    // condition/comparator: equal
    r->cond=  LOG_FILTER_COND_EQ;
    // match information: type MySQL error code, class integer
    log_bi->item_set(&r->match, LOG_ITEM_SQL_ERRCODE)->data_integer= ER_YES;

    // action/verb: throttle (rate-limit)
    r->verb=  LOG_FILTER_THROTTLE;
    // auxiliary information: maximum number of messages per minute
    log_bi->item_set(&r->aux, LOG_ITEM_GEN_INTEGER)->data_integer= 3;

    // not requested by the user
    r->flags= LOG_FILTER_FLAG_SYNTHETIC;
    // rule complete, be counted
    rs->count++;
  }

  log_bf->filter_ruleset_release();

  LogEvent().type(LOG_TYPE_ERROR)
            .prio(INFORMATION_LEVEL)
            .source_line(__LINE__)
            .source_file(MY_NAME)
            .message("filter_rules: %d", rs->count);

  {
    int c;

    for (c= 0; c < 16; c++)
      LogEvent().type(LOG_TYPE_ERROR)
                .prio(INFORMATION_LEVEL)
                .source_line(__LINE__)
                .source_file(MY_NAME)
                .lookup(ER_YES);
  }

  if ((rs= (log_filter_ruleset *)
       log_bf->filter_ruleset_get(LOG_BUILTINS_LOCK_EXCLUSIVE)) == nullptr)
    return -3;

  rule_delete(rs, LOG_ITEM_SQL_ERRCODE, nullptr,
              LOG_FILTER_COND_EQ, LOG_FILTER_THROTTLE);

  rr= 0;

done:
  log_bf->filter_ruleset_release();

  LogEvent().type(LOG_TYPE_ERROR)
            .prio(INFORMATION_LEVEL)
            .source_line(__LINE__)
            .source_file(MY_NAME)
            .message("filter_rules: %d", rs->count);

  return rr;
}


/**
  Log a message each from the C and the C++ API to the error logger,
  showing that we can log from external services!
*/
static void banner()
{
  /*
    Use this if for some bizarre reason you really can't or won't use C++
  */
  log_bi->message(LOG_TYPE_ERROR,
                  LOG_ITEM_LOG_PRIO,    (longlong) INFORMATION_LEVEL,
                  LOG_ITEM_LOG_MESSAGE,
                  "using log_message() in external service");

  log_bi->message(LOG_TYPE_ERROR,
                  LOG_ITEM_LOG_PRIO,    (longlong) ERROR_LEVEL,
                  LOG_ITEM_SRC_LINE,    (longlong) 1234,
                  LOG_ITEM_SRC_LINE,    (longlong) 9876,
                  LOG_ITEM_LOG_MESSAGE,
                  "using log_message() with duplicate source-line k/v pair");

  log_bi->message(LOG_TYPE_ERROR,
                  LOG_ITEM_LOG_PRIO,    (longlong) ERROR_LEVEL,
                  LOG_ITEM_GEN_CSTRING, "key", "val",
                  LOG_ITEM_GEN_CSTRING, "key", "val",
                  LOG_ITEM_LOG_MESSAGE,
                  "using log_message() with duplicate generic C-string k/v pair");

  log_bi->message(LOG_TYPE_ERROR,
                  LOG_ITEM_LOG_PRIO,    (longlong) ERROR_LEVEL,
                  LOG_ITEM_GEN_CSTRING, "key", "val",
                  LOG_ITEM_GEN_INTEGER, "key", (longlong) 4711,
                  LOG_ITEM_LOG_VERBATIM,
                  "using log_message() with duplicate generic mixed k/v pair");

  log_bi->message(LOG_TYPE_ERROR,
                  LOG_ITEM_LOG_PRIO,    (longlong) ERROR_LEVEL,
                  LOG_ITEM_SYS_ERRNO,   (longlong) 0,
                  LOG_ITEM_LOG_VERBATIM,
                  "using log_message() with errno 0");

  log_bi->message(LOG_TYPE_ERROR,
                  LOG_ITEM_LOG_PRIO,   (longlong) ERROR_LEVEL,
                  LOG_ITEM_LOG_LOOKUP, (longlong) ER_YES);

  log_bi->message(LOG_TYPE_ERROR,
                  LOG_ITEM_LOG_PRIO,    (longlong) ERROR_LEVEL,
                  LOG_ITEM_SQL_ERRSYMBOL, "ER_NO",
                  LOG_ITEM_LOG_VERBATIM,
                  "using log_message() with errsymbol");

  /*
    Fluent C++ API.  Use this free-form constructor if-and-only-if
    you do NOT have error messages registered with the server (and
    therefore need to use ad hoc messages with message() or verbatim().
  */
  LogEvent().type(LOG_TYPE_ERROR)
            .prio(INFORMATION_LEVEL)
            .source_line(__LINE__)
            .source_file(MY_NAME)
            .float_value("test_float", 3.1415926927)
            .int_value("test_int",   739241)
            .string_value("test_cstring", "cstring")
            .string_value("test_lexstring", "lexstring", 9)
            .message("using LogEvent() object in external service");

  // built-in API test: test "well-known" lookups
  {
    int         wellknown= log_bi->wellknown_by_type(LOG_ITEM_LOG_LABEL);
    const char *label_key= log_bi->wellknown_get_name(wellknown);
    int         wellagain= log_bi->wellknown_by_name(label_key,
                                                     log_bs->length(label_key));

    assert(wellknown == wellagain);

    assert(LOG_ITEM_TYPE_NOT_FOUND == log_bi->wellknown_by_name("", 0));
  }

  // built-in API test: test item_consistent() checks
  {
    log_item  my_item,
             *li=             &my_item;

    const char *consistent[]= { "OK", "NOT_FOUND", "RESERVED",
                                "CLASS_MISMATCH", "KEY_MISMATCH",
                                "STRING_NULL", "KEY_NULL" };

    // My_item is in some undefined, garbage state so far.

    // LOG_ITEM_TYPE_NOT_FOUND
    li->type=                 ((log_item_type) (~0));
    LogEvent().type(LOG_TYPE_ERROR)
              .prio(INFORMATION_LEVEL)
              .message("item_inconsistent(#%d): %s",
                       1, consistent[-log_bi->item_inconsistent(li)]);

    // LOG_ITEM_CLASS_MISMATCH
    li->type=                 LOG_ITEM_LOG_MESSAGE;
    li->item_class=           LOG_INTEGER;
    LogEvent().type(LOG_TYPE_ERROR)
              .prio(INFORMATION_LEVEL)
              .message("item_inconsistent(#%d): %s",
                       2, consistent[-log_bi->item_inconsistent(li)]);

    // LOG_ITEM_KEY_MISMATCH
    li->type=                 LOG_ITEM_LOG_PRIO;
    li->item_class=           LOG_INTEGER;
    li->key=                  "-fail-";
    LogEvent().type(LOG_TYPE_ERROR)
              .prio(INFORMATION_LEVEL)
              .message("item_inconsistent(#%d): %s",
                       2, consistent[-log_bi->item_inconsistent(li)]);

    // LOG_ITEM_KEY_NULL
    li->type=                 LOG_ITEM_LOG_PRIO;
    li->item_class=           LOG_INTEGER;
    li->key=                  nullptr;
    LogEvent().type(LOG_TYPE_ERROR)
              .prio(INFORMATION_LEVEL)
              .message("item_inconsistent(#%d): %s",
                       3, consistent[-log_bi->item_inconsistent(li)]);

    // LOG_ITEM_STRING_NULL
    li->type=                 LOG_ITEM_LOG_MESSAGE;
    li->item_class=           LOG_LEX_STRING;
    li->key=                  log_bi->wellknown_get_name(
                               log_bi->wellknown_by_type(LOG_ITEM_LOG_MESSAGE));
    li->data.data_string.str= nullptr;
    LogEvent().type(LOG_TYPE_ERROR)
              .prio(INFORMATION_LEVEL)
              .message("item_inconsistent(#%d): %s",
                       4, consistent[-log_bi->item_inconsistent(li)]);

    // LOG_ITEM_OK
    li->type=                 LOG_ITEM_LOG_MESSAGE;
    li->item_class=           LOG_LEX_STRING;
    li->key=                  log_bi->wellknown_get_name(
                               log_bi->wellknown_by_type(LOG_ITEM_LOG_MESSAGE));
    li->data.data_string.str= "";
    li->data.data_string.length= 0;
    LogEvent().type(LOG_TYPE_ERROR)
              .prio(INFORMATION_LEVEL)
              .message("item_inconsistent(#%d): %s",
                       5, consistent[-log_bi->item_inconsistent(li)]);
  }
}


/**
  services: log sinks: basic structured dump writer

  This is intended for testing and debugging, not production.

  Writes all fields. No escaping is done. Submits various
  events of its own to demonstrate the availability of the error
  event submission interface from within external service, as well
  as the correct functioning of said interface.

  @param           instance             instance
  @param           ll                   the log line to write

  @retval          int                  number of accepted fields, if any
  @retval          <0                   failure
*/
DEFINE_METHOD(int, log_service_imp::run, (void *instance MY_ATTRIBUTE((unused)),
                                          log_line *ll))
{
  char                out_buff[LOG_BUFF_MAX];
  char               *out_writepos= out_buff;
  size_t              out_left= LOG_BUFF_MAX-1, // bytes left in output buffer
                      len;                  // length of current output particle
  int                 out_fields= 0,        // number of fields in output
                      wellknown_label;      // index of label in wellknowns[]
  enum loglevel       level= ERROR_LEVEL;   // default severity
  log_item_type       t= LOG_ITEM_END;
  log_item_type_mask  out_types= 0;
  log_item_iter      *it;
  log_item           *li;

  /*
    If we have detected some sort of massive failure (disk full, out of
    memory, etc.), we set the "failed" flag.  While this is set, any call
    to run() will immediately return.  As a result of this, we may call
    the error logger with information about this failure (AFTER first
    setting the failed flag to prevent a potential endless loop!) in case
    another log_sink is active that may show this alert.
  */
  if (failed)
    return -1;

  if ((it= log_bi->line_item_iter_acquire(ll)) == nullptr)
    return 0;

  li= log_bi->line_item_iter_first(it);

  while ((li != nullptr) && (out_left > 0))
  {
    t= li->type;

    if (log_bi->item_inconsistent(li))
    {
      len= log_bs->substitute(out_writepos, out_left,
                              "[%s=log_sink_test: broken item with class %d, "
                              "type %d];",
                              (li->key == nullptr) ? "_null" : li->key,
                              li->item_class, li->type);
      goto broken_item;
    }

    if (t == LOG_ITEM_LOG_PRIO)
    {
      level= static_cast<enum loglevel>(li->data.data_integer);
    }

    switch(li->item_class)
    {
    case LOG_LEX_STRING:
      if (li->data.data_string.str != nullptr)
        len= log_bs->substitute(out_writepos, out_left, "[%s=%.*s];",
                                li->key,
                                (int) li->data.data_string.length,
                                li->data.data_string.str);
      else
        len= 0;
      break;
    case LOG_INTEGER:
      len= log_bs->substitute(out_writepos, out_left, "[%s=%lld];",
                              li->key, li->data.data_integer);
      break;
    case LOG_FLOAT:
      len= log_bs->substitute(out_writepos, out_left, "[%s=%.12lf];",
                              li->key, li->data.data_float);
      break;

    default:
      goto unknown_item;
      break;
    }

    out_types|=    t;

  broken_item:
    out_fields++;
    out_left-=     len;
    out_writepos+= len;

  unknown_item:
    li= log_bi->line_item_iter_next(it);
  }

  if (out_fields > 0)
  {
    if (!(out_types & LOG_ITEM_LOG_LABEL) && (out_left > 0) &&
        (out_types & LOG_ITEM_LOG_PRIO))
    {
      const char *label= log_bi->label_from_prio(level);

      wellknown_label= log_bi->wellknown_by_type(LOG_ITEM_LOG_LABEL);
      len= log_bs->substitute(out_writepos, out_left, "[%s=%.*s];",
                              log_bi->wellknown_get_name((log_item_type) wellknown_label),
                              (int) log_bs->length(label), label);
      out_left-= len;
      out_fields++;
    }

    log_bi->write_errstream(nullptr,
                            out_buff, (size_t) LOG_BUFF_MAX - out_left);
  }

  /*
    Run some tests of the error logging system.
  */
  if (run_tests)
  {
    /*
      We'll be calling the logger below, so let's first
      prevent any more activations of these tests, otherwise,
      we might create an endless loop!
    */
    run_tests= false;

    // log a message from this here external service
    banner();

    // show that the rate-limiter actually works
    test_throttle();

    // show that adding key/value pairs actually works
    test_add_item();

    // get coverage for assorted built-ins
    test_builtins();

    /*
      There wasn't actually a failure; we're just testing
      the failure code: this disables this log_writer.
      Similar to "run_tests" above, if we had hit a real
      error and wanted to report on it using the error logger,
      we would need to set "failed" before calling the logger
      to prevent potential endless loops!
    */
    failed= true;
  }

  log_bi->line_item_iter_release(it);

  return out_fields;  // returning number of processed items
}


/**
  De-initialization method for Component used when unloading the Component.

  @return Status of performed operation
  @retval false success
  @retval true failure
*/
bool log_service_exit()
{
  if (inited)
  {
    log_service_release(log_bi);
    log_service_release(log_bs);
    log_service_release(log_bf);

    inited=     false;
    failed=     false;
    run_tests=  false;

    return false;
  }
  return true;
}


/**
  Initialization entry method for Component used when loading the Component.

  @return Status of performed operation
  @retval false success
  @retval true failure
*/
bool log_service_init()
{
  if (inited)
    return true;

  inited= true;
  failed= false;

  if (mysql_service_registry->acquire("log_builtins", &bls) ||
      ((log_bi= reinterpret_cast<SERVICE_TYPE(log_builtins)*>(bls)) ==
       nullptr) ||
      mysql_service_registry->acquire("log_builtins_string", &bls) ||
      ((log_bs= reinterpret_cast<SERVICE_TYPE(log_builtins_string)*>(bls)) ==
       nullptr) ||
      mysql_service_registry->acquire("log_builtins_filter", &bls) ||
      ((log_bf= reinterpret_cast<SERVICE_TYPE(log_builtins_filter)*>(bls)) ==
       nullptr))
  {
    log_service_exit();
    return true;
  }

  // run some examples/tests
  run_tests= true;

  return false;
}


/* flush logs */
DEFINE_METHOD(int, log_service_imp::flush,
              (void **instance MY_ATTRIBUTE((unused))))
{
  int res;

  if (inited)
    log_service_exit();

  res=       log_service_init();
  run_tests= false;

  return res;
}


/* implementing a service: log_service */
BEGIN_SERVICE_IMPLEMENTATION(log_sink_test, log_service)
  log_service_imp::run,
  log_service_imp::flush,
  nullptr,
  nullptr,
  log_service_imp::variable_check,
  log_service_imp::variable_update
END_SERVICE_IMPLEMENTATION()


/* component provides: just the log_service service, for now */
BEGIN_COMPONENT_PROVIDES(log_sink_test)
  PROVIDES_SERVICE(log_sink_test, log_service)
END_COMPONENT_PROVIDES()


/* component requires: n/a */
BEGIN_COMPONENT_REQUIRES(log_sink_test)
END_COMPONENT_REQUIRES()


/* component description */
BEGIN_COMPONENT_METADATA(log_sink_test)
  METADATA("mysql.author",  "Oracle Corporation")
  METADATA("mysql.license", "GPL")
  METADATA("log_service_type", "sink")
END_COMPONENT_METADATA()


/* component declaration */
DECLARE_COMPONENT(log_sink_test, "mysql:log_sink_test")
  log_service_init,
  log_service_exit
END_DECLARE_COMPONENT()


/* components contained in this library.
   for now assume that each library will have exactly one component. */
DECLARE_LIBRARY_COMPONENTS
  &COMPONENT_REF(log_sink_test)
END_DECLARE_LIBRARY_COMPONENTS

/* EOT */
