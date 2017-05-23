/* -*- c-basic-offset: 2 -*- */
/*
 * Copyright (C) 2017 Caliste Damien.
 * Contact: Damien Caliste <damien.caliste@cea.fr>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "Python.h"
#include "datetime.h"

#include <yaml.h>
#include <pcre.h>

#include <stdlib.h>

#if PY_MAJOR_VERSION < 3
#define PyLong_FromString(S, P, B) PyInt_FromString(S, P, B)
#define PyFloat_FromString(S) PyFloat_FromString(S, NULL)
#endif

#define YAML_TAG_MAP   "tag:yaml.org,2002:map"
#define YAML_TAG_SET   "tag:yaml.org,2002:set"
#define YAML_TAG_SEQ   "tag:yaml.org,2002:seq"
#define YAML_TAG_PAIRS "tag:yaml.org,2002:pairs"

#define YAML_TAG_NULL      "tag:yaml.org,2002:null"
#define YAML_TAG_BOOL      "tag:yaml.org,2002:bool"
#define YAML_TAG_TRUE      "tag:yaml.org,2002:true"
#define YAML_TAG_FALSE     "tag:yaml.org,2002:false"
#define YAML_TAG_INT       "tag:yaml.org,2002:int"
#define YAML_TAG_FLOAT     "tag:yaml.org,2002:float"
#define YAML_TAG_STR       "tag:yaml.org,2002:str"
#define YAML_TAG_TIMESTAMP "tag:yaml.org,2002:timestamp"
#define YAML_TAG_BINARY    "tag:yaml.org,2002:binary"

#define _has_tag(E, T) (E->data.scalar.tag && !strcmp(E->data.scalar.tag, T))

struct MinYamlBuilders;

typedef PyObject* (*MinYamlCConstructor)(struct MinYamlBuilders*, char*, size_t);

#define N_MAX_PATTERNS 20
#define OVECTOR_SIZE (N_MAX_PATTERNS * 3)
struct MinYamlBuilders
{
  char *tag;

  int ovector[OVECTOR_SIZE];
  int ncaptures;
  pcre *c_resolver;
  MinYamlCConstructor c_constructor;

  PyObject *py_match;
  PyObject *py_resolver;
  PyObject *py_constructor;
  
  struct MinYamlBuilders *next, *prev;
};

static PyObject *base64 = NULL;
static struct MinYamlBuilders *implicitBuilders = NULL;

static struct MinYamlBuilders* _new_c_builder(const char *tag,
                                              const char *regexp,
                                              MinYamlCConstructor c_constructor,
                                              struct MinYamlBuilders *sibling)
{
  struct MinYamlBuilders *builder;
  pcre *_regexp = NULL;
  const char *error;
  int offset;

  if (regexp)
    {
      _regexp = pcre_compile(regexp, 0, &error, &offset, NULL);
      if (!_regexp)
        {
          PyErr_SetString(PyExc_RuntimeError, error);
          return NULL;
        }
    }
  builder = malloc(sizeof(struct MinYamlBuilders));
  if (!builder)
    {
      PyErr_SetString(PyExc_MemoryError, "cannot create builder");
      return NULL;
    }
  builder->tag = strdup(tag);
  builder->c_resolver = _regexp;
  builder->c_constructor = c_constructor;
  Py_INCREF(Py_None);
  builder->py_match = Py_None;
  builder->py_resolver = NULL;
  builder->py_constructor = NULL;
  builder->prev = sibling;
  builder->next = NULL;
  if (sibling)
    sibling->next = builder;
  return builder;
}

static struct MinYamlBuilders* _new_py_builder(const char *tag,
                                               PyObject *py_resolver,
                                               PyObject *py_constructor,
                                               struct MinYamlBuilders *sibling)
{
  struct MinYamlBuilders *builder;

  builder = malloc(sizeof(struct MinYamlBuilders));
  if (!builder)
    {
      PyErr_SetString(PyExc_MemoryError, "cannot create builder");
      return NULL;
    }
  builder->tag = strdup(tag);
  builder->c_resolver = NULL;
  builder->c_constructor = NULL;
  Py_INCREF(Py_None);
  builder->py_match = Py_None;
  Py_XINCREF(py_resolver);
  builder->py_resolver = py_resolver;
  Py_XINCREF(py_constructor);
  builder->py_constructor = py_constructor;
  builder->next = NULL;
  if (sibling)
    sibling->next = builder;
  return builder;
}

static void _free_builders(struct MinYamlBuilders *builders)
{
  if (!builders)
    return;

  free(builders->tag);
  if (builders->c_resolver)
    pcre_free(builders->c_resolver);

  Py_XDECREF(builders->py_match);
  Py_XDECREF(builders->py_resolver);
  Py_XDECREF(builders->py_constructor);

  _free_builders(builders->next);

  free(builders);
}

static PyObject* _match_builder(struct MinYamlBuilders *builder,
                                  const char *value, size_t length)
{
  if (!builder)
    Py_RETURN_FALSE;

  if (builder->py_match != Py_None)
    {
      Py_XDECREF(builder->py_match);
      Py_INCREF(Py_None);
      builder->py_match = Py_None;
    }
  if (builder->py_resolver)
    {
      Py_XDECREF(builder->py_match);
      builder->py_match = PyObject_CallMethod(builder->py_resolver,
                                              "match", "s", value);
      if (!builder->py_match)
        return NULL;
      if (builder->py_match == Py_None)
        Py_RETURN_FALSE;
      else
        Py_RETURN_TRUE;
    }
  if (builder->c_resolver)
    {
      builder->ncaptures = pcre_exec(builder->c_resolver, NULL, value, length, 0, 0,
                                     builder->ovector, OVECTOR_SIZE);
      if (builder->ncaptures < 0)
        Py_RETURN_FALSE;
      else
        Py_RETURN_TRUE;
    }

  Py_RETURN_FALSE;
}
#define _action_if_match(B, V, L, A) {         \
    PyObject *valid = _match_builder(B, V, L); \
    if (!valid)                                \
      return NULL;                             \
    Py_DECREF(valid);                          \
    if (valid == Py_True)                      \
      { A; }                                   \
  }
#define _action_if_no_match(B, V, L, A) {      \
    PyObject *valid = _match_builder(B, V, L); \
    if (!valid)                                \
      return NULL;                             \
    Py_DECREF(valid);                          \
    if (valid == Py_False)                     \
      { A; }                                   \
  }

static void _remove_digit_separator(char *value, size_t length, char separator)
{
  size_t i, j;

  for (i = 0, j = 0; i < length && value[i]; i++)
    {
      value[j] = value[i];
      if (value[i] != separator)
        j += 1;
    }
  for (; j < i; j++)
    value[j] = '\0';
}

static char* _ovector_at(struct MinYamlBuilders *self, char *value, int i)
{
  if (i >= self->ncaptures)
    return NULL;

  return self->ovector[2 * i + 2] < self->ovector[2 * i + 3] ?
    value + self->ovector[2 * i + 2] : NULL;
}

static int _ovector_len_at(struct MinYamlBuilders *self, int i)
{
  if (i >= self->ncaptures)
    return 0;

  return self->ovector[2 * i + 3] - self->ovector[2 * i + 2];
}

static PyObject* _to_null(struct MinYamlBuilders *self, char *value, size_t length)
{
  Py_RETURN_NONE;
}

static PyObject* _to_bool(struct MinYamlBuilders *self, char *value, size_t length)
{
  if (!value)
    Py_RETURN_FALSE;
  
  if (!strcmp(value, "y") || !strcmp(value, "Y") ||
      !strcmp(value, "yes") || !strcmp(value, "Yes") || !strcmp(value, "YES") ||
      !strcmp(value, "true") || !strcmp(value, "True") || !strcmp(value, "TRUE") ||
      !strcmp(value, "on") || !strcmp(value, "On") || !strcmp(value, "ON"))
    Py_RETURN_TRUE;
  else if (!strcmp(value, "n") || !strcmp(value, "N") ||
           !strcmp(value, "no") || !strcmp(value, "No") || !strcmp(value, "NO") ||
           !strcmp(value, "false") || !strcmp(value, "False") || !strcmp(value, "FALSE") ||
           !strcmp(value, "off") || !strcmp(value, "Off") || !strcmp(value, "OFF"))
    Py_RETURN_FALSE;

  PyErr_SetString(PyExc_RuntimeError, "unknown bool value");
  return NULL;
}

static PyObject* _to_true(struct MinYamlBuilders *self, char *value, size_t length)
{
  Py_RETURN_TRUE;
}

static PyObject* _to_false(struct MinYamlBuilders *self, char *value, size_t length)
{
  Py_RETURN_FALSE;
}

static PyObject* _to_int(struct MinYamlBuilders *self, char *value, size_t length)
{
  char *cur;
  
  if (self->ncaptures < 0)
    _action_if_no_match(self, value, length, {
        PyErr_Format(PyExc_ValueError, "wrong int format '%s'", value);
        return NULL;
      });

  cur = _ovector_at(self, value, 0);
  if (cur)
    {
      _remove_digit_separator(cur, _ovector_len_at(self, 0), '_');
      return PyLong_FromString(cur, NULL, 8);
    }
  cur = _ovector_at(self, value, 1);
  if (cur)
    {
      long val;
      _remove_digit_separator(cur, _ovector_len_at(self, 1), '_');
      val = atol(cur);
      cur = _ovector_at(self, value, 2);
      while (cur)
        {
          val = val * 60. + atol(++cur);
          cur = strchr(cur, ':');
        }
      return PyLong_FromLong(val);
    }
  _remove_digit_separator((char*)value, length, '_');
  return PyLong_FromString((char*)value, NULL, 0);
}

static PyObject* _to_float(struct MinYamlBuilders *self, char *value, size_t length)
{
  PyObject *str, *val;
  char *cur;

  if (self->ncaptures < 0)
    _action_if_no_match(self, value, length, {
        _remove_digit_separator(value, length, '_');
        str = PyUnicode_FromString(value);
        if (!str)
          return NULL;
        val = PyFloat_FromString(str);
        Py_DECREF(str);
        return val;
      });

  if ((cur = _ovector_at(self, value, 0)))
    {
      double dval;
      _remove_digit_separator(cur, _ovector_len_at(self, 0), '_');
      dval = (double)atoi(cur);
      cur = _ovector_at(self, value, 1);
      while (cur)
        {
          dval = dval * 60. + (double)atoi(++cur);
          cur = strchr(cur, ':');
        }
      cur = _ovector_at(self, value, 2);
      _remove_digit_separator(cur, _ovector_len_at(self, 2), '_');
      dval += (cur) ? atof(cur) : 0.;
      return PyFloat_FromDouble(dval);
    }

  if (_ovector_at(self, value, 3))
    str = (value[0] == '-') ?
      PyUnicode_FromString("-inf") : PyUnicode_FromString("inf");
  else if (_ovector_at(self, value, 4))
    str = PyUnicode_FromString("nan");
  else
    {
      _remove_digit_separator(value, length, '_');
      str = PyUnicode_FromString(value);
    }
  if (!str)
    return NULL;
  val = PyFloat_FromString(str);
  Py_DECREF(str);
  return val;
}

static PyObject* _to_timestamp(struct MinYamlBuilders *self, char *value, size_t length)
{
  int year, month, day, hour, minute, second, usecond, tz_h, tz_m;

  const char *cur;

  if (self->ncaptures < 0)
    _action_if_no_match(self, value, length, {
        PyErr_Format(PyExc_ValueError, "wrong timestamp format '%s'", value);
        return NULL;
      });

  cur = _ovector_at(self, value, 0);
  if (cur)
    {
      year = atoi(cur);
      cur = _ovector_at(self, value, 1);
      month = atoi(cur);
      cur = _ovector_at(self, value, 2);
      day = atoi(cur);

      return PyDate_FromDate(year, month, day);
    }
  else
    {
      cur = _ovector_at(self, value, 3);
      year = atoi(cur);
      cur = _ovector_at(self, value, 4);
      month = atoi(cur);
      cur = _ovector_at(self, value, 5);
      day = atoi(cur);
    }

  cur = _ovector_at(self, value, 6);
  hour = atoi(cur);
  cur = _ovector_at(self, value, 7);
  minute = atoi(cur);
  cur = _ovector_at(self, value, 8);
  second = atoi(cur);
  cur = _ovector_at(self, value, 9);
  if (cur)
    {
      float fraction;
      fraction = atof(cur);
      usecond = (int)(fraction * 1000000);
    }
  else
    usecond = 0;
  cur = _ovector_at(self, value, 10);
  tz_h = (cur) ? atoi(cur) : 0;
  cur = _ovector_at(self, value, 11);
  tz_m = (cur) ? atoi(cur) : 0;

  minute -= tz_m;
  if (minute < 0)
    {
      hour -= 1;
      minute += 60;
    }
  if (minute > 60)
    {
      hour += 1;
      minute -= 60;
    }
  hour -= tz_h;
  if (hour < 0)
    {
      day -= 1;
      hour += 24;
    }
  if (hour > 24)
    {
      day += 1;
      hour -= 24;
    }

  return PyDateTime_FromDateAndTime(year, month, day, hour, minute, second, usecond);
}

static PyObject* _to_binary(struct MinYamlBuilders *self, char *value, size_t length)
{
#if PY_MAJOR_VERSION > 2
  return PyObject_CallMethod(base64, "decodebytes", "y#", value, length);
#else
  return PyObject_CallMethod(base64, "decodestring", "s#", value, length);
#endif
}

static PyObject* _to_str(struct MinYamlBuilders *self, char *value, size_t length)
{
#if PY_MAJOR_VERSION > 2
  return PyUnicode_FromString(value);
#else
  return PyString_FromString(value);
#endif
}

static int _init_implicit_builders(void)
{
  struct MinYamlBuilders *builders;
  
  implicitBuilders = builders = _new_c_builder(YAML_TAG_NULL,
                                             "^~$" /* (canonical) */
                                             "|^null$|^Null$|^NULL$" /* (English) */
                                             "|^$" /* (Empty) */, _to_null, NULL);
  if (!builders)
    return 0;
  builders = _new_c_builder(YAML_TAG_BOOL, NULL, _to_bool, builders);
  if (!builders)
    return 0;
  builders = _new_c_builder(YAML_TAG_TRUE,
                          "^(yes|Yes|YES"
                          "|true|True|TRUE"
                          "|on|On|ON)$", _to_true, builders);
  if (!builders)
    return 0;
  builders = _new_c_builder(YAML_TAG_FALSE,
                          "^(no|No|NO"
                          "|false|False|FALSE"
                          "|off|Off|OFF)$", _to_false, builders);
  if (!builders)
    return 0;
  builders = _new_c_builder(YAML_TAG_INT,
                          "^[-+]?0b[0-1_]+$" /* (base 2) */
                          "|^([-+]?0[0-7_]+)$" /* (base 8) */
                          "|^[-+]?(?:0|[1-9][0-9_]*)$" /* (base 10) */
                          "|^[-+]?0x[0-9a-fA-F_]+$" /* (base 16) */
                          "|^([-+]?[1-9][0-9_]*)((?::[0-5]?[0-9])+)$" /*  (base 60) */,
                          _to_int, builders);
  if (!builders)
    return 0;
  builders = _new_c_builder
    (YAML_TAG_FLOAT,
     "^[-+]?(?:[0-9][0-9_]*)?\\.[0-9_]*(?:[eE][-+][0-9]+)?$" /* (base 10) */
     "|^([-+]?[0-9][0-9_]*)((?::[0-5]?[0-9])+)(\\.[0-9_]*)$" /*  (base 60) */
     "|^([-+]?\\.inf|Inf|INF)$" /* (infinity) */
     "|^\\.(nan|NaN|NAN)$" /* (not a number) */, _to_float, builders);
  if (!builders)
    return 0;
  builders = _new_c_builder(YAML_TAG_TIMESTAMP,
                          "^([-+]?[0-9][0-9][0-9][0-9])-([0-9][0-9]?)-([0-9][0-9]?)$" /* (ymd) */
                          "|^([-+]?[0-9][0-9][0-9][0-9])" /* (year) */
                          "-([0-9][0-9]?)" /* (month) */
                          "-([0-9][0-9]?)" /* (day) */
                          "(?:[Tt]|[ \\t]+)([0-9][0-9]?)" /* (hour) */
                          ":([0-9][0-9])" /* (minute) */
                          ":([0-9][0-9])" /* (second) */
                          "(\\.[0-9]*)?" /* (fraction) */
                          "(?:(?:[ \\t]*)(?:Z|([-+][0-9][0-9]?)(?::([0-9][0-9]))?))?$" /* (time
                                                                           zone) */,
                          _to_timestamp, builders);
  if (!builders)
    return 0;
  builders = _new_c_builder(YAML_TAG_STR, NULL, _to_str, builders);
  if (!builders)
    return 0;
  builders = _new_c_builder(YAML_TAG_BINARY, NULL, _to_binary, builders);
  if (!builders)
    return 0;
  return 1;
}

enum MinYamlParserStatus
  {
    BLOCK_ERROR,
    BLOCK_PROCEED,
    BLOCK_DONE
  };
static enum MinYamlParserStatus _parse_next(yaml_parser_t *parser, yaml_event_t *event,
                                            yaml_event_type_t doneEvent)
{
  if (!yaml_parser_parse(parser, event))
    switch (parser->error)
      {
      case YAML_MEMORY_ERROR:
        PyErr_SetString(PyExc_MemoryError, "cannot parse");
        return BLOCK_ERROR;
      case YAML_READER_ERROR:
        if (parser->problem_value != -1)
          PyErr_Format(PyExc_SyntaxError, "%s: #%x at %ld", parser->problem,
                       parser->problem_value, parser->problem_offset);
        else
          PyErr_Format(PyExc_SyntaxError, "%s at %ld", parser->problem,
                       parser->problem_offset);
        return BLOCK_ERROR;
      case YAML_SCANNER_ERROR:
      case YAML_PARSER_ERROR:
        if (parser->context)
          PyErr_Format(PyExc_SyntaxError, "%s at line %ld, column %ld\n"
                       "%s at line %ld, column %ld", parser->context,
                       parser->context_mark.line+1, parser->context_mark.column+1,
                       parser->problem, parser->problem_mark.line+1,
                       parser->problem_mark.column+1);
        else
          PyErr_Format(PyExc_SyntaxError, "%s at line %ld, column %ld",
                       parser->problem, parser->problem_mark.line+1,
                       parser->problem_mark.column+1);
        return BLOCK_ERROR;
      default:
        PyErr_SetString(PyExc_RuntimeError, "Internal error");
        return BLOCK_ERROR;
      }
  
  return (event->type == doneEvent) ? BLOCK_DONE : BLOCK_PROCEED;
}

static struct MinYamlBuilders* _match_tag(struct MinYamlBuilders *builders,
                                          const char *tag)
{
  struct MinYamlBuilders *builder = NULL;

  if (tag)
    for (builder = builders;
         builder && strcmp(builder->tag, tag);
         builder = builder->next);
  return builder;
}

static PyObject* _build_scalar(yaml_event_t *event,
                               struct MinYamlBuilders *builders)
{
  struct MinYamlBuilders *builder = _match_tag(builders,
                                               (const char*)event->data.scalar.tag);
  if ((const char*)event->data.scalar.tag && !builder)
    {
      PyErr_Format(PyExc_TypeError, "No constructor for tag %s",
                   event->data.scalar.tag);
      return NULL;
    }

  if (!builder)
    for (builder = builders; builder; builder = builder->next)
      {
        _action_if_match(builder, (const char*)event->data.scalar.value,
                         event->data.scalar.length, break);
      }
  else
    builder->ncaptures = -1;

  if (builder && builder->py_constructor)
    return PyObject_CallFunction(builder->py_constructor,
                                 "sO", event->data.scalar.value, builder->py_match);
  else if (builder && builder->c_constructor)
    return builder->c_constructor(builder, (char*)event->data.scalar.value,
                                  event->data.scalar.length);

#if PY_MAJOR_VERSION > 2
  return PyUnicode_FromString((const char*)event->data.scalar.value);
#else
  return PyString_FromString((const char*)event->data.scalar.value);
#endif
}

static PyObject* _save_alias(PyObject *aliases, const char *anchor, PyObject *value)
{
  if (!anchor || !value)
    return value;

  if (PyDict_SetItemString(aliases, anchor, value) == -1)
    {
      Py_DECREF(value);
      return NULL;
    }
  return value;
}

static PyObject* _load_alias(PyObject *aliases, const char *anchor)
{
  PyObject *value;
  
  value = PyDict_GetItemString(aliases, anchor);
  if (!value)
    {
      PyErr_Format(PyExc_KeyError, "unknown alias '%s'", anchor);
      return NULL;
    }
  Py_INCREF(value);
  return value;
}

static PyObject* _build_seq(yaml_parser_t *parser,
                            struct MinYamlBuilders *builders,
                            PyObject *aliases);
static PyObject* _build_pairs(yaml_parser_t *parser,
                              struct MinYamlBuilders *builders,
                              PyObject *aliases);
static PyObject* _build_map(yaml_parser_t *parser,
                            struct MinYamlBuilders *builders,
                            PyObject *aliases);
static PyObject* _build_custom(yaml_parser_t *parser, const char *tag,
                               struct MinYamlBuilders *builders,
                               PyObject *source);

static PyObject* _build_value(yaml_parser_t *parser, yaml_event_t *event,
                              struct MinYamlBuilders *builders,
                              PyObject *aliases)
{
  switch (event->type)
    {
    case YAML_MAPPING_START_EVENT:
      if (!event->data.mapping_start.tag ||
          !strcmp((const char*)event->data.mapping_start.tag, YAML_TAG_MAP))
        return _save_alias(aliases, (const char*)event->data.mapping_start.anchor,
                           _build_map(parser, builders, aliases));
      else if (event->data.mapping_start.tag &&
               !strcmp((const char*)event->data.mapping_start.tag, YAML_TAG_SET))
        return _save_alias(aliases, (const char*)event->data.mapping_start.anchor,
                           _build_map(parser, builders, aliases));
      else
        return _save_alias(aliases, (const char*)event->data.mapping_start.anchor,
                           _build_custom(parser, (const char*)event->data.mapping_start.tag,
                                         builders, _build_map(parser, builders, aliases)));
    case YAML_SEQUENCE_START_EVENT:
      if (!event->data.sequence_start.tag ||
          !strcmp((const char*)event->data.sequence_start.tag, YAML_TAG_SEQ))
        return _save_alias(aliases, (const char*)event->data.sequence_start.anchor,
                           _build_seq(parser, builders, aliases));
      else if (event->data.sequence_start.tag &&
               !strcmp((const char*)event->data.sequence_start.tag, YAML_TAG_PAIRS))
        return _save_alias(aliases, (const char*)event->data.sequence_start.anchor,
                           _build_pairs(parser, builders, aliases));
      else
        return _save_alias(aliases, (const char*)event->data.sequence_start.anchor,
                           _build_custom(parser, (const char*)event->data.sequence_start.tag,
                                         builders, _build_seq(parser, builders, aliases)));
    case YAML_SCALAR_EVENT:
      return _save_alias(aliases, (const char*)event->data.scalar.anchor,
                         _build_scalar(event, builders));
    case YAML_ALIAS_EVENT:
      return _load_alias(aliases, (const char*)event->data.alias.anchor);
    default:
      PyErr_SetString(PyExc_SyntaxError, "collection, scalar or alias event awaited");
      return NULL;
    }
}

static PyObject* _build_custom(yaml_parser_t *parser, const char *tag,
                               struct MinYamlBuilders *builders,
                               PyObject *source)
{
  struct MinYamlBuilders *builder = _match_tag(builders, tag);
  
  if (!builder || !builder->py_constructor)
    {
      PyErr_Format(PyExc_ValueError, "unknown collection tag '%s'", tag);
      return NULL;
    }
  return PyObject_CallFunction(builder->py_constructor, "O", source);
}

static PyObject* _build_seq(yaml_parser_t *parser,
                            struct MinYamlBuilders *builders,
                            PyObject *aliases)
{
  PyObject *sequence;
  yaml_event_t event;
  enum MinYamlParserStatus status;

  sequence = PyList_New(0);
  if (!sequence)
    return NULL;
  do
    {
      status = _parse_next(parser, &event, YAML_SEQUENCE_END_EVENT);
      if (status == BLOCK_ERROR)
        goto error;
      
      if (status == BLOCK_PROCEED)
        {
          PyObject *value;
              
          value = _build_value(parser, &event, builders, aliases);
          if (!value)
            {
              yaml_event_delete(&event);
              goto error;
            }
          if (PyList_Append(sequence, value) == -1)
            {
              Py_DECREF(value);
              yaml_event_delete(&event);
              goto error;
            }
          Py_DECREF(value);
        }
      
      yaml_event_delete(&event);
    }
  while (status != BLOCK_DONE);

  return sequence;

 error:
  Py_DECREF(sequence);
  return NULL;
}

static PyObject* _build_pairs(yaml_parser_t *parser,
                              struct MinYamlBuilders *builders,
                              PyObject *aliases)
{
  PyObject *sequence, *key = NULL;
  yaml_event_t event;
  enum MinYamlParserStatus status;

  sequence = PyList_New(0);
  if (!sequence)
    return NULL;
  do
    {
      status = _parse_next(parser, &event, YAML_SEQUENCE_END_EVENT);
      if (status == BLOCK_ERROR)
        goto error;

      if (status == BLOCK_PROCEED)
        {
          PyObject *value = NULL;
              
          if (!key)
            {
              if (event.type != YAML_MAPPING_START_EVENT)                   
                {
                  PyErr_SetString(PyExc_SyntaxError, "awaited opening pair");
                  yaml_event_delete(&event);
                  goto error;
                }
              yaml_event_delete(&event);
              status = _parse_next(parser, &event, YAML_SEQUENCE_END_EVENT);
              if (status == BLOCK_ERROR)
                goto error;
              if (status == BLOCK_PROCEED)
                {
                  key = _build_value(parser, &event, builders, aliases);
                  if (!key)
                    {
                      yaml_event_delete(&event);
                      goto error;
                    }
                }
            }
          else
            {
              value = _build_value(parser, &event, builders, aliases);
              if (!value)
                {
                  yaml_event_delete(&event);
                  goto error;
                }
            }
          if (key && value)
            {
              PyObject *pair;

              pair = PyTuple_New(2);
              if (!pair)
                {
                  Py_DECREF(value);
                  yaml_event_delete(&event);
                  goto error;
                }
              if (PyTuple_SetItem(pair, 0, key) != 0)
                {
                  Py_DECREF(pair);
                  Py_DECREF(value);
                  yaml_event_delete(&event);
                  goto error;
                }
              key = NULL;
              if (PyTuple_SetItem(pair, 1, value) != 0)
                {
                  Py_DECREF(pair);
                  Py_DECREF(value);
                  yaml_event_delete(&event);
                  goto error;
                }
              if (PyList_Append(sequence, pair) == -1)
                {
                  Py_DECREF(pair);
                  yaml_event_delete(&event);
                  goto error;
                }
              Py_DECREF(pair);

              yaml_event_delete(&event);
              status = _parse_next(parser, &event, YAML_SEQUENCE_END_EVENT);
              if (status == BLOCK_ERROR)
                goto error;
              if (event.type != YAML_MAPPING_END_EVENT)                   
                {
                  PyErr_SetString(PyExc_SyntaxError, "awaited close pair");
                  yaml_event_delete(&event);
                  goto error;
                }
            }
        }

      yaml_event_delete(&event);
    }
  while (status != BLOCK_DONE);

  return sequence;

 error:
  Py_XDECREF(key);
  Py_DECREF(sequence);
  return NULL;
}

static int _is_key_merge(PyObject *key)
{
#if PY_MAJOR_VERSION > 2
  const char *value = PyUnicode_Check(key) ? PyUnicode_AsUTF8(key) : NULL;
#else
  const char *value = PyString_Check(key) ? PyString_AS_STRING(key) : NULL;
#endif
  return (value && value[0] == '<' && value[1] == '<' && value[2] == '\0');
}

static PyObject* _build_map(yaml_parser_t *parser,
                            struct MinYamlBuilders *builders,
                            PyObject *aliases)
{
  yaml_event_t event;
  PyObject *mapping;
  PyObject *key = NULL;
  enum MinYamlParserStatus status;
  int isSet = 1;

  mapping = PyDict_New();
  do
    {
      status = _parse_next(parser, &event, YAML_MAPPING_END_EVENT);
      if (status == BLOCK_ERROR)
        goto error;
      if (status == BLOCK_PROCEED)
        {
          PyObject *value = NULL;

          if (!key)
            {
              if (!(key = _build_value(parser, &event, builders, aliases)))
                {
                  yaml_event_delete(&event);
                  goto error;
                }
            }
          else
            {
              if (!(value = _build_value(parser, &event, builders, aliases)))
                {
                  yaml_event_delete(&event);
                  goto error;
                }
            }
          if (key && value)
            {
              if (_is_key_merge(key))
                {
                  if (PyList_Check(value))
                    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(value); i++)
                      {
                        if (PyDict_Merge(mapping, PyList_GET_ITEM(value, i), 0) == -1)
                          {
                            Py_DECREF(value);
                            yaml_event_delete(&event);
                            goto error;
                          }
                      }
                  else if (PyDict_Check(value))
                    {
                      if (PyDict_Merge(mapping, value, 0) == -1)
                        {
                          Py_DECREF(value);
                          yaml_event_delete(&event);
                          goto error;
                        }
                    }
                  else
                    {
                      PyErr_SetString(PyExc_SyntaxError, "dictionary or list awaited");
                      Py_DECREF(value);
                      yaml_event_delete(&event);
                      goto error;
                    }
                }
              else
                {
                  if (PyDict_SetItem(mapping, key, value) == -1)
                    {
                      Py_DECREF(value);
                      yaml_event_delete(&event);
                      goto error;
                    }
                }
              Py_DECREF(key);
              Py_DECREF(value);
              key = NULL;
              isSet = isSet && value == Py_None;
            }
        }        

      yaml_event_delete(&event);
    }
  while (status != BLOCK_DONE);

  if (isSet)
    {
      PyObject *set = PySet_New(mapping);
      Py_DECREF(mapping);
      return set;
    }
  else
    return mapping;

 error:
  Py_XDECREF(key);
  Py_DECREF(mapping);
  return NULL;
}

static PyObject* _build_document(yaml_parser_t *parser,
                                 struct MinYamlBuilders *builders)
{
  PyObject *document = NULL;
  yaml_event_t event;
  enum MinYamlParserStatus status;

  status = _parse_next(parser, &event, YAML_DOCUMENT_END_EVENT);
  if (status == BLOCK_ERROR)
    return NULL;

  if (status == BLOCK_PROCEED)
    {
      PyObject *aliases = PyDict_New();
      document = _build_value(parser, &event, builders, aliases);
      Py_DECREF(aliases);
      if (!document)
        {
          yaml_event_delete(&event);
          return NULL;
        }
    }
  yaml_event_delete(&event);

  status = _parse_next(parser, &event, YAML_DOCUMENT_END_EVENT);
  if (status == BLOCK_ERROR)
    return NULL;
  yaml_event_delete(&event);
  if (status != BLOCK_DONE)
    {
      Py_DECREF(document);
      PyErr_SetString(PyExc_SyntaxError, "awaited end of document");
      return NULL;
    }
  
  if (document)
    return document;
  else
    Py_RETURN_NONE;
}

typedef struct
{
  PyObject_HEAD
  PyObject *stream;
  yaml_parser_t parser;
} MinYamlLoader;

static PyObject* _min_yaml_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  MinYamlLoader *self;

  self = (MinYamlLoader*)type->tp_alloc(type, 0);
  if (self != NULL) {
    self->stream = NULL;
    yaml_parser_initialize(&self->parser);
  }

  return (PyObject*)self;
}

static int _input_handler(void *data, unsigned char *buffer, size_t size,
                          size_t *size_read)
{
  PyObject *rbuf;
  Py_ssize_t ln;
  char *rbuffer;

  *size_read = 0;
  rbuf = PyObject_CallMethod((PyObject*)data, "read", "i", (int)size);
  if (!rbuf)
    return 0;

#if PY_MAJOR_VERSION > 2
  rbuffer = PyUnicode_AsUTF8AndSize(rbuf, &ln);
#else
  PyString_AsStringAndSize(rbuf, &rbuffer, &ln);
#endif
  if (!rbuffer)
    return 0;
  
  memcpy(buffer, rbuffer, ln);
  *size_read = ln;

  Py_DECREF(rbuf);

  return 1;
}

static int _min_yaml_init(MinYamlLoader *self, PyObject *args, PyObject *kwds)
{
  if (!PyArg_ParseTuple(args, "O", &self->stream))
    return -1;

  /* Detect if read method is available. */
  if (PyObject_HasAttrString(self->stream, "read"))
    {
      Py_INCREF(self->stream);
      yaml_parser_set_input(&self->parser, _input_handler, self->stream);
    }
#if PY_MAJOR_VERSION > 2
  else if (PyUnicode_Check(self->stream))
    {
      Py_ssize_t ln;
      yaml_parser_set_input_string
        (&self->parser, (unsigned char*)PyUnicode_AsUTF8AndSize(self->stream, &ln),
         (size_t)ln);
    }
#else
  else if (PyString_Check(self->stream))
    yaml_parser_set_input_string
      (&self->parser, (unsigned char*)PyString_AS_STRING(self->stream),
       (size_t)PyString_Size(self->stream));
#endif
  else
    {
      PyErr_SetString(PyExc_TypeError, "wrong type for stream argument");
      return -1;
    }

  return 0;
}

static void _min_yaml_dealloc(MinYamlLoader* self)
{
  yaml_parser_delete(&self->parser);
  Py_XDECREF(self->stream);
}

static PyObject* _min_yaml_check_data(MinYamlLoader* self)
{
  yaml_event_t event;
  enum MinYamlParserStatus status;
  
  status = _parse_next(&self->parser, &event, YAML_STREAM_END_EVENT);
  if (status == BLOCK_ERROR)
    return NULL;
  if (event.type == YAML_STREAM_START_EVENT)
    {
      yaml_event_delete(&event);  
      status = _parse_next(&self->parser, &event, YAML_STREAM_END_EVENT);
      if (status == BLOCK_ERROR)
        return NULL;
    }
  if (event.type == YAML_DOCUMENT_START_EVENT)
    {
      yaml_event_delete(&event);
      Py_RETURN_TRUE;
    }
  yaml_event_delete(&event);  
  if (status != BLOCK_DONE)
    {
      PyErr_SetString(PyExc_SyntaxError, "awaited document start");
      return NULL;
    }
  Py_RETURN_FALSE;
}

static PyObject* _min_yaml_get_data(MinYamlLoader* self)
{
#if PY_MAJOR_VERSION > 2
  return _build_document(&self->parser, implicitBuilders);
#else
  PyObject *doc;
  
  if (!_init_implicit_builders())
    {
      _free_builders(implicitBuilders);
      return NULL;
    }
  doc = _build_document(&self->parser, implicitBuilders);
  _free_builders(implicitBuilders);
  return doc;
#endif
}

static PyObject* _min_yaml_single_data(MinYamlLoader* self)
{
  PyObject *valid;
  PyObject *doc = NULL;

  valid = _min_yaml_check_data(self);
  if (!valid)
    return NULL;

  Py_DECREF(valid);
  if (valid == Py_True)
    {
      doc = _build_document(&self->parser, implicitBuilders);
      if (!doc)
        return NULL;
    }

  valid = _min_yaml_check_data(self);
  if (!valid)
    {
      Py_DECREF(doc);
      return NULL;
    }
  Py_DECREF(valid);
  if (valid == Py_True)
    {
      PyErr_SetString(PyExc_RuntimeError, "expected a single document in the stream");
      Py_XDECREF(doc);
      return NULL;
    }
  if (doc)
    return doc;
  else
    Py_RETURN_NONE;
}

static PyObject* _min_yaml_dispose(MinYamlLoader* self)
{
  Py_RETURN_NONE;
}

static PyObject* _add_resolver_constructor(const char *tag,
                                           PyObject *resolver, PyObject *constructor)
{
  struct MinYamlBuilders *builder, *pbuilder;

  for (builder = implicitBuilders; builder; builder = builder->next)
    {
      if (!strcmp(builder->tag, tag))
        {
          if (constructor)
            {
              Py_XDECREF(builder->py_constructor);
              Py_INCREF(constructor);
              builder->py_constructor = constructor;
            }
          if (resolver)
            {
              Py_XDECREF(builder->py_resolver);
              Py_INCREF(resolver);
              builder->py_resolver = resolver;
            }
          Py_RETURN_NONE;
        }
    }
  if (builder)
    {
      pbuilder = _new_py_builder(tag, resolver, constructor, builder->prev);
      if (!pbuilder)
        return NULL;
      pbuilder->next = builder;
      builder->prev = pbuilder;
      if (builder == implicitBuilders)
        implicitBuilders = pbuilder;
    }
  else
    {
      for (builder = implicitBuilders; builder->next; builder = builder->next);
      pbuilder = _new_py_builder(tag, resolver, constructor, builder);
    }
  
  Py_RETURN_NONE;
}
 
static PyObject* _min_yaml_add_constructor(PyObject *cls, PyObject *args)
{
  const char *tag;
  PyObject *constructor;
  
  if (!PyArg_ParseTuple(args, "sO", &tag, &constructor))
    return NULL;

  if (!PyCallable_Check(constructor))
    {
      PyErr_SetString(PyExc_TypeError, "constructor argument is not callable");
      return NULL;
    }

  return _add_resolver_constructor(tag, NULL, constructor);
}
 
static PyObject* _min_yaml_add_resolver(PyObject *cls, PyObject *args)
{
  const char *tag;
  PyObject *resolver, *notused;
  
  if (!PyArg_ParseTuple(args, "sOO", &tag, &resolver, &notused))
    return NULL;

  if (!PyObject_HasAttrString(resolver, "match"))
    {
      PyErr_SetString(PyExc_TypeError, "resolver argument has no match() routine");
      return NULL;
    }

  return _add_resolver_constructor(tag, resolver, NULL);
}
 
static PyMethodDef MinYamlLoaderMethods[] = {
  {"get_single_data", (PyCFunction)_min_yaml_single_data, METH_NOARGS,
   "Ensure that the stream contains a single document and construct it."},
  {"check_data", (PyCFunction)_min_yaml_check_data, METH_NOARGS,
   "If there are more documents available?"},
  {"get_data", (PyCFunction)_min_yaml_get_data, METH_NOARGS,
   "construct and return the next document."},
  {"dispose", (PyCFunction)_min_yaml_dispose, METH_NOARGS,
   "do nothing."},
  {"add_constructor", (PyCFunction)_min_yaml_add_constructor, METH_VARARGS | METH_CLASS,
   "add a constructor for the given tag."},
  {"add_implicit_resolver", (PyCFunction)_min_yaml_add_resolver, METH_VARARGS | METH_CLASS,
   "add a resolver for the given tag."},
  {NULL, NULL, 0, NULL}
};

static PyTypeObject MinYamlLoaderType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "minYaml.MinLoader",       /* tp_name */
    sizeof(MinYamlLoader),     /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)_min_yaml_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "Min YAML loaders",        /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    MinYamlLoaderMethods,      /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)_min_yaml_init,  /* tp_init */
    0,                         /* tp_alloc */
    _min_yaml_new,             /* tp_new */
};

#if PY_MAJOR_VERSION >= 3
static void _free_minYaml(void *m)
{
  _free_builders(implicitBuilders);
  Py_XDECREF(base64);
}

static struct PyModuleDef moduledef = {
  PyModuleDef_HEAD_INIT,
  "_minyaml",     /* m_name */
  "minimal YAML parser",  /* m_doc */
  -1,                  /* m_size */
  NULL,                /* m_methods */
  NULL,                /* m_reload */
  NULL,                /* m_traverse */
  NULL,                /* m_clear */
  _free_minYaml,       /* m_free */
};
#endif

#if PY_MAJOR_VERSION == 2
PyMODINIT_FUNC
init_minyaml(void)
{
  PyObject *m;

  if (PyType_Ready(&MinYamlLoaderType) < 0)
    return;

  base64 = PyImport_ImportModule("base64");
  if (!base64)
    return;

  PyDateTime_IMPORT;

  m = Py_InitModule("_minyaml", NULL);
  if (m == NULL)
    return;

  Py_INCREF(&MinYamlLoaderType);
  PyModule_AddObject(m, "MinLoader", (PyObject*)&MinYamlLoaderType);
}
#else
PyMODINIT_FUNC PyInit__minyaml(void)
{
  PyObject *m;

  if (PyType_Ready(&MinYamlLoaderType) < 0)
    return NULL;

  if (!_init_implicit_builders())
    {
      _free_builders(implicitBuilders);
      return NULL;
    }

  base64 = PyImport_ImportModule("base64");
  if (!base64)
    return NULL;

  PyDateTime_IMPORT;

  m = PyModule_Create(&moduledef);
  if (m == NULL)
    return NULL;

  Py_INCREF(&MinYamlLoaderType);
  PyModule_AddObject(m, "MinLoader", (PyObject*)&MinYamlLoaderType);

  return m;
}
#endif
