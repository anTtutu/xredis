#include "xHiredis.h"



static int intlen(int i)
{
	int len = 0;
	if (i < 0)
	{
		len++;
		i = -i;
	}

	do
	{
		len++;
		i /= 10;
	}while(i);

	return len;
}


/* Create a reply object */
redisReply * createReplyObject(int type)
{
	redisReply * r = (redisReply*)zcalloc( sizeof(*r));

	if (r == nullptr)
		return nullptr;

	r->type = type;
	return r;
}

static long long readLongLong(const char * s)
{
	long long v = 0;
	int  dec, mult = 1;
	char	c;

	if (*s == '-')
	{
		mult	= -1;
		s++;
	}
	else if (*s == '+')
	{
		mult	= 1;
		s++;
	}

	while ((c = * (s++)) != '\r')
	{
		dec = c - '0';

		if (dec >= 0 && dec < 10)
		{
			v *= 10;
			v += dec;
		}
		else
		{
			/* Should not happen... */
			return - 1;
		}
	}

	return mult * v;
}


/* Find pointer to \r\n. */
static const char * seekNewline(const char * s, size_t len)
{
	int 	pos = 0;
	int 	_len = len - 1;

	/* Position should be < len-1 because the character at "pos" should be
	 * followed by a \n. Note that strchr cannot be used because it doesn't
	 * allow to search a limited length and the buffer that is being searched
	 * might not have a trailing NULL character. */
	while (pos < _len)
	{
		while (pos < _len && s[pos] != '\r')
			pos++;

		if (s[pos] != '\r')
		{
			/* Not found. */
			return nullptr;
		}
		else
		{
			if (s[pos + 1] == '\n')
			{
				/* Found. */
				return s + pos;
			}
			else
			{
				/* Continue searching. */
				pos++;
			}
		}
	}

	return nullptr;
}


static const char * readLine(const xRedisReaderPtr & r,  int * _len)
{
	const char * p;
	const char *s;
	int len;

	p = r->buf->peek() + r->pos;
	s = seekNewline(p, (r->buf->readableBytes() - r->pos));

	if (s != nullptr)
	{
		len = s - (r->buf->peek() + r->pos);
		r->pos += len + 2; 			/* skip \r\n */

		if (_len)
			*_len = len;

		return p;
	}

	return nullptr;
}

static  const char * readBytes(const xRedisReaderPtr & r, unsigned int bytes)
{
	const char * p;

	if (r->buf->readableBytes() - r->pos >= bytes)
	{
		p = r->buf->peek() + r->pos;
		r->pos += bytes;
		return p;
	}

	return nullptr;
}


/* Free a reply object */
void freeReply(void * reply)
{
	redisReply * r = (redisReply*)reply;
	size_t	 j;

	switch (r->type)
	{
		case REDIS_REPLY_INTEGER:
			break; /* Nothing to free */

		case REDIS_REPLY_ARRAY:
			if (r->element != nullptr)
			{
				for (j = 0; j < r->elements; j++)
					if (r->element[j] != nullptr)
						freeReply(r->element[j]);

				zfree(r->element);
			}

			break;

		case REDIS_REPLY_ERROR:
		case REDIS_REPLY_STATUS:
		case REDIS_REPLY_STRING:
			if (r->str != nullptr)
				zfree(r->str);

			break;
	}

	zfree(r);
}

void * createString(const redisReadTask * task, const char * str, size_t len)
{
	redisReply *r, *parent;
	char * buf;

	r = createReplyObject(task->type);

	if (r == nullptr)
		return nullptr;

	buf  = (char *)zmalloc(len + 1);

	if (buf == nullptr)
	{
		freeReply(r);
		return nullptr;
	}

	assert(task->type == REDIS_REPLY_ERROR || task->type == REDIS_REPLY_STATUS || task->type == REDIS_REPLY_STRING);

	/* Copy string value */
	memcpy(buf, str, len);
	buf[len] = '\0';
	r->str = buf;
	r->len = len;

	if (task->parent)
	{
		parent =(redisReply*) task->parent->obj;
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element[task->idx] = r;
	}

	return r;
}


void * createArray(const redisReadTask * task, int elements)
{
	redisReply * r, *parent;

	r = createReplyObject(REDIS_REPLY_ARRAY);

	if (r == nullptr)
		return nullptr;

	if (elements > 0)
	{
		r->element	 = (redisReply**)zmalloc(elements * sizeof(redisReply *));

		if (r->element == nullptr)
		{
			freeReply(r);
			return nullptr;
		}
	}

	r->elements = elements;

	if (task->parent)
	{
		parent = (redisReply*)task->parent->obj;
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element[task->idx] = r;
	}

	return r;
}


void * createInteger(const redisReadTask * task, long long value)
{
	redisReply * r, *parent;

	r = createReplyObject(REDIS_REPLY_INTEGER);

	if (r == nullptr)
		return nullptr;

	r->integer	= value;

	if (task->parent)
	{
		parent = (redisReply*)task->parent->obj;
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element[task->idx] = r;
	}

	return r;
}


void * createNil(const redisReadTask * task)
{
	redisReply * r, *parent;

	r = createReplyObject(REDIS_REPLY_NIL);

	if (r == NULL)
		return nullptr;

	if (task->parent)
	{
		parent = (redisReply*)task->parent->obj;
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element[task->idx] = r;
	}

	return r;
}


static size_t bulklen(size_t len)
{
	return 1 + intlen(len) + 2 + len + 2;
}


static char *nextArgument(char *start, char **str, size_t *len) {
    char *p = start;
    if (p[0] != '$') {
        p = strchr(p,'$');
        if (p == NULL) return NULL;
    }

    *len = (int)strtol(p+1,NULL,10);
    p = strchr(p,'\r');
    assert(p);
    *str = p+2;
    return p+2+(*len)+2;
}



void __redisSetError(const xRedisContextPtr  & c, int type, const char * str)
{
	size_t	len;

	c->err = type;

	if (str != nullptr)
	{
		len  = strlen(str);
		len  = len < (sizeof(c->errstr) - 1) ? len: (sizeof(c->errstr) - 1);
		memcpy(c->errstr, str, len);
		c->errstr[len]		= '\0';
	}
	else
	{
		/* Only REDIS_ERR_IO may lack a description! */
		assert(type == REDIS_ERR_IO);
		strerror_r(errno, c->errstr, sizeof(c->errstr));
	}
}

static void moveToNextTask(const xRedisReaderPtr & r)
{
	redisReadTask * cur, *prv;
	while (r->ridx >= 0)
	{
		/* Return a.s.a.p. when the stack is now empty. */
		if (r->ridx == 0)
		{
			r->ridx--;
			return;
		}

		cur = & (r->rstack[r->ridx]);
		prv = & (r->rstack[r->ridx - 1]);
		assert(prv->type == REDIS_REPLY_ARRAY);

		if (cur->idx == prv->elements - 1)
		{
			r->ridx--;
		}
		else
		{
			/* Reset the type because the next item can be anything */
			assert(cur->idx < prv->elements);
			cur->type = -1;
			cur->elements = -1;
			cur->idx++;
			return;
		}
	}
}


static int processLineItem(const xRedisReaderPtr  &r)
{
	redisReadTask * cur = & (r->rstack[r->ridx]);
	void * obj;
	const char * p;
	int len;

	if ((p = readLine(r, &len)) != nullptr)
	{
		if (cur->type == REDIS_REPLY_INTEGER)
		{
			if (r->fn && r->fn->createIntegerFuc)
				obj = r->fn->createIntegerFuc(cur, readLongLong(p));
			else
				obj = (void *)
				REDIS_REPLY_INTEGER;
			}
			else
			{
				/* Type will be error or status. */
				if (r->fn && r->fn->createStringFuc)
					obj = r->fn->createStringFuc(cur, p, len);
				else
					obj = (void *) (size_t) (cur->type);
			}

			if (obj == nullptr)
			{
				//__redisReaderSetErrorOOM(r);
				return REDIS_ERR;
			}

			/* Set reply if this is the root object. */
			if (r->ridx == 0)
				r->reply = obj;

			moveToNextTask(r);
			return REDIS_OK;
	}

	return REDIS_ERR;
}


static int processBulkItem(const xRedisReaderPtr  & r)
{
	redisReadTask * cur = & (r->rstack[r->ridx]);
	void * obj = nullptr;
	const char * p;
	const char *s;
	long	len;
	unsigned long bytelen;
	int success = 0;

	p = r->buf->peek() + r->pos;
	s = seekNewline(p, r->buf->readableBytes() - r->pos);

	if (s != nullptr)
	{
		p = r->buf->peek() + r->pos;
		bytelen = s - (r->buf->peek() + r->pos) + 2; /* include \r\n */
		len = readLongLong(p);

		if (len < 0)
		{
			/* The nil object can always be created. */
			if (r->fn && r->fn->createNilFuc)
				obj = r->fn->createNilFuc(cur);
			else
				obj = (void *)
				REDIS_REPLY_NIL;

				success 			= 1;
		}
		else
		{
			/* Only continue when the buffer contains the entire bulk item. */
			bytelen 			+= len + 2; 		/* include \r\n */

			if (r->pos + bytelen <= r->buf->readableBytes())
			{
				if (r->fn && r->fn->createStringFuc)
					obj = r->fn->createStringFuc(cur, s + 2, len);
				else
					obj = (void *)
					REDIS_REPLY_STRING;

				success  = 1;
			}
		}

		/* Proceed when obj was created. */
		if (success)
		{
			if (obj == nullptr)
			{
				LOG_WARN<<"obj == nullptr";
				return REDIS_ERR;
			}

			r->pos				+= bytelen;

			/* Set reply if this is the root object. */
			if (r->ridx == 0)
				r->reply = obj;

			moveToNextTask(r);
			return REDIS_OK;
		}
	}

	return REDIS_ERR;
}


static int processMultiBulkItem(const xRedisReaderPtr  & r)
{
	redisReadTask * cur = & (r->rstack[r->ridx]);
	void * obj;
	const char * p;
	long elements;
	int  root = 0;

	/* Set error for nested multi bulks with depth > 7 */
	if (r->ridx == 8)
	{
		LOG_WARN<<"REDIS_ERR_PROTOCOL";
		return REDIS_ERR;
	}

	if ((p = readLine(r, NULL)) != NULL)
	{
		elements			= readLongLong(p);
		root				= (r->ridx == 0);

		if (elements == -1)
		{
			if (r->fn && r->fn->createNilFuc)
				obj = r->fn->createNilFuc(cur);
			else
				obj = (void *)
				REDIS_REPLY_NIL;

			if (obj == nullptr)
			{
				LOG_WARN<<"obj == nullptr";
				return REDIS_ERR;
			}

			moveToNextTask(r);
		}
		else
		{
			if (r->fn && r->fn->createArrayFuc)
				obj = r->fn->createArrayFuc(cur, elements);
			else
				obj = (void *)
				REDIS_REPLY_ARRAY;

			if (obj == nullptr)
			{
				LOG_WARN<<"obj == nullptr";
				return REDIS_ERR;
			}

			/* Modify task stack when there are more than 0 elements. */
			if (elements > 0)
			{
				cur->elements = elements;
				cur->obj = obj;
				r->ridx++;
				r->rstack[r->ridx].type = -1;
				r->rstack[r->ridx].elements = -1;
				r->rstack[r->ridx].idx = 0;
				r->rstack[r->ridx].obj = NULL;
				r->rstack[r->ridx].parent = cur;
				r->rstack[r->ridx].privdata = r->privdata;
			}
			else
			{
				moveToNextTask(r);
			}
		}

		/* Set reply if this is the root object. */
		if (root)
			r->reply = obj;

		return REDIS_OK;
	}

	return REDIS_ERR;
}

static int processItem(const xRedisReaderPtr & r)
{
	redisReadTask * cur = & (r->rstack[r->ridx]);
	const char *			p;

/* check if we need to read type */
	if (cur->type < 0)
	{
		if ((p = readBytes(r, 1)) != NULL)
		{
			switch (p[0])
			{
				case '-':
					cur->type = REDIS_REPLY_ERROR;
					break;

				case '+':
					cur->type = REDIS_REPLY_STATUS;
					break;

				case ':':
					cur->type = REDIS_REPLY_INTEGER;
					break;

				case '$':
					cur->type = REDIS_REPLY_STRING;
					break;

				case '*':
					cur->type = REDIS_REPLY_ARRAY;
					break;

				default:
					
					return REDIS_ERR;
			}
		}
		else
		{
			/* could not consume 1 byte */
			return REDIS_ERR;
		}
	}

/* process typed item */
	switch (cur->type)
	{
		case REDIS_REPLY_ERROR:
		case REDIS_REPLY_STATUS:
		case REDIS_REPLY_INTEGER:
			return processLineItem(r);

		case REDIS_REPLY_STRING:
			return processBulkItem(r);

		case REDIS_REPLY_ARRAY:
			return processMultiBulkItem(r);

		default:
			assert(NULL);
			return REDIS_ERR; /* Avoid warning. */
	}
}

int redisReaderGetReply(const xRedisReaderPtr &r,void * *reply)
{
	/* Default target pointer to NULL. */
	if (reply != nullptr)
		*reply = nullptr;

	/* Return early when this reader is in an erroneous state. */
	if (r->err)
		return REDIS_ERR;

	/* When the buffer is empty, there will never be a reply. */
	if (r->buf->readableBytes() == 0)
		return REDIS_OK;
	/* Set first item to process when the stack is empty. */
		if (r->ridx == -1)
		{
			r->rstack[0].type	= -1;
			r->rstack[0].elements = -1;
			r->rstack[0].idx	= -1;
			r->rstack[0].obj	= nullptr;
			r->rstack[0].parent = nullptr;
			r->rstack[0].privdata = r->privdata;
			r->ridx 			= 0;
		}

	/* Process items in reply. */
		while (r->ridx >= 0)
			if (processItem(r) != REDIS_OK)
				break;

	/* Return ASAP when an error occurred. */
		if (r->err)
			return REDIS_ERR;

		/* Discard part of the buffer when we've consumed at least 1k, to avoid
		 * doing unnecessary calls to memmove() in sds.c. */
		if (r->pos >= 1024)
		{
			r->buf->retrieve(r->pos);
			r->pos				= 0;
		}

		/* Emit a reply when there is one. */
		if (r->ridx == -1)
		{
			if (reply != nullptr)
				*reply = r->reply;

			r->reply = nullptr;
		}

		return REDIS_OK;

}


int redisGetReplyFromReader(const xRedisContextPtr & c, void * *reply)
{
	if (redisReaderGetReply(c->reader, reply) == REDIS_ERR)
	{
		LOG_WARN<<"redisGetReplyFromReader error"<< c->reader->err<<c->reader->errstr;
		return REDIS_ERR;
	}

	return REDIS_OK;
}



int redisBufferWrite(const xRedisContextPtr & c, int * done)
{
	int nwritten;

	/* Return early when the context has seen an error. */
	if (c->err)
		return REDIS_ERR;

	if (c->sender.readableBytes() > 0)
	{
		nwritten =  ::write(c->fd, c->sender.peek(), c->sender.readableBytes());

		if (nwritten  < 0 )
		{
			if ((errno == EAGAIN && ! (c->flags & REDIS_BLOCK)) || (errno == EINTR))
			{
				/* Try again later */
			}
			else
			{
				LOG_WARN<<"redisBufferWrite write error";
				return REDIS_ERR;
			}
		}
		else if (nwritten > 0)
		{
			if (nwritten == (signed) c->sender.readableBytes())
			{
				c->sender.retrieveAll();
			}
			else
			{
				c->sender.retrieve(nwritten);
			}
		}
	}

	if (done != nullptr)
		*done =( c->sender.readableBytes() == 0);

	return REDIS_OK;
}

int redisBufferRead(const xRedisContextPtr & c)
{
	int savedErrno = 0;
	ssize_t n = c->reader->buf->readFd(c->fd, &savedErrno);
	if (n > 0)
	{
		
	}
	else if (n == 0)
	{
		LOG_WARN<<"Server closed the connection";
		return REDIS_ERR;
	}
	else
	{
		if ((errno == EAGAIN && ! (c->flags & REDIS_BLOCK)) || (errno == EINTR))
		{
			/* Try again later */
		}
		else
		{
			LOG_WARN<<"Server closed the connection";
			return REDIS_ERR;
		}
	}

	return REDIS_OK;
}
int redisGetReply(const xRedisContextPtr & c, void * *reply)
{
	int wdone	= 0;
	void * aux	= nullptr;

	/* Try to read pending replies */
	if (redisGetReplyFromReader(c, &aux) == REDIS_ERR)
		return REDIS_ERR;

	/* For the blocking context, flush output buffer and read reply */
	if (aux == nullptr && c->flags & REDIS_BLOCK)
	{
		do
		{
			if (redisBufferWrite(c, &wdone) == REDIS_ERR)
			return REDIS_ERR;
		}
		while(!wdone);


		do
		{
			if (redisBufferRead(c) == REDIS_ERR)
				return REDIS_ERR;

			if (redisGetReplyFromReader(c, &aux) == REDIS_ERR)
				return REDIS_ERR;
		}
		while(aux == nullptr);
	}

	/* Set reply object */
	if (reply != nullptr)
		*reply = aux;

	return REDIS_OK;
}



int __redisAsyncCommand(const xRedisAsyncContextPtr &ac,redisCallbackFn *fn, void *privdata, char *cmd, size_t len)
{
	redisCallback cb;
	int pvariant, hasnext;
	char *cstr, *astr;
	size_t clen, alen;
	char *p;
	sds sname;
	cb.fn = fn;
	cb.privdata = privdata;
	p = nextArgument(cmd,&cstr,&clen);
	assert(p != nullptr);
	hasnext = (p[0] == '$');
	pvariant = (tolower(cstr[0]) == 'p') ? 1 : 0;
	cstr += pvariant;
	clen -= pvariant;
	
	{
		std::unique_lock<std::mutex> lk(mutex);
		ac->replies.push_back(std::move(cb));
		ac->conn->send(stringPiepe(cmd,len));
	}
	
   	return REDIS_OK;
}


int redisvFormatCommand(char * *target, const char * format, va_list ap)
{
	const char * c = format;
	char * cmd = nullptr; 					/* final command */
	int pos;							/* position in final command */
	sds curarg, newarg; 				/* current argument */
	int touched = 0;					/* was the current argument touched? */
	char * * curargv = nullptr, * *newargv = nullptr;
	int argc = 0;
	int totlen = 0;
	int j;

	/* Abort if there is not target to set */
	if (target == nullptr)
		return - 1;
	/* Build the command string accordingly to protocol */
	curarg = sdsempty();
	if (curarg == nullptr)
		return - 1;

	while (*c != '\0')
	{
		if (*c != '%' || c[1] == '\0')
		{
			if (*c == ' ')
			{
				if (touched)
				{
					newargv = (char**) zrealloc(curargv, sizeof(char *) * (argc + 1));
					if (newargv == nullptr)
						goto err;
					curargv  = newargv;
					curargv[argc++] = curarg;
					totlen += bulklen(sdslen(curarg));
					/* curarg is put in argv so it can be overwritten. */
					curarg = sdsempty();
					if (curarg == nullptr)
						goto err;
					touched  = 0;
				}
			}
			else
			{
				newarg = sdscatlen(curarg, c, 1);
				if (newarg == nullptr)
					goto err;
				curarg = newarg;
				touched = 1;
			}
		}
		else
		{
			char * arg;
			size_t	 size;
			/* Set newarg so it can be checked even if it is not touched. */
			newarg = curarg;
			switch (c[1])
			{
				case 's':
					arg = va_arg(ap, char *);
					size = strlen(arg);

					if (size > 0)
						newarg = sdscatlen(curarg, arg, size);
					break;
				case 'b':
					arg = va_arg(ap, char *);
					size = va_arg(ap, size_t);
					if (size > 0)
						newarg = sdscatlen(curarg, arg, size);
					break;
				case '%':
					newarg = sdscat(curarg, "%");
					break;
				default:
				/* Try to detect printf format */
				{
					static const char intfmts[] = "diouxX";
					char	 _format[16];
					const char * _p = c + 1;
					size_t	 _l	= 0;
					va_list  _cpy;

					/* Flags */
					if (*_p != '\0' && *_p == '#')
						_p ++;

					if (*_p != '\0' && *_p == '0')
						_p ++;

					if (*_p != '\0' && *_p == '-')
						_p ++;

					if (*_p != '\0' && *_p == ' ')
						_p ++;

					if (*_p != '\0' && *_p == '+')
						_p ++;

					/* Field width */
					while (*_p != '\0' && isdigit(*_p))
					_p ++;

					/* Precision */
					if (*_p == '.')
					{
						_p ++;
						while (*_p != '\0' && isdigit(*_p))
							_p ++;
					}
					/* Copy va_list before consuming with va_arg */
					va_copy(_cpy, ap);
					/* Integer conversion (without modifiers) */
					if (strchr(intfmts, *_p) != nullptr)
					{
						va_arg(ap, int);
						goto fmt_valid;
					}
					/* Double conversion (without modifiers) */
					if (strchr("eEfFgGaA", *_p) != nullptr)
					{
						va_arg(ap, double);
						goto fmt_valid;
					}
					/* Size: char */
					if (_p[0] == 'h' && _p[1] == 'h')
					{
						_p += 2;
						if (*_p != '\0' && strchr(intfmts, *_p) != nullptr)
						{
							va_arg(ap, int);	/* char gets promoted to int */
							goto fmt_valid;
						}
						goto fmt_invalid;
					}
					/* Size: short */
					if (_p[0] == 'h')
					{
						_p					+= 1;
						if (*_p != '\0' && strchr(intfmts, *_p) != NULL)
						{
							va_arg(ap, int);	/* short gets promoted to int */
							goto fmt_valid;
						}
						goto fmt_invalid;
					}
					/* Size: long long */
					if (_p[0] == 'l' && _p[1] == 'l')
					{
						_p					+= 2;

						if (*_p != '\0' && strchr(intfmts, *_p) != NULL)
						{
							va_arg(ap, long long);
							goto fmt_valid;
						}

						goto fmt_invalid;
					}
					/* Size: long */
					if (_p[0] == 'l')
					{
						_p					+= 1;
						if (*_p != '\0' && strchr(intfmts, *_p) != NULL)
						{
							va_arg(ap, long);
							goto fmt_valid;
						}
						goto fmt_invalid;
					}
	fmt_invalid:
					va_end(_cpy);
					goto err;
	fmt_valid:
					_l = (_p +1) -c;
					if (_l < sizeof(_format) - 2)
					{
						memcpy(_format, c, _l);
						_format[_l] 	 = '\0';
						newarg = sdscatvprintf(curarg, _format, _cpy);

						/* Update current position (note: outer blocks
						 * increment c twice so compensate here) */
						c = _p -1;
					}
					va_end(_cpy);
					break;
				
				}
			}
			
			if (newarg == nullptr)
				goto err;
			curarg = newarg;
			touched = 1;
			c++;
		}
	c++;
	}
	/* Add the last argument if needed */
	if (touched)
	{
		newargv = (char **)zrealloc(curargv, sizeof(char *) * (argc + 1));

		if (newargv == NULL)
			goto err;

		curargv = newargv;
		curargv[argc++] 	= curarg;
		totlen += bulklen(sdslen(curarg));
	}
	else
	{
		sdsfree(curarg);
	}
	/* Clear curarg because it was put in curargv or was free'd. */
	curarg = nullptr;
	/* Add bytes needed to hold multi bulk count */
	totlen += 1 + intlen(argc) + 2;
	/* Build the command at protocol level */
	cmd 	= (char *)zmalloc(totlen + 1);
	if (cmd == nullptr)
		goto err;
	pos = sprintf(cmd, "*%d\r\n", argc);
	for (j = 0; j < argc; j++)
	{
		pos 	+= sprintf(cmd + pos, "$%zu\r\n", sdslen(curargv[j]));
		memcpy(cmd + pos, curargv[j], sdslen(curargv[j]));
		pos 	+= sdslen(curargv[j]);
		sdsfree(curargv[j]);
		cmd[pos++]	= '\r';
		cmd[pos++]	= '\n';
	}
	assert(pos == totlen);
	cmd[pos]			= '\0';
	zfree(curargv);
	*target 			= cmd;
	return totlen;
err:
	while (argc--)
		sdsfree(curargv[argc]);
	zfree(curargv);
	if (curarg != nullptr)
		sdsfree(curarg);

	if (cmd != nullptr)
		zfree(cmd);
	return - 1;

}


int redisFormatCommand(char **target, const char *format, ...)
{
    va_list ap;
    int len;
    va_start(ap,format);
    len = redisvFormatCommand(target,format,ap);
    va_end(ap);

    /* The API says "-1" means bad result, but we now also return "-2" in some
     * cases.  Force the return value to always be -1. */
    if (len < 0)
        len = -1;

    return len;
}


xRedisReader::xRedisReader()
{
	pos = 0;
	err = 0;
	errstr[0] = '\0';
	fn = std::shared_ptr<redisReplyObjectFunctions>(new redisReplyObjectFunctions());
	ridx 	= -1;

}




int redisvAsyncCommand(const xRedisAsyncContextPtr &ac,redisCallbackFn *fn, void *privdata, const char *format, va_list ap)
{
	char *cmd;
	int len;
	int status;
	len = redisvFormatCommand(&cmd,format,ap);
	status = __redisAsyncCommand(ac,fn,privdata,cmd,len);
	zfree(cmd);
	return status;
}

int redisAsyncCommand(const xRedisAsyncContextPtr &ac,redisCallbackFn *fn, void *privdata, const char *format, ...)
{
	va_list ap;
	int status;
	va_start(ap,format);
	status = redisvAsyncCommand(ac,fn,privdata,format,ap);
	va_end(ap);
	return status;
}


static void * __redisBlockForReply(const xRedisContextPtr & c)
{
	void * reply;

	if (c->flags & REDIS_BLOCK)
	{
		if (redisGetReply(c, &reply) != REDIS_OK)
			return NULL;

		return reply;
	}

	return NULL;
}


int __redisAppendCommand(const xRedisContextPtr & c, const char * cmd, size_t len)
{
	c->sender.append(cmd,len);
	return REDIS_OK;
}

int redisvAppendCommand(const xRedisContextPtr  & c, const char * format, va_list ap)
{
	char * cmd;
	int len;

	len = redisvFormatCommand(&cmd, format, ap);

	if (len == -1)
	{
		LOG_WARN<<"Out of memory";
		return REDIS_ERR;
	}

	if (__redisAppendCommand(c, cmd, len) != REDIS_OK)
	{
		zfree(cmd);
		return REDIS_ERR;
	}

	zfree(cmd);
	return REDIS_OK;
}


void * redisCommand(const xRedisContextPtr & c, const char * format, ...)
{
	va_list ap;
	void * reply	 = nullptr;
	va_start(ap, format);
	reply	= redisvCommand(c, format, ap);
	va_end(ap);
	return reply;
}

void * redisvCommand(const xRedisContextPtr & c, const char * format, va_list ap)
{

	if (redisvAppendCommand(c, format, ap) != REDIS_OK)
		return nullptr;

	  return __redisBlockForReply(c);
}



int redisFormatCommandArgv(char * *target, int argc, const char * *argv, const size_t * argvlen)
{
	char * cmd = nullptr; 					/* final command */
	int pos;							/* position in final command */
	size_t	len;
	int totlen, j;

	/* Calculate number of bytes needed for the command */
	totlen = 1 + intlen(argc) + 2;

	for (j = 0; j < argc; j++)
	{
		len = argvlen ? argvlen[j]: strlen(argv[j]);
		totlen += bulklen(len);
	}

	/* Build the command at protocol level */
	cmd = (char*)zmalloc(totlen + 1);

	if (cmd == NULL)
		return - 1;

	pos = sprintf(cmd, "*%d\r\n", argc);

	for (j = 0; j < argc; j++)
	{
		len = argvlen ? argvlen[j]: strlen(argv[j]);
		pos += sprintf(cmd + pos, "$%zu\r\n", len);
		memcpy(cmd + pos, argv[j], len);
		pos += len;
		cmd[pos++] = '\r';
		cmd[pos++] = '\n';
	}

	assert(pos == totlen);
	cmd[pos] = '\0';

	*target = cmd;
	return totlen;
}


int redisAppendCommandArgv(const xRedisContextPtr  & c, int argc, const char * *argv, const size_t * argvlen)
{
	char * cmd;
	int len;

	len = redisFormatCommandArgv(&cmd, argc, argv, argvlen);

	if (len == -1)
	{
		LOG_WARN<<"Out of memory";
		return REDIS_ERR;
	}

	if (__redisAppendCommand(c, cmd, len) != REDIS_OK)
	{
		zfree(cmd);
		return REDIS_ERR;
	}

	zfree(cmd);
	return REDIS_OK;
}


void * redisCommandArgv(const xRedisContextPtr & c, int argc, const char * *argv, const size_t * argvlen)
{
	if (redisAppendCommandArgv(c, argc, argv, argvlen) != REDIS_OK)
			return nullptr;

	return __redisBlockForReply(c);
}
