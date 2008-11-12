/*
  Cursor handling code

  Copyright (C) 2004-2008 Roger Binns <rogerb@rogerbinns.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any
  damages arising from the use of this software.
 
  Permission is granted to anyone to use this software for any
  purpose, including commercial applications, and to alter it and
  redistribute it freely, subject to the following restrictions:
 
  1. The origin of this software must not be misrepresented; you must
     not claim that you wrote the original software. If you use this
     software in a product, an acknowledgment in the product
     documentation would be appreciated but is not required.

  2. Altered source versions must be plainly marked as such, and must
     not be misrepresented as being the original software.

  3. This notice may not be removed or altered from any source
     distribution.
 
*/

/** 

Cursors (executing SQL)
***********************

A cursor encapsulates a SQL query and returning results.  To make a
new cursor you should call :meth:`~Connection.cursor` on your
database::

  db=apsw.Connection("databasefilename")
  cursor=db.cursor()

A cursor executes SQL::

  cursor.execute("create table example(title, isbn)")

You can also read data back.  The row is returned as a tuple of the
column values::
 
  for row in cursor.execute("select * from example"):
     print row

There are two ways of supplying data to a query.  The **really bad** way is to compose a string::

  sql="insert into example values('%s', %s)" % ("string", "8390823904")
  cursor.execute(sql)

If there were any single quotes in string then you would have invalid
syntax.  Additionally this is how `SQL injection attacks
<http://en.wikipedia.org/wiki/SQL_injection>`_ happen. Instead you should use bindings::

  sql="insert into example values(?, ?)"
  cursor.execute(sql, ("string", "8390823904"))

  # You can also use dictionaries
  sql="insert into example values(:title, :isbn)"
  cursor.execute(sql, {"title": "string", "isbn": "8390823904"})

  # You can use local variables as the dictionary
  title="..."
  isbn="...."
  cursor.execute(sql, locals())

Cursors are cheap.  Use as many as you need.  It is safe to use them
across threads, such as calling :meth:`~Cursor.execute` in one thread,
passing the cursor to another thread that then calls
:meth:`Cursor.next`.  The only thing you can't do is call methods at
exactly the same time on the same cursor in two different threads - eg
trying to call :meth:`~Cursor.execute` in both at the same time, or
:meth:`~Cursor.execute` in one and :meth:`Cursor.next` in another.
(If you do attempt this, it will be detected and
:exc:`ThreadingViolationError` will be raised.)  

Behind the scenes a :class:`Cursor` maps to a `SQLite statement
<http://www.sqlite.org/c3ref/stmt.html>`_.  APSW maintains a
:ref:`cache <statementcache>` so that the mapping is very fast, and the
SQLite objects are reused when possible.

A unique feature of APSW is that your query can be multiple semi-colon
separated statements.  For example::

  cursor.execute("select ... ; insert into ... ; update ... ; select ...")

.. note::

  SQLite fetches data as it is needed.  If table `example` had 10
  million rows it would only get the next row as requested (the for
  loop effectively calls :meth:`~Cursor.next` to get each row).  This
  code would not work as expected::

    for row in cursor.execute("select * from example"):
       cursor.execute("insert .....")

  The nested :meth:`~Cursor.execute` would start a new query
  abandoning any remaining results from the ``SELECT``.  There are two
  ways to work around this.  Use a different cursor::

    for row in cursor1.execute("select * from example"):
       cursor2.execute("insert ...")

  You can also get all the rows immediately by filling in a list::

    rows=list( cursor.execute("select * from example") )
    for row in rows:
       cursor.execute("insert ...")

  This last approach is recommended since you don't have to worry
  about the database changing while doing the ``select``.  You should
  also understand transactions and where to put the transaction
  boundaries.

.. seealso::

  * `SQLite transactions <http://www.sqlite.org/lang_transaction.html>`_
  * `Atomic commit <http://www.sqlite.org/atomiccommit.html>`_
  * :ref:`Benchmarking`

*/


/** .. class:: Cursor

  You obtain cursors by calling :meth:`Connection.cursor`.
*/

/* CURSOR TYPE */

struct APSWCursor {
  PyObject_HEAD
  Connection *connection;          /* pointer to parent connection */

  unsigned inuse;                  /* track if we are in use preventing concurrent thread mangling */
  struct APSWStatement *statement; /* statement we are currently using */

  /* what state we are in */
  enum { C_BEGIN, C_ROW, C_DONE } status;

  /* bindings for query */
  PyObject *bindings;              /* dict or sequence */
  Py_ssize_t bindingsoffset;       /* for sequence tracks how far along we are when dealing with multiple statements */

  /* iterator for executemany, original query string */
  PyObject *emiter;
  PyObject *emoriginalquery;

  /* tracing functions */
  PyObject *exectrace;
  PyObject *rowtrace;
  
};

typedef struct APSWCursor APSWCursor;
static PyTypeObject APSWCursorType;

/* CURSOR CODE */

/* Do finalization and free resources.  Returns the SQLITE error code */
static int
resetcursor(APSWCursor *self, int force)
{
  int res=SQLITE_OK;
  PyObject *nextquery=self->statement?self->statement->next:NULL;
  PyObject *etype, *eval, *etb;

  if(force)
    PyErr_Fetch(&etype, &eval, &etb);

  if(nextquery) 
    Py_INCREF(nextquery);

  Py_XDECREF(self->bindings);
  self->bindings=NULL;
  self->bindingsoffset= -1;

  if(self->statement)
    {
      INUSE_CALL(res=statementcache_finalize(self->connection->stmtcache, self->statement));
      if(!force) /* we don't care about errors when forcing */
	SET_EXC(res, self->connection->db);
      self->statement=0;
    }

  if(!force && self->status!=C_DONE && nextquery)
    {
      if (res==SQLITE_OK)
        {
          /* We still have more, so this is actually an abort. */
          res=SQLITE_ERROR;
          if(!PyErr_Occurred())
            {
              PyErr_Format(ExcIncomplete, "Error: there are still remaining sql statements to execute");
              AddTraceBackHere(__FILE__, __LINE__, "resetcursor", "{s: N}", "remaining", convertutf8buffertounicode(nextquery));
            }
        }
    }

  Py_XDECREF(nextquery);
  
  if(!force && self->status!=C_DONE && self->emiter)
    {
      PyObject *next=PyIter_Next(self->emiter);
      if(next)
        {
          Py_DECREF(next);
          res=SQLITE_ERROR;
          if (!PyErr_Occurred())
	    /* Technically this line won't get executed since the
	       block above will already have set ExcIncomplete.
	       Leaving it in as defensive coding. */
            PyErr_Format(ExcIncomplete, "Error: there are still many remaining sql statements to execute");
        }
    }
     
  Py_XDECREF(self->emiter);
  self->emiter=NULL;
  Py_XDECREF(self->emoriginalquery);
  self->emoriginalquery=NULL;

  self->status=C_DONE;

  if (PyErr_Occurred())
    {
      assert(res);
      AddTraceBackHere(__FILE__, __LINE__, "resetcursor", "{s: i}", "res", res);
    }

  if(force && (etype || eval || etb))
    PyErr_Restore(etype, eval, etb);

  return res;
}

static void
APSWCursor_dealloc(APSWCursor * self)
{
  PyObject *err_type, *err_value, *err_traceback;
  int have_error=PyErr_Occurred()?1:0;

  /* do our finalisation ... */

  if (have_error)
    {
      /* remember the existing error so that resetcursor won't immediately return */
      PyErr_Fetch(&err_type, &err_value, &err_traceback);
    }

  resetcursor(self, /* force = */ 1);
  assert(!PyErr_Occurred());

  if (have_error)
    /* restore earlier error if there was one */
    PyErr_Restore(err_type, err_value, err_traceback);

  /* we no longer need connection */
  if(self->connection)
    {
      pointerlist_remove(&self->connection->dependents, self);
      Py_DECREF(self->connection);
      self->connection=0;
    }

  /* executemany iterator */
  Py_XDECREF(self->emiter);
  self->emiter=NULL;

  /* no need for tracing */
  Py_XDECREF(self->exectrace);
  Py_XDECREF(self->rowtrace);
  self->exectrace=self->rowtrace=0;
  
  Py_TYPE(self)->tp_free((PyObject*)self);
}

static void
APSWCursor_init(APSWCursor *self, Connection *connection)
{
  self->connection=connection;
  self->statement=0;
  self->status=C_DONE;
  self->bindings=0;
  self->bindingsoffset=0;
  self->emiter=0;
  self->emoriginalquery=0;
  self->exectrace=0;
  self->rowtrace=0;
  self->inuse=0;
}

/** .. method:: getdescription() -> list

   Returns a list describing each column in the result row.  The
   return is identical for every row of the results.  You can only
   call this method once you have started executing a statement and
   before you have finished::

      # This will error
      cursor.getdescription()

      for row in cursor.execute("select ....."):
         # this works
         print cursor.getdescription()
         print row

   The information about each column is a tuple of ``(column_name,
   declared_column_type)``.  The type is what was declared in the
   ``CREATE TABLE`` statement - the value returned in the row will be
   whatever type you put in for that row and column.  (This is known
   as `manifest typing <http://www.sqlite.org/different.html#typing>`_
   which is also the way that Python works.  The variable ``a`` could
   contain an integer, and then you could put a string in it.  Other
   static languages such as C or other SQL databases only let you put
   one type in - eg ``a`` could only every contain integer or string,
   but never both.)

   Example::

      cursor.execute("create table books(title string, isbn number, wibbly wobbly zebra)")
      cursor.execute("insert into books values(?,?,?)", (97, "fjfjfj", 3.7))
      cursor.execute("insert into books values(?,?,?)", ("fjfjfj", 3.7, 97))

      for row in cursor.execute("select * from books"):
         print cursor.getdescription()
         print row
 
   Output::
 
     # row 0 - description
     (('title', 'string'), ('isbn', 'number'), ('wibbly', 'wobbly zebra'))
     # row 0 - values
     (97, 'fjfjfj', 3.7)
     # row 1 - description
     (('title', 'string'), ('isbn', 'number'), ('wibbly', 'wobbly zebra'))
     # row 1 - values
     ('fjfjfj', 3.7, 97)

   -* sqlite3_column_name sqlite3_column_decltype

*/

static PyObject *
APSWCursor_getdescription(APSWCursor *self)
{
  int ncols,i;
  PyObject *result=NULL;
  PyObject *pair=NULL;

  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection,NULL);

  if(!self->statement)
    {
      PyErr_Format(ExcComplete, "Can't get description for statements that have completed execution");
      return NULL;
    }
  
  ncols=sqlite3_column_count(self->statement->vdbestatement);
  result=PyTuple_New(ncols);
  if(!result) goto error;

  for(i=0;i<ncols;i++)
    {
      const char *colname;
      const char *coldesc;

      PYSQLITE_VOID_CALL( (colname=sqlite3_column_name(self->statement->vdbestatement, i), coldesc=sqlite3_column_decltype(self->statement->vdbestatement, i)) );
      APSW_FAULT_INJECT(GetDescriptionFail,
      pair=Py_BuildValue("(O&O&)", 
			 convertutf8string, colname,
			 convertutf8string, coldesc),
      pair=PyErr_NoMemory()
      );                  
			 
      if(!pair) goto error;

      PyTuple_SET_ITEM(result, i, pair);
      /* owned by result now */
      pair=0;
    }
  
  return result;

 error:
  Py_XDECREF(result);
  Py_XDECREF(pair);
  return NULL;
}

/* internal function - returns SQLite error code (ie SQLITE_OK if all is well) */
static int
APSWCursor_dobinding(APSWCursor *self, int arg, PyObject *obj)
{

  /* DUPLICATE(ish) code: this is substantially similar to the code in
     set_context_result.  If you fix anything here then do it there as
     well. */

  int res=SQLITE_OK;

  assert(!PyErr_Occurred());

  if(obj==Py_None)
    PYSQLITE_CUR_CALL(res=sqlite3_bind_null(self->statement->vdbestatement, arg));
  /* Python uses a 'long' for storage of PyInt.  This could
     be a 32bit or 64bit quantity depending on the platform. */
#if PY_MAJOR_VERSION < 3
  else if(PyInt_Check(obj))
    {
      long v=PyInt_AS_LONG(obj);
      PYSQLITE_CUR_CALL(res=sqlite3_bind_int64(self->statement->vdbestatement, arg, v));
    }
#endif
  else if (PyLong_Check(obj))
    {
      /* nb: PyLong_AsLongLong can cause Python level error */
      long long v=PyLong_AsLongLong(obj);
      PYSQLITE_CUR_CALL(res=sqlite3_bind_int64(self->statement->vdbestatement, arg, v));
    }
  else if (PyFloat_Check(obj))
    {
      double v=PyFloat_AS_DOUBLE(obj);
      PYSQLITE_CUR_CALL(res=sqlite3_bind_double(self->statement->vdbestatement, arg, v));
    }
  else if (PyUnicode_Check(obj))
    {
      const void *badptr=NULL;
      UNIDATABEGIN(obj)
        APSW_FAULT_INJECT(DoBindingUnicodeConversionFails,,strdata=(char*)PyErr_NoMemory());
        badptr=strdata;
#ifdef APSW_TEST_LARGE_OBJECTS
        APSW_FAULT_INJECT(DoBindingLargeUnicode,,strbytes=0x001234567890L);
#endif
        if(strdata)
          {
	    if(strbytes>APSW_INT32_MAX)
	      {
                SET_EXC(SQLITE_TOOBIG, NULL);
	      }
	    else
              PYSQLITE_CUR_CALL(res=USE16(sqlite3_bind_text)(self->statement->vdbestatement, arg, strdata, strbytes, SQLITE_TRANSIENT));
          }
      UNIDATAEND(obj);
      if(!badptr) 
        {
          assert(PyErr_Occurred());
          return -1;
        }
    }
#if PY_MAJOR_VERSION < 3
  else if (PyString_Check(obj))
    {
      const char *val=PyString_AS_STRING(obj);
      const size_t lenval=PyString_GET_SIZE(obj);
      const char *chk=val;

      if(lenval<10000)
        for(;chk<val+lenval && !((*chk)&0x80); chk++);
      if(chk<val+lenval)
        {
          const void *badptr=NULL;
          PyObject *str2=PyUnicode_FromObject(obj);
          if(!str2)
            return -1;
          UNIDATABEGIN(str2)
            APSW_FAULT_INJECT(DoBindingStringConversionFails,,strdata=(char*)PyErr_NoMemory());
#ifdef APSW_TEST_LARGE_OBJECTS
            APSW_FAULT_INJECT(DoBindingLargeString,,strbytes=0x001234567890L);
#endif
            badptr=strdata;
            if(strdata)
              {
		if(strbytes>APSW_INT32_MAX)
		  {
                    SET_EXC(SQLITE_TOOBIG, NULL);
                    res=SQLITE_TOOBIG;
		  }
		else
                  PYSQLITE_CUR_CALL(res=USE16(sqlite3_bind_text)(self->statement->vdbestatement, arg, strdata, strbytes, SQLITE_TRANSIENT));
              }
          UNIDATAEND(str2);
          Py_DECREF(str2);
          if(!badptr) 
            {
              assert(PyErr_Occurred());
              return -1;
            }
        }
      else
	{
	  assert(lenval<APSW_INT32_MAX);
	  PYSQLITE_CUR_CALL(res=sqlite3_bind_text(self->statement->vdbestatement, arg, val, lenval, SQLITE_TRANSIENT));
	}
    }
#endif
  else if (PyObject_CheckReadBuffer(obj))
    {
      const void *buffer;
      Py_ssize_t buflen;
      int asrb;
      
      APSW_FAULT_INJECT(DoBindingAsReadBufferFails,asrb=PyObject_AsReadBuffer(obj, &buffer, &buflen), (PyErr_NoMemory(), asrb=-1));
      if(asrb!=0)
        return -1;

      if (buflen>APSW_INT32_MAX)
	{
          SET_EXC(SQLITE_TOOBIG, NULL);
	  return -1;
	}
      PYSQLITE_CUR_CALL(res=sqlite3_bind_blob(self->statement->vdbestatement, arg, buffer, buflen, SQLITE_TRANSIENT));
    }
  else if(PyObject_TypeCheck(obj, &ZeroBlobBindType)==1)
    {
      PYSQLITE_CUR_CALL(res=sqlite3_bind_zeroblob(self->statement->vdbestatement, arg, ((ZeroBlobBind*)obj)->blobsize));
    }
  else 
    {
      PyErr_Format(PyExc_TypeError, "Bad binding argument type supplied - argument #%d: type %s", (int)(arg+self->bindingsoffset), Py_TYPE(obj)->tp_name);
      return -1;
    }
  if(res!=SQLITE_OK)
    {
      SET_EXC(res, self->connection->db);
      return -1;
    }
  if(PyErr_Occurred())
    return -1;
  return 0;
}

/* internal function */
static int
APSWCursor_dobindings(APSWCursor *self)
{
  int nargs, arg, res=-1, sz=0;
  PyObject *obj;

  assert(!PyErr_Occurred());
  assert(self->bindingsoffset>=0);

  nargs=sqlite3_bind_parameter_count(self->statement->vdbestatement);
  if(nargs==0 && !self->bindings)
    return 0; /* common case, no bindings needed or supplied */

  if (nargs>0 && !self->bindings)
    {
      PyErr_Format(ExcBindings, "Statement has %d bindings but you didn't supply any!", nargs);
      return -1;
    }

  /* a dictionary? */
  if (self->bindings && PyDict_Check(self->bindings))
    {
      for(arg=1;arg<=nargs;arg++)
        {
	  PyObject *keyo=NULL;
          const char *key;

          PYSQLITE_CUR_CALL(key=sqlite3_bind_parameter_name(self->statement->vdbestatement, arg));

          if(!key)
            {
              PyErr_Format(ExcBindings, "Binding %d has no name, but you supplied a dict (which only has names).", arg-1);
              return -1;
            }

	  assert(*key==':' || *key=='$');
          key++; /* first char is a colon or dollar which we skip */

	  keyo=PyUnicode_DecodeUTF8(key, strlen(key), NULL);
	  if(!keyo) return -1;

	  obj=PyDict_GetItem(self->bindings, keyo);
	  Py_DECREF(keyo);

          if(!obj)
            /* this is where we could error on missing keys */
            continue;
          if(APSWCursor_dobinding(self,arg,obj)!=SQLITE_OK)
            {
              assert(PyErr_Occurred());
              return -1;
            }
        }

      return 0;
    }

  /* it must be a fast sequence */
  /* verify the number of args supplied */
  if (self->bindings)
    sz=PySequence_Fast_GET_SIZE(self->bindings);
  /* there is another statement after this one ... */
  if(self->statement->next && sz-self->bindingsoffset<nargs)
    {
      PyErr_Format(ExcBindings, "Incorrect number of bindings supplied.  The current statement uses %d and there are only %d left.  Current offset is %d",
                   nargs, (self->bindings)?sz:0, (int)(self->bindingsoffset));
      return -1;
    }
  /* no more statements */
  if(!self->statement->next && sz-self->bindingsoffset!=nargs)
    {
      PyErr_Format(ExcBindings, "Incorrect number of bindings supplied.  The current statement uses %d and there are %d supplied.  Current offset is %d",
                   nargs, (self->bindings)?sz:0, (int)(self->bindingsoffset));
      return -1;
    }
  
  res=SQLITE_OK;

  /* nb sqlite starts bind args at one not zero */
  for(arg=1;arg<=nargs;arg++)
    {
      obj=PySequence_Fast_GET_ITEM(self->bindings, arg-1+self->bindingsoffset);
      if(APSWCursor_dobinding(self, arg, obj))
        {
          assert(PyErr_Occurred());
          return -1;
        }
    }

  self->bindingsoffset+=nargs;
  assert(res==0);
  return 0;
}

static int
APSWCursor_doexectrace(APSWCursor *self, Py_ssize_t savedbindingsoffset)
{
  PyObject *retval=NULL;
  PyObject *sqlcmd=NULL;
  PyObject *bindings=NULL;
  int result;

  assert(self->exectrace);
  assert(self->statement);

  /* make a string of the command */
  sqlcmd=convertutf8buffersizetounicode(self->statement->utf8, self->statement->querylen);

  if(!sqlcmd) return -1;

  /* now deal with the bindings */
  if(self->bindings)
    {
      if(PyDict_Check(self->bindings))
        {
          bindings=self->bindings;
          Py_INCREF(self->bindings);
        }
      else
        {
          APSW_FAULT_INJECT(DoExecTraceBadSlice,
          bindings=PySequence_GetSlice(self->bindings, savedbindingsoffset, self->bindingsoffset),
          bindings=PyErr_NoMemory());

          if(!bindings)
            {
              Py_DECREF(sqlcmd);
              return -1;
            }
        }
    }
  else
    {
      bindings=Py_None;
      Py_INCREF(bindings);
    }

  retval=PyObject_CallFunction(self->exectrace, "OO", sqlcmd, bindings);
  Py_DECREF(sqlcmd);
  Py_DECREF(bindings);
  if(!retval) 
    {
      assert(PyErr_Occurred());
      return -1;
    }
  result=PyObject_IsTrue(retval);
  Py_DECREF(retval);
  assert (result==-1 || result==0 || result ==1);
  if(result==-1)
    {
      assert(PyErr_Occurred());
      return -1;
    }
  if(result)
    return 0;

  /* callback didn't want us to continue */
  PyErr_Format(ExcTraceAbort, "Aborted by false/null return value of exec tracer");
  return -1;
}

static PyObject*
APSWCursor_dorowtrace(APSWCursor *self, PyObject *retval)
{
  assert(self->rowtrace);

  retval=PyEval_CallObject(self->rowtrace, retval);
  if(!retval) 
    return NULL;
  
  return retval;
}

/* Returns a borrowed reference to self if all is ok, else NULL on error */
static PyObject *
APSWCursor_step(APSWCursor *self)
{
  int res;
  int savedbindingsoffset=0; /* initialised to stop stupid compiler from whining */

  for(;;)
    {
      assert(!PyErr_Occurred());
      PYSQLITE_CUR_CALL(res=(self->statement->vdbestatement)?(sqlite3_step(self->statement->vdbestatement)):(SQLITE_DONE));

      switch(res&0xff)
        {
	case SQLITE_ROW:
          self->status=C_ROW;
          return (PyErr_Occurred())?(NULL):((PyObject*)self);

        case SQLITE_DONE:
	  if (PyErr_Occurred())
	    {
	      self->status=C_DONE;
	      return NULL;
	    }
          break;


	case SQLITE_SCHEMA:
	  /* We used to call statementcache_dup which did a reprepare.
             To avoid race conditions with the statement cache (we
             release the GIL around prepare now) we now just return
             the error.  See SQLite ticket 2158.

             ::TODO:: with the new fangled statementcache it is safe
             to do a reprepare
	   */

        default: /* sqlite3_prepare_v2 introduced in 3.3.9 means the
		    error code is returned from step as well as
		    finalize/reset */
          /* FALLTHRU */
        case SQLITE_ERROR:  /* SQLITE_BUSY is handled here as well */
          /* there was an error - we need to get actual error code from sqlite3_finalize */
          self->status=C_DONE;
          if(PyErr_Occurred())
            /* we don't care about further errors from the sql */
            resetcursor(self, 1);
          else
            {
              res=resetcursor(self, 0);  /* this will get the error code for us */
              assert(res!=SQLITE_OK);
            }
          return NULL;

          
        }
      assert(res==SQLITE_DONE);

      /* done with that statement, are there any more? */
      self->status=C_DONE;
      if(!self->statement->next)
        {
          PyObject *next;

          /* in executemany mode ?*/
          if(!self->emiter)
            {
              /* no more so we finalize */
              if(resetcursor(self, 0)!=SQLITE_OK)
                {
                  assert(PyErr_Occurred());
                  return NULL; /* exception */
                }
              return (PyObject*)self;
            }

          /* we are in executemany mode */
          next=PyIter_Next(self->emiter);
          if(PyErr_Occurred())
            return NULL;
          
          if(!next)
            {
              /* clear out statement if no more*/
              if(resetcursor(self, 0)!=SQLITE_OK)
                {
                  assert(PyErr_Occurred());
                  return NULL;
                }

            return (PyObject*)self;
            }

          /* we need to clear just completed and restart original executemany statement */
          INUSE_CALL(statementcache_finalize(self->connection->stmtcache, self->statement));
          self->statement=NULL;
          /* don't need bindings from last round if emiter.next() */
          Py_XDECREF(self->bindings);
          self->bindings=0;
          self->bindingsoffset=0;
          /* verify type of next before putting in bindings */
          if(PyDict_Check(next))
            self->bindings=next;
          else
            {
              self->bindings=PySequence_Fast(next, "You must supply a dict or a sequence");
              /* we no longer need next irrespective of what happens in line above */
              Py_DECREF(next);
              if(!self->bindings)
                return NULL;
            }
          assert(self->bindings);
        }

      /* finalise and go again */
      if(!self->statement)
        {
          /* we are going again in executemany mode */
          assert(self->emiter);
          INUSE_CALL(self->statement=statementcache_prepare(self->connection->stmtcache, self->emoriginalquery));
          res=(self->statement)?SQLITE_OK:SQLITE_ERROR;
        }
      else
        {
          /* next sql statement */
          INUSE_CALL(res=statementcache_next(self->connection->stmtcache, &self->statement));
          SET_EXC(res, self->connection->db);
        }

      if (res!=SQLITE_OK)
        {
          assert((res&0xff)!=SQLITE_BUSY); /* finalize shouldn't be returning busy, only step */
          assert(!self->statement);
          return NULL;
        }

      assert(self->statement);
      if(self->exectrace)
        {
          savedbindingsoffset=self->bindingsoffset;
        }
      assert(!PyErr_Occurred());

      if(APSWCursor_dobindings(self))
        {
          assert(PyErr_Occurred());
          return NULL;
        }

      if(self->exectrace)
        {
          if(APSWCursor_doexectrace(self, savedbindingsoffset))
            {
              assert(self->status==C_DONE);
              assert(PyErr_Occurred());
              return NULL;
            }
        }
      assert(self->status==C_DONE);
      self->status=C_BEGIN;
    }

  /* you can't actually get here */
  assert(0);
  return NULL;
}

/** .. method:: execute(statements[, bindings]) -> iterator

    Executes the statements using the supplied bindings.  Execution
    returns when the first row is available or all statements have
    completed.  

    :param statements: One or more SQL statements such as ``select *
      from books`` or ``begin; insert into books ...; select
      last_insert_rowid(); end``.
    :param bindings: If supplied should either be a sequence or a dictionary.  Each item must be one of the :ref:`supported types <types>`

    If you use numbered bindings in the query then supply a sequence.
    Any sequence will work including lists and iterators.  For
    example::

      cursor.execute("insert into books values(?,?)", ("title", "number"))

    .. note:: 

      A common gotcha is wanting to insert a single string but not
      putting it in a tuple::

        cursor.execute("insert into books values(?)", "a title")

      The string is a sequence of 8 characters and so it will look
      like you are supplying 8 bindings when only one is needed.  Use
      a one item tuple with a trailing comma like this::

        cursor.execute("insert into books values(?)", ("a title",) )

    If you used names in the statement then supply a dictionary as the
    binding.  It is ok to be missing entries from the dictionary -
    None/null will be used.  For example::

       cursor.execute("insert into books values(:title, :isbn, :rating)", 
            {"title": "book title", "isbn": 908908908})

    The return is the cursor object itself which is also an iterator.  This allows you to write::

       for row in cursor.execute("select * from books"):
          print row

    :raises TypeError: The bindings supplied were neither a dict nor a sequence
    :raises BindingsError: You supplied too many or too few bindings for the statements
    :raises IncompleteExecutionError: There are remaining unexecuted queries from your last execute

    -* sqlite3_prepare_v2 sqlite3_step sqlite3_bind_int64 sqlite3_bind_null sqlite3_bind_text sqlite3_bind_double sqlite3_bind_blob sqlite3_bind_zeroblob

    .. seealso::

       * :ref:`executionmodel`
       * :ref:`Example <example-cursor>`
 
*/
static PyObject *
APSWCursor_execute(APSWCursor *self, PyObject *args)
{
  int res;
  int savedbindingsoffset=-1;
  PyObject *retval=NULL;
  PyObject *query;

  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  res=resetcursor(self, /* force= */ 0);
  if(res!=SQLITE_OK)
    {
      assert(PyErr_Occurred());
      return NULL;
    }
  
  assert(!self->bindings);
  assert(PyTuple_Check(args));

  if(PyTuple_GET_SIZE(args)<1 || PyTuple_GET_SIZE(args)>2)
    {
      PyErr_Format(PyExc_TypeError, "Incorrect number of arguments.  execute(statements [,bindings])");
      return NULL;
    }

  query=PyTuple_GET_ITEM(args, 0);
  if (PyTuple_GET_SIZE(args)==2)
    self->bindings=PyTuple_GET_ITEM(args, 1);

  if(self->bindings)
    {
      if(PyDict_Check(self->bindings))
        Py_INCREF(self->bindings);
      else
        {
          self->bindings=PySequence_Fast(self->bindings, "You must supply a dict or a sequence");
          if(!self->bindings)
            return NULL;
        }
    }

  assert(!self->statement);
  assert(!PyErr_Occurred());
  INUSE_CALL(self->statement=statementcache_prepare(self->connection->stmtcache, query));
  if (!self->statement)
    {
      AddTraceBackHere(__FILE__, __LINE__, "APSWCursor_execute.sqlite3_prepare_v2", "{s: O, s: O}", 
		       "Connection", self->connection, 
		       "statement", query);
      return NULL;
    }
  assert(!PyErr_Occurred());

  self->bindingsoffset=0;
  if(self->exectrace)
    savedbindingsoffset=0;

  if(APSWCursor_dobindings(self))
    {
      assert(PyErr_Occurred());
      return NULL;
    }

  if(self->exectrace)
    {
      if(APSWCursor_doexectrace(self, savedbindingsoffset))
        {
          assert(PyErr_Occurred());
          return NULL;  
        }
    }

  self->status=C_BEGIN;

  retval=APSWCursor_step(self);
  if (!retval) 
    {
      assert(PyErr_Occurred());
      return NULL;
    }
  Py_INCREF(retval);
  return retval;
}

/** .. method:: executemany(statements, sequenceofbindings)  -> iterator

  This method is for when you want to execute the same statements over
  a sequence of bindings.  Conceptually it does this::

    for binding in sequenceofbindings:
        cursor.execute(statements, binding)

  Example::

    rows=(  (1, 7),
            (2, 23),
            (4, 92),
            (12, 12) )

    cursor.executemany("insert into nums values(?,?)", rows)

  The return is the cursor itself which acts as an iterator.  Your
  statements can return data.  See :meth:`~Cursor.execute` for more
  information.
*/

static PyObject *
APSWCursor_executemany(APSWCursor *self, PyObject *args)
{
  int res;
  PyObject *retval=NULL;
  PyObject *theiterable=NULL;
  PyObject *next=NULL;
  PyObject *query=NULL;
  int savedbindingsoffset=-1;

  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  res=resetcursor(self, /* force= */ 0);
  if(res!=SQLITE_OK)
    {
      assert(PyErr_Occurred());
      return NULL;
    }
  
  assert(!self->bindings);
  assert(!self->emiter);
  assert(!self->emoriginalquery);
  assert(self->status=C_DONE);

  if(!PyArg_ParseTuple(args, "OO:executemany(statements, sequenceofbindings)", &query, &theiterable))
    return NULL;

  self->emiter=PyObject_GetIter(theiterable);
  if (!self->emiter)
    {
      PyErr_Format(PyExc_TypeError, "2nd parameter must be iterable");
      return NULL;
    }

  next=PyIter_Next(self->emiter);
  if(!next && PyErr_Occurred())
    return NULL;
  if(!next)
    {
      /* empty list */
      Py_INCREF(self);
      return (PyObject*)self;
    }

  if(PyDict_Check(next))
    self->bindings=next;
  else
    {
      self->bindings=PySequence_Fast(next, "You must supply a dict or a sequence");
      Py_DECREF(next); /* _Fast makes new reference */
      if(!self->bindings)
          return NULL;
    }

  assert(!self->statement);
  assert(!PyErr_Occurred());
  assert(!self->statement);
  INUSE_CALL(self->statement=statementcache_prepare(self->connection->stmtcache, query));
  if (!self->statement)
    {
      AddTraceBackHere(__FILE__, __LINE__, "APSWCursor_executemany.sqlite3_prepare_v2", "{s: O, s: O}", 
		       "Connection", self->connection, 
		       "statement", query);
      return NULL;
    }
  assert(!PyErr_Occurred());

  self->emoriginalquery=self->statement->utf8;
  Py_INCREF(self->emoriginalquery);

  self->bindingsoffset=0;
  if(self->exectrace)
    savedbindingsoffset=0;

  if(APSWCursor_dobindings(self))
    {
      assert(PyErr_Occurred());
      return NULL;
    }

  if(self->exectrace)
    {
      if(APSWCursor_doexectrace(self, savedbindingsoffset))
        {
          assert(PyErr_Occurred());
          return NULL;  
        }
    }

  self->status=C_BEGIN;

  retval=APSWCursor_step(self);
  if (!retval) 
    {
      assert(PyErr_Occurred());
      return NULL;
    }
  Py_INCREF(retval);
  return retval;
}

/** .. method:: close(force=False)

  It is very unlikely you will need to call this method.  It exists
  because older versions of SQLite required all Connection/Cursor
  activity to be confined to the same thread.  That is no longer the
  case.  Cursors are automatically garbage collected and when their
  are none left will allow the connection to be garbage collected if
  it has no other references.

  A cursor is open if there are remaining statements to execute (if
  your query included multiple statements), or if you called
  :meth:`~Cursor.executemany` and not all of the `sequenceofbindings`
  have been used yet.

  :param force: If False then you will get exceptions if there is
   remaining work to do be in the Cursor such as more statements to
   execute, more data from the executemany binding sequence etc. If
   force is True then all remaining work and state information will be
   silently discarded.

*/

static PyObject *
APSWCursor_close(APSWCursor *self, PyObject *args)
{
  int res;
  int force=0;

  CHECK_USE(NULL);
  if (!self->connection->db) /* if connection is closed, then we must also be closed */
    Py_RETURN_NONE;

  if(!PyArg_ParseTuple(args, "|i:close(force=False)", &force))
    return NULL;

  res=resetcursor(self, force);
  if(res!=SQLITE_OK)
    {
      assert(PyErr_Occurred());
      return NULL;
    }
  Py_RETURN_NONE;
}

/** .. method:: next() -> row

  Returns the next row of data or raises StopIteration if there are no
  more rows.  Python calls this method behind the scenes when using
  the cursor as an iterator.  It is unlikely you will want to manually
  call it.
*/

static PyObject *
APSWCursor_next(APSWCursor *self)
{
  PyObject *retval;
  PyObject *item;
  int numcols=-1;
  int i;

  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

 again:
  if(self->status==C_BEGIN)
    if(!APSWCursor_step(self))
      {
        assert(PyErr_Occurred());
        return NULL;
      }
  if(self->status==C_DONE)
    return NULL;

  assert(self->status==C_ROW);

  self->status=C_BEGIN;
  
  /* return the row of data */
  numcols=sqlite3_data_count(self->statement->vdbestatement);
  retval=PyTuple_New(numcols);
  if(!retval) goto error;

  for(i=0;i<numcols;i++)
    {
      INUSE_CALL(item=convert_column_to_pyobject(self->statement->vdbestatement, i));
      if(!item) goto error;
      PyTuple_SET_ITEM(retval, i, item);
    }
  if(self->rowtrace)
    {
      PyObject *r2=APSWCursor_dorowtrace(self, retval);
      Py_DECREF(retval);
      if(!r2) 
	return NULL;
      if (r2==Py_None)
        {
          Py_DECREF(r2);
          goto again;
        }
      return r2;
    }
  return retval;
 error:
  Py_XDECREF(retval);
  return NULL;
}

static PyObject *
APSWCursor_iter(APSWCursor *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  Py_INCREF(self);
  return (PyObject*)self;
}

/** .. method:: setexectrace(callable)

  `callable` is called with the statement and bindings for each
  :meth:`~Cursor.execute` or :meth:`~Cursor.executemany` on this
  cursor.  An example use is if you want to log all statements
  executed.  Your execution tracer can also abort execution of a
  statement.  See :ref:`executiontracer` for more details.

  If `callable` is `None` then any existing execution tracer is
  removed.
*/

static PyObject *
APSWCursor_setexectrace(APSWCursor *self, PyObject *func)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  if(func!=Py_None && !PyCallable_Check(func))
    {
      PyErr_SetString(PyExc_TypeError, "parameter must be callable");
      return NULL;
    }

  if(func!=Py_None)
    Py_INCREF(func);

  Py_XDECREF(self->exectrace);
  self->exectrace=(func!=Py_None)?func:NULL;

  Py_RETURN_NONE;
}

/** .. method:: setrowtrace(callable)

  `callable` is called with each row being returned.  You can change
  the data that is returned or cause the row to be skipped altogether.
  An example use is if you want to log all rows returned.  See
  :ref:`rowtracer` for more details.

  If `callable` is `None` then any existing row tracer is
  removed.
*/

static PyObject *
APSWCursor_setrowtrace(APSWCursor *self, PyObject *func)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  if(func!=Py_None && !PyCallable_Check(func))
    {
      PyErr_SetString(PyExc_TypeError, "parameter must be callable");
      return NULL;
    }

  if(func!=Py_None)
    Py_INCREF(func);

  Py_XDECREF(self->rowtrace);
  self->rowtrace=(func!=Py_None)?func:NULL;

  Py_RETURN_NONE;
}

/** .. method:: getexectrace() -> callable or None

  Returns the currently installed (via :meth:`~Cursor.setexectrace`)
  execution tracer.
*/
static PyObject *
APSWCursor_getexectrace(APSWCursor *self)
{
  PyObject *ret;

  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  ret=(self->exectrace)?(self->exectrace):Py_None;
  Py_INCREF(ret);
  return ret;
}

/** .. method:: getrowtrace() -> callable or None

  Returns the currently installed (via :meth:`~Cursor.setrowtrace`)
  row tracer.
*/
static PyObject *
APSWCursor_getrowtrace(APSWCursor *self)
{
  PyObject *ret;
  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);
  ret =(self->rowtrace)?(self->rowtrace):Py_None;
  Py_INCREF(ret);
  return ret;
}

/** .. method:: getconnection() -> Connection

  Returns the :class:`Connection` this cursor belongs to.  An example usage is to get another cursor::

    def func(cursor):
      # I don't want to alter existing cursor, so make a new one
      mycursor=cursor.getconnection().cursor()
      mycursor.execute("....")

*/

static PyObject *
APSWCursor_getconnection(APSWCursor *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  Py_INCREF(self->connection);
  return (PyObject*)self->connection;
}

static PyMethodDef APSWCursor_methods[] = {
  {"execute", (PyCFunction)APSWCursor_execute, METH_VARARGS,
   "Executes one or more statements" },
  {"executemany", (PyCFunction)APSWCursor_executemany, METH_VARARGS,
   "Repeatedly executes statements on sequence" },
  {"setexectrace", (PyCFunction)APSWCursor_setexectrace, METH_O,
   "Installs a function called for every statement executed"},
  {"setrowtrace", (PyCFunction)APSWCursor_setrowtrace, METH_O,
   "Installs a function called for every row returned"},
  {"getexectrace", (PyCFunction)APSWCursor_getexectrace, METH_NOARGS,
   "Returns the current exec tracer function"},
  {"getrowtrace", (PyCFunction)APSWCursor_getrowtrace, METH_NOARGS,
   "Returns the current row tracer function"},
  {"getrowtrace", (PyCFunction)APSWCursor_getrowtrace, METH_NOARGS,
   "Returns the current row tracer function"},
  {"getconnection", (PyCFunction)APSWCursor_getconnection, METH_NOARGS,
   "Returns the connection object for this cursor"},
  {"getdescription", (PyCFunction)APSWCursor_getdescription, METH_NOARGS,
   "Returns the description for the current row"},
  {"close", (PyCFunction)APSWCursor_close, METH_VARARGS,
   "Closes the cursor" },
  {0, 0, 0, 0}  /* Sentinel */
};


static PyTypeObject APSWCursorType = {
#if PY_MAJOR_VERSION < 3
    PyObject_HEAD_INIT(NULL)
    0,                         /*ob_size*/
#else
    PyVarObject_HEAD_INIT(NULL,0)
#endif
    "apsw.Cursor",             /*tp_name*/
    sizeof(APSWCursor),            /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)APSWCursor_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_VERSION_TAG
#if PY_MAJOR_VERSION < 3
 | Py_TPFLAGS_HAVE_ITER
#endif
 , /*tp_flags*/
    "Cursor object",           /* tp_doc */
    0,		               /* tp_traverse */
    0,		               /* tp_clear */
    0,		               /* tp_richcompare */
    0,		               /* tp_weaklistoffset */
    (getiterfunc)APSWCursor_iter,  /* tp_iter */
    (iternextfunc)APSWCursor_next, /* tp_iternext */
    APSWCursor_methods,            /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    0,                         /* tp_new */
    0,                         /* tp_free */
    0,                         /* tp_is_gc */
    0,                         /* tp_bases */
    0,                         /* tp_mro */
    0,                         /* tp_cache */
    0,                         /* tp_subclasses */
    0,                         /* tp_weaklist */
    0,                         /* tp_del */
#if PY_VERSION_HEX>=0x02060000
    0                          /* tp_version_tag */
#endif
};
