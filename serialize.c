#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include "lz4.h"

#define TYPE_NIL 0
#define TYPE_BOOLEAN 1
// hibits 0 false 1 true

#define TYPE_NUMBER 2
// hibits 0 : 0 , 1: byte, 2:word, 4: dword, 6: qword, 8 : double
#define TYPE_NUMBER_ZERO 0
#define TYPE_NUMBER_BYTE 1
#define TYPE_NUMBER_WORD 2
#define TYPE_NUMBER_DWORD 4
#define TYPE_NUMBER_QWORD 6
#define TYPE_NUMBER_REAL 8

#define TYPE_USERDATA 3
#define TYPE_SHORT_STRING 4
// hibits 0~31 : len
#define TYPE_LONG_STRING 5
#define TYPE_TABLE 6

#define MAX_COOKIE 32
#define COMBINE_TYPE(t,v) ((t) | (v) << 3)

#define BLOCK_SIZE 128
#define MAX_DEPTH 32

struct block {
	struct block * next;
	char buffer[BLOCK_SIZE];
};

struct write_block {
	struct block * head;
	int len;
	struct block * current;
	int ptr;
};

struct read_block {
	char * buffer;
	struct block * current;
	int len;
	int ptr;
};

inline static struct block *
blk_alloc(void) {
	struct block *b = malloc(sizeof(struct block));
	b->next = NULL;
	return b;
}

inline static void
wb_push(struct write_block *b, const void *buf, int sz) {
	const char * buffer = buf;
	if (b->ptr == BLOCK_SIZE) {
_again:
		b->current = b->current->next = blk_alloc();
		b->ptr = 0;
	}
	if (b->ptr <= BLOCK_SIZE - sz) {
		memcpy(b->current->buffer + b->ptr, buffer, sz);
		b->ptr+=sz;
		b->len+=sz;
	} else {
		int copy = BLOCK_SIZE - b->ptr;
		memcpy(b->current->buffer + b->ptr, buffer, copy);
		buffer += copy;
		b->len += copy;
		sz -= copy;
		goto _again;
	}
}

static void
wb_init(struct write_block *wb , struct block *b) {
	if (b==NULL) {
		wb->head = blk_alloc();
		wb->len = 0;
		wb->current = wb->head;
		wb->ptr = 0;
		wb_push(wb, &wb->len, sizeof(wb->len));
	} else {
		wb->head = b;
		int * plen = (int *)b->buffer;
		int sz = *plen;
		wb->len = sz;
		while (b->next) {
			sz -= BLOCK_SIZE;		
			b = b->next;
		}
		wb->current = b;
		wb->ptr = sz;
	}
}

static struct block *
wb_close(struct write_block *b) {
	b->current = b->head;
	b->ptr = 0;
	wb_push(b, &b->len, sizeof(b->len));
	b->current = NULL;
	return b->head;
}

static void
wb_free(struct write_block *wb) {
	struct block *blk = wb->head;
	while (blk) {
		struct block * next = blk->next;
		free(blk);
		blk = next;
	}
	wb->head = NULL;
	wb->current = NULL;
	wb->ptr = 0;
	wb->len = 0;
}

static void
rball_init(struct read_block * rb, char * buffer) {
	rb->buffer = buffer;
	rb->current = NULL;
	uint8_t header[4];
	memcpy(header,buffer,4);
	rb->len = header[0] | header[1] <<8 | header[2] << 16 | header[3] << 24;
	rb->ptr = 4;
	rb->len -= rb->ptr;
}

static int
rb_init(struct read_block *rb, struct block *b) {
	rb->buffer = NULL;
	rb->current = b;
	memcpy(&(rb->len),b->buffer,sizeof(rb->len));
	rb->ptr = sizeof(rb->len);
	rb->len -= rb->ptr;
	return rb->len;
}

static void *
rb_read(struct read_block *rb, void *buffer, int sz) {
	if (rb->len < sz) {
		return NULL;
	}

	if (rb->buffer) {
		int ptr = rb->ptr;
		rb->ptr += sz;
		rb->len -= sz;
		return rb->buffer + ptr;
	}

	if (rb->ptr == BLOCK_SIZE) {
		struct block * next = rb->current->next;
		free(rb->current);
		rb->current = next;
		rb->ptr = 0;
	}

	int copy = BLOCK_SIZE - rb->ptr;

	if (sz <= copy) {
		void * ret = rb->current->buffer + rb->ptr;
		rb->ptr += sz;
		rb->len -= sz;
		return ret;
	}

	char * tmp = buffer;

	memcpy(tmp, rb->current->buffer + rb->ptr,  copy);
	sz -= copy;
	tmp += copy;
	rb->len -= copy;

	for (;;) {
		struct block * next = rb->current->next;
		free(rb->current);
		rb->current = next;

		if (sz < BLOCK_SIZE) {
			memcpy(tmp, rb->current->buffer, sz);
			rb->ptr = sz;
			rb->len -= sz;
			return buffer;
		}
		memcpy(tmp, rb->current->buffer, BLOCK_SIZE);
		sz -= BLOCK_SIZE;
		tmp += BLOCK_SIZE;
		rb->len -= BLOCK_SIZE;
	}
}

static void
rb_close(struct read_block *rb) {
	while (rb->current) {
		struct block * next = rb->current->next;
		free(rb->current);
		rb->current = next;
	}
	rb->len = 0;
	rb->ptr = 0;
}

static inline void
wb_nil(struct write_block *wb) {
	int n = TYPE_NIL;
	wb_push(wb, &n, 1);
}

static inline void
wb_boolean(struct write_block *wb, int boolean) {
	int n = COMBINE_TYPE(TYPE_BOOLEAN , boolean ? 1 : 0);
	wb_push(wb, &n, 1);
}

static inline void
wb_integer(struct write_block *wb, lua_Integer v) {
	int type = TYPE_NUMBER;
	if (v == 0) {
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_ZERO);
		wb_push(wb, &n, 1);
	} else if (v != (int32_t)v) {
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_QWORD);
		int64_t v64 = v;
		wb_push(wb, &n, 1);
		wb_push(wb, &v64, sizeof(v64));
	} else if (v < 0) {
		int32_t v32 = (int32_t)v;
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_DWORD);
		wb_push(wb, &n, 1);
		wb_push(wb, &v32, sizeof(v32));
	} else if (v<0x100) {
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_BYTE);
		wb_push(wb, &n, 1);
		uint8_t byte = (uint8_t)v;
		wb_push(wb, &byte, sizeof(byte));
	} else if (v<0x10000) {
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_WORD);
		wb_push(wb, &n, 1);
		uint16_t word = (uint16_t)v;
		wb_push(wb, &word, sizeof(word));
	} else {
		uint8_t n = COMBINE_TYPE(type , TYPE_NUMBER_DWORD);
		wb_push(wb, &n, 1);
		uint32_t v32 = (uint32_t)v;
		wb_push(wb, &v32, sizeof(v32));
	}
}

static inline void
wb_real(struct write_block *wb, double v) {
	uint8_t n = COMBINE_TYPE(TYPE_NUMBER , TYPE_NUMBER_REAL);
	wb_push(wb, &n, 1);
	wb_push(wb, &v, sizeof(v));
}

static inline void
wb_pointer(struct write_block *wb, void *v) {
	int n = TYPE_USERDATA;
	wb_push(wb, &n, 1);
	wb_push(wb, &v, sizeof(v));
}

static inline void
wb_string(struct write_block *wb, const char *str, int len) {
	if (len < MAX_COOKIE) {
		int n = COMBINE_TYPE(TYPE_SHORT_STRING, len);
		wb_push(wb, &n, 1);
		if (len > 0) {
			wb_push(wb, str, len);
		}
	} else {
		int n;
		if (len < 0x10000) {
			n = COMBINE_TYPE(TYPE_LONG_STRING, 2);
			wb_push(wb, &n, 1);
			uint16_t x = (uint16_t) len;
			wb_push(wb, &x, 2);
		} else {
			n = COMBINE_TYPE(TYPE_LONG_STRING, 4);
			wb_push(wb, &n, 1);
			uint32_t x = (uint32_t) len;
			wb_push(wb, &x, 4);
		}
		wb_push(wb, str, len);
	}
}

static void pack_one(lua_State *L, struct write_block *b, int index, int depth);

static int
wb_table_array(lua_State *L, struct write_block * wb, int index, int depth) {
	int array_size = lua_rawlen(L,index);
	if (array_size >= MAX_COOKIE-1) {
		int n = COMBINE_TYPE(TYPE_TABLE, MAX_COOKIE-1);
		wb_push(wb, &n, 1);
		wb_integer(wb, array_size);
	} else {
		int n = COMBINE_TYPE(TYPE_TABLE, array_size);
		wb_push(wb, &n, 1);
	}

	int i;
	for (i=1;i<=array_size;i++) {
		lua_rawgeti(L,index,i);
		pack_one(L, wb, -1, depth);
		lua_pop(L,1);
	}

	return array_size;
}

static void
wb_table_hash(lua_State *L, struct write_block * wb, int index, int depth, int array_size) {
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		if (lua_type(L,-2) == LUA_TNUMBER) {
			if (lua_isinteger(L, -2)) {
				lua_Integer x = lua_tointeger(L,-2);
				if (x>0 && x<=array_size) {
					lua_pop(L,1);
					continue;
				}
			}
		}
		pack_one(L,wb,-2,depth);
		pack_one(L,wb,-1,depth);
		lua_pop(L, 1);
	}
	wb_nil(wb);
}

static void
wb_table(lua_State *L, struct write_block *wb, int index, int depth) {
	luaL_checkstack(L,LUA_MINSTACK,NULL);
	if (index < 0) {
		index = lua_gettop(L) + index + 1;
	}
	int array_size = wb_table_array(L, wb, index, depth);
	wb_table_hash(L, wb, index, depth, array_size);
}

#if LUA_VERSION_NUM < 503

static int
lua_isinteger(lua_State *L, int index) {
	int32_t x = (int32_t)lua_tointeger(L,index);
	lua_Number n = lua_tonumber(L,index);
	return ((lua_Number)x==n);
}

#endif

static void
pack_one(lua_State *L, struct write_block *b, int index, int depth) {
	if (depth > MAX_DEPTH) {
		wb_free(b);
		luaL_error(L, "serialize can't pack too depth table");
	}
	int type = lua_type(L,index);
	switch(type) {
	case LUA_TNIL:
		wb_nil(b);
		break;
	case LUA_TNUMBER: {
		if (lua_isinteger(L, index)) {
			lua_Integer x = lua_tointeger(L,index);
			wb_integer(b, x);
		} else {
			lua_Number n = lua_tonumber(L,index);
			wb_real(b,n);
		}
		break;
	}
	case LUA_TBOOLEAN: 
		wb_boolean(b, lua_toboolean(L,index));
		break;
	case LUA_TSTRING: {
		size_t sz = 0;
		const char *str = lua_tolstring(L,index,&sz);
		wb_string(b, str, (int)sz);
		break;
	}
	case LUA_TLIGHTUSERDATA:
		wb_pointer(b, lua_touserdata(L,index));
		break;
	case LUA_TTABLE: {
		if (index < 0) {
			index = lua_gettop(L) + index + 1;
		}
		wb_table(L, b, index, depth+1);
		break;
	}
	default:
		wb_free(b);
		luaL_error(L, "Unsupport type %s to serialize", lua_typename(L, type));
	}
}

static void
pack_from(lua_State *L, struct write_block *b, int from) {
	int n = lua_gettop(L) - from;
	int i;
	for (i=1;i<=n;i++) {
		pack_one(L, b , from + i, 0);
	}
}

static int
lpack(lua_State *L) {
	struct write_block b;
	wb_init(&b, NULL);
	pack_from(L,&b,0);
	struct block * ret = wb_close(&b);
	lua_pushlightuserdata(L,ret);
	return 1;
}

static int
lappend(lua_State *L) {
	struct write_block b;
	wb_init(&b, lua_touserdata(L,1));
	pack_from(L,&b,1);
	struct block * ret = wb_close(&b);
	lua_pushlightuserdata(L,ret);
	return 1;
}

static inline void
invalid_stream_line(lua_State *L, struct read_block *rb, int line) {
	int len = rb->len;
	luaL_error(L, "Invalid serialize stream %d (line:%d)", len, line);
}

#define invalid_stream(L,rb) invalid_stream_line(L,rb,__LINE__)

static lua_Integer
get_integer(lua_State *L, struct read_block *rb, int cookie) {
	switch (cookie) {
	case TYPE_NUMBER_ZERO:
		return 0;
	case TYPE_NUMBER_BYTE: {
		uint8_t n = 0;
		uint8_t * pn = rb_read(rb, &n, sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		n = *pn;
		return n;
	}
	case TYPE_NUMBER_WORD: {
		uint16_t n = 0;
		uint16_t * pn = rb_read(rb, &n, sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	case TYPE_NUMBER_DWORD: {
		int32_t n = 0;
		int32_t * pn = rb_read(rb, &n, sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	case TYPE_NUMBER_QWORD: {
		int64_t n=0;
		int64_t * pn = rb_read(rb, &n, sizeof(n));
		if (pn == NULL)
			invalid_stream(L,rb);
		memcpy(&n, pn, sizeof(n));
		return n;
	}
	default:
		invalid_stream(L,rb);
		return 0;
	}
}

static double
get_real(lua_State *L, struct read_block *rb) {
	double n = 0;
	double * pn = rb_read(rb, &n, sizeof(n));
	if (pn == NULL)
		invalid_stream(L,rb);
	memcpy(&n, pn, sizeof(n));
	return n;
}

static void *
get_pointer(lua_State *L, struct read_block *rb) {
	void * userdata = 0;
	void ** v = (void **)rb_read(rb,&userdata,sizeof(userdata));
	if (v == NULL) {
		invalid_stream(L,rb);
	}
	return *v;
}

static void
get_buffer(lua_State *L, struct read_block *rb, int len) {
	char tmp[len];
	char * p = rb_read(rb,tmp,len);
	if (p == NULL) {
		invalid_stream(L, rb);
	}
	lua_pushlstring(L,p,len);
}

static void unpack_one(lua_State *L, struct read_block *rb);

static void
unpack_table(lua_State *L, struct read_block *rb, int array_size) {
	if (array_size == MAX_COOKIE-1) {
		uint8_t type = 0;
		uint8_t *t = rb_read(rb, &type, sizeof(type));
		if (t==NULL) {
			invalid_stream(L,rb);
		}
		type = *t;
		int cookie = type >> 3;
		if ((type & 7) != TYPE_NUMBER || cookie == TYPE_NUMBER_REAL) {
			invalid_stream(L,rb);
		}
		array_size = get_integer(L,rb,cookie);
	}
	luaL_checkstack(L,LUA_MINSTACK,NULL);
	lua_createtable(L,array_size,0);
	int i;
	for (i=1;i<=array_size;i++) {
		unpack_one(L,rb);
		lua_rawseti(L,-2,i);
	}
	for (;;) {
		unpack_one(L,rb);
		if (lua_isnil(L,-1)) {
			lua_pop(L,1);
			return;
		}
		unpack_one(L,rb);
		lua_rawset(L,-3);
	}
}

static void
push_value(lua_State *L, struct read_block *rb, int type, int cookie) {
	switch(type) {
	case TYPE_NIL:
		lua_pushnil(L);
		break;
	case TYPE_BOOLEAN:
		lua_pushboolean(L,cookie);
		break;
	case TYPE_NUMBER:
		if (cookie == TYPE_NUMBER_REAL) {
			lua_pushnumber(L,get_real(L,rb));
		} else {
			lua_pushinteger(L, get_integer(L, rb, cookie));
		}
		break;
	case TYPE_USERDATA:
		lua_pushlightuserdata(L,get_pointer(L,rb));
		break;
	case TYPE_SHORT_STRING:
		get_buffer(L,rb,cookie);
		break;
	case TYPE_LONG_STRING: {
		uint32_t len = 0;
		if (cookie == 2) {
			uint16_t *plen = rb_read(rb, &len, 2);
			if (plen == NULL) {
				invalid_stream(L,rb);
			}
			uint16_t n;
			memcpy(&n, plen, sizeof(n));
			get_buffer(L,rb,n);
		} else {
			if (cookie != 4) {
				invalid_stream(L,rb);
			}
			uint32_t *plen = rb_read(rb, &len, 4);
			if (plen == NULL) {
				invalid_stream(L,rb);
			}
			uint32_t n;
			memcpy(&n, plen, sizeof(n));
			get_buffer(L,rb,n);
		}
		break;
	}
	case TYPE_TABLE: {
		unpack_table(L,rb,cookie);
		break;
	}
	default: {
		invalid_stream(L,rb);
		break;
	}
	}
}

static void
unpack_one(lua_State *L, struct read_block *rb) {
	uint8_t type = 0;
	uint8_t *t = rb_read(rb, &type, sizeof(type));
	if (t==NULL) {
		invalid_stream(L, rb);
	}
	type = *t;
	push_value(L, rb, type & 0x7, type>>3);
}

static int
lunpack(lua_State *L) {
	struct block * blk = lua_touserdata(L,1);
	if (blk == NULL) {
		return luaL_error(L, "Need a block to unpack");
	}
	lua_settop(L,0);
	struct read_block rb;
	rb_init(&rb, blk);

	int i;
	for (i=0;;i++) {
		if (i%8==7) {
			luaL_checkstack(L,LUA_MINSTACK,NULL);
		}
		uint8_t type = 0;
		uint8_t *t = rb_read(&rb, &type, 1);
		if (t==NULL)
			break;
		push_value(L, &rb, *t & 0x7, *t>>3);
	}

	rb_close(&rb);

	return lua_gettop(L);
}

static int
_dump_mem(const char * buffer, int len, int size) {
	int i;
	for (i=0;i<len && i<size;i++) {
		printf("%02x ",(unsigned char)buffer[i]);
	}
	return size - len;
}

static int
_dump(lua_State *L) {
	struct block *b = lua_touserdata(L,1);
	if (b==NULL) {
		return luaL_error(L, "dump null pointer");
	}
	int len = 0;
	memcpy(&len, b->buffer ,sizeof(len));
	len -= sizeof(len);
	printf("Len = %d\n",len);
	len = _dump_mem(b->buffer + sizeof(len), BLOCK_SIZE - sizeof(len) , len);
	while (len > 0) {
		b=b->next;
		len = _dump_mem(b->buffer, BLOCK_SIZE , len);
	}
	printf("\n");
	return 0;
}

static int
lserialize(lua_State *L) {
	struct block *b = lua_touserdata(L,1);
	if (b==NULL) {
		return luaL_error(L, "dump null pointer");
	}

	uint32_t len = 0;
	memcpy(&len, b->buffer ,sizeof(len));

	uint8_t * buffer = malloc(len);
	uint8_t * ptr = buffer;
	int sz = len;
	while(len>0) {
		if (len >= BLOCK_SIZE) {
			memcpy(ptr, b->buffer, BLOCK_SIZE);
			ptr += BLOCK_SIZE;
			len -= BLOCK_SIZE;
		} else {
			memcpy(ptr, b->buffer, len);
			break;
		}
		b = b->next;
	}

	buffer[0] = sz & 0xff;
	buffer[1] = (sz>>8) & 0xff;
	buffer[2] = (sz>>16) & 0xff;
	buffer[3] = (sz>>24) & 0xff;
	
	lua_pushlightuserdata(L, buffer);
	lua_pushinteger(L, sz);

	return 2;
}

static void
deserialize_buffer(lua_State *L, void * buffer) {
	struct read_block rb;
	rball_init(&rb, buffer);

	int i;
	for (i=0;;i++) {
		if (i%16==15) {
			lua_checkstack(L,i);
		}
		uint8_t type = 0;
		uint8_t *t = rb_read(&rb, &type, 1);
		if (t==NULL)
			break;
		push_value(L, &rb, *t & 0x7, *t>>3);
	}
}

static int
ldeserialize(lua_State *L) {
	void * buffer = lua_touserdata(L,1);
	if (buffer == NULL) {
		return luaL_error(L, "deserialize null pointer");
	}

	lua_settop(L,0);
	deserialize_buffer(L, buffer);

	// Need not free buffer

	return lua_gettop(L);
}

static int
seristring(lua_State *L) {
	struct write_block b;
	wb_init(&b, NULL);
	pack_from(L,&b,0);
	struct block * ret = wb_close(&b);
	lua_settop(L,0);
	lua_pushlightuserdata(L,ret);
	lserialize(L);
	wb_free(&b);
	void *buffer = lua_touserdata(L, -2);
	int sz = lua_tointeger(L, -1);
	lua_pushlstring(L, buffer, sz);
	free(buffer);
	return 1;
}

static int
deseristring(lua_State *L) {
	const char * buffer = luaL_checkstring(L, 1);
	deserialize_buffer(L, (void *)buffer);

	return lua_gettop(L) - 1;
}

static int
seristring_lz4(lua_State *L) {
	struct write_block b;
	wb_init(&b, NULL);
	pack_from(L,&b,0);
	struct block * ret = wb_close(&b);
	lua_settop(L,0);
	lua_pushlightuserdata(L,ret);
	lserialize(L);
	wb_free(&b);
	void *buffer = lua_touserdata(L, -2);
	int sz = lua_tointeger(L, -1);

	if (sz < 1024) {
		char *out = malloc(1 + sz);
		if (!out) {
			free(buffer);
			return luaL_error(L, "out of memory");
		}
		out[0] = 0x00;
		memcpy(out + 1, buffer, sz);
		lua_pushlstring(L, out, 1 + sz);
		free(buffer);
		free(out);
		return 1;
	}

	int bound = LZ4_compressBound(sz);
	char *zbuf = malloc(bound);
	if (!zbuf) {
		free(buffer);
		return luaL_error(L, "out of memory");
	}

	int csize = LZ4_compress_default(buffer, zbuf, sz, bound);
	if (csize <= 0) {
		free(buffer);
		free(zbuf);
		return luaL_error(L, "LZ4 compression failed");
	}

	char *out = malloc(5 + csize);
	if (!out) {
		free(buffer);
		free(zbuf);
		return luaL_error(L, "out of memory");
	}
	out[0] = 0x80;
	memcpy(out + 1, &sz, 4);
	memcpy(out + 5, zbuf, csize);
	lua_pushlstring(L, out, 5 + csize);
	free(buffer);
	free(zbuf);
	free(out);
	return 1;
}

static int
deseristring_lz4(lua_State *L) {
	size_t src_len;
	const char *src = luaL_checklstring(L, 1, &src_len);

	if (src_len < 1) {
		return luaL_error(L, "Invalid compressed data");
	}

	switch ((unsigned char)src[0]) {
	case 0x00: {
		deserialize_buffer(L, (void *)(src + 1));
		break;
	}
	case 0x80: {
		if (src_len < 5) {
			return luaL_error(L, "Invalid compressed data");
		}
		uint32_t sz;
		memcpy(&sz, src + 1, 4);
		char *dst = malloc(sz);
		if (!dst) {
			return luaL_error(L, "out of memory");
		}
		int ret = LZ4_decompress_safe(src + 5, dst, (int)(src_len - 5), (int)sz);
		if (ret < 0) {
			free(dst);
			return luaL_error(L, "LZ4 decompression failed");
		}
		deserialize_buffer(L, dst);
		free(dst);
		break;
	}
	default:
		return luaL_error(L, "Unknown compression tag %x", (unsigned char)src[0]);
	}

	return lua_gettop(L) - 1;
}

/* --- pure C DES-CBC decryption (standard FIPS 46-3) --- */

#define GET_BIT(a, p)  (((a)[(p) >> 3] >> (7 - ((p) & 7))) & 1)
#define SET_BIT(a, p, v) do { \
	if (v) (a)[(p) >> 3] |=  (1 << (7 - ((p) & 7))); \
	else   (a)[(p) >> 3] &= ~(1 << (7 - ((p) & 7))); \
} while(0)

static const uint8_t des_ip[64] = {
	58,50,42,34,26,18,10,2, 60,52,44,36,28,20,12,4,
	62,54,46,38,30,22,14,6, 64,56,48,40,32,24,16,8,
	57,49,41,33,25,17,9,1,  59,51,43,35,27,19,11,3,
	61,53,45,37,29,21,13,5, 63,55,47,39,31,23,15,7
};
static const uint8_t des_fp[64] = {
	40,8,48,16,56,24,64,32, 39,7,47,15,55,23,63,31,
	38,6,46,14,54,22,62,30, 37,5,45,13,53,21,61,29,
	36,4,44,12,52,20,60,28, 35,3,43,11,51,19,59,27,
	34,2,42,10,50,18,58,26, 33,1,41,9,49,17,57,25
};
static const uint8_t des_pc1[56] = {
	57,49,41,33,25,17,9, 1,58,50,42,34,26,18,
	10,2,59,51,43,35,27, 19,11,3,60,52,44,36,
	63,55,47,39,31,23,15, 7,62,54,46,38,30,22,
	14,6,61,53,45,37,29, 21,13,5,28,20,12,4
};
static const uint8_t des_pc2[48] = {
	14,17,11,24,1,5, 3,28,15,6,21,10,
	23,19,12,4,26,8, 16,7,27,20,13,2,
	41,52,31,37,47,55, 30,40,51,45,33,48,
	44,49,39,56,34,53, 46,42,50,36,29,32
};
static const uint8_t des_e[48] = {
	32,1,2,3,4,5, 4,5,6,7,8,9,
	8,9,10,11,12,13, 12,13,14,15,16,17,
	16,17,18,19,20,21, 20,21,22,23,24,25,
	24,25,26,27,28,29, 28,29,30,31,32,1
};
static const uint8_t des_p[32] = {
	16,7,20,21,29,12,28,17, 1,15,23,26,5,18,31,10,
	2,8,24,14,32,27,3,9, 19,13,30,6,22,11,4,25
};
static const uint8_t des_rot[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};

static const uint8_t des_s[8][64] = {
	{14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,
	 0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
	 4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,
	 15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
	{15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,
	 3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
	 0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,
	 13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
	{10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,
	 13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
	 13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,
	 1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
	{7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,
	 13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
	 10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,
	 3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
	{2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,
	 14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
	 4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,
	 11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
	{12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,
	 10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
	 9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,
	 4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
	{4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,
	 13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
	 1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,
	 6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
	{13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,
	 1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
	 7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,
	 2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}
};

static void
permute(const uint8_t *in, uint8_t *out, const uint8_t *tbl, int n) {
	memset(out, 0, (n + 7) >> 3);
	for (int i = 0; i < n; i++)
		if (GET_BIT(in, tbl[i] - 1)) SET_BIT(out, i, 1);
}

static void
des_f(const uint8_t r[4], const uint8_t k[6], uint8_t out[4]) {
	uint8_t er[6];
	permute(r, er, des_e, 48);
	for (int i = 0; i < 6; i++) er[i] ^= k[i];

	uint8_t sb[4] = {0};
	for (int i = 0; i < 8; i++) {
		int bo = i * 6;
		int val = 0;
		for (int j = 0; j < 6; j++)
			val = (val << 1) | GET_BIT(er, bo + j);
		int row = ((val >> 4) & 2) | (val & 1);
		int col = (val >> 1) & 0xF;
		int sval = des_s[i][row * 16 + col];
		int ob = i * 4;
		for (int j = 3; j >= 0; j--) {
			if (sval & (1 << j)) SET_BIT(sb, ob, 1);
			ob++;
		}
	}
	permute(sb, out, des_p, 32);
}

static void
des_key_schedule(const uint8_t key[8], uint8_t ks[16][6]) {
	uint8_t cd[7];
	permute(key, cd, des_pc1, 56);

	for (int i = 0; i < 16; i++) {
		for (int r = 0; r < des_rot[i]; r++) {
			int c0 = GET_BIT(cd, 0);
			for (int j = 0; j < 27; j++) {
				if (GET_BIT(cd, j + 1)) SET_BIT(cd, j, 1);
				else SET_BIT(cd, j, 0);
			}
			if (c0) SET_BIT(cd, 27, 1); else SET_BIT(cd, 27, 0);

			int d0 = GET_BIT(cd, 28);
			for (int j = 28; j < 55; j++) {
				if (GET_BIT(cd, j + 1)) SET_BIT(cd, j, 1);
				else SET_BIT(cd, j, 0);
			}
			if (d0) SET_BIT(cd, 55, 1); else SET_BIT(cd, 55, 0);
		}
		permute(cd, ks[i], des_pc2, 48);
	}
}

static void
des_decrypt_block(const uint8_t ks[16][6], const uint8_t block[8], uint8_t out[8]) {
	uint8_t ip[8];
	permute(block, ip, des_ip, 64);

	uint8_t l[4], r[4];
	memcpy(l, ip, 4);
	memcpy(r, ip + 4, 4);

	for (int i = 0; i < 16; i++) {
		uint8_t fout[4];
		des_f(r, ks[15 - i], fout);
		uint8_t nl[4];
		memcpy(nl, r, 4);
		for (int j = 0; j < 4; j++) r[j] = l[j] ^ fout[j];
		memcpy(l, nl, 4);
	}

	uint8_t pre[8];
	memcpy(pre, r, 4);
	memcpy(pre + 4, l, 4);
	permute(pre, out, des_fp, 64);
}

static void
des_encrypt_block(const uint8_t ks[16][6], const uint8_t block[8], uint8_t out[8]) {
	uint8_t ip[8];
	permute(block, ip, des_ip, 64);

	uint8_t l[4], r[4];
	memcpy(l, ip, 4);
	memcpy(r, ip + 4, 4);

	for (int i = 0; i < 16; i++) {
		uint8_t fout[4];
		des_f(r, ks[i], fout);
		uint8_t nl[4];
		memcpy(nl, r, 4);
		for (int j = 0; j < 4; j++) r[j] = l[j] ^ fout[j];
		memcpy(l, nl, 4);
	}

	uint8_t pre[8];
	memcpy(pre, r, 4);
	memcpy(pre + 4, l, 4);
	permute(pre, out, des_fp, 64);
}

static int
des_cbc_decrypt(const uint8_t key[8], const uint8_t iv[8],
                const uint8_t *data, int len, uint8_t **out) {
	if (len % 8 != 0 || len < 8) return -1;

	uint8_t ks[16][6];
	des_key_schedule(key, ks);

	*out = (uint8_t *)malloc(len);
	if (!*out) return -1;

	uint8_t prev[8];
	memcpy(prev, iv, 8);

	for (int i = 0; i < len; i += 8) {
		uint8_t dec[8];
		des_decrypt_block(ks, data + i, dec);
		for (int j = 0; j < 8; j++) {
			(*out)[i + j] = dec[j] ^ prev[j];
		}
		memcpy(prev, data + i, 8);
	}

	int pad = (*out)[len - 1];
	if (pad < 1 || pad > 8) { free(*out); *out = NULL; return -1; }
	for (int i = len - pad; i < len; i++)
		if ((*out)[i] != pad) { free(*out); *out = NULL; return -1; }

	return len - pad;
}

static int
des_cbc_encrypt(const uint8_t key[8], const uint8_t iv[8],
                const uint8_t *data, int len, uint8_t **out) {
	int pad = 8 - (len % 8);
	if (pad == 0) pad = 8;
	int out_len = len + pad;

	*out = (uint8_t *)malloc(out_len);
	if (!*out) return -1;

	uint8_t ks[16][6];
	des_key_schedule(key, ks);

	memcpy(*out, data, len);
	for (int i = len; i < out_len; i++)
		(*out)[i] = pad;

	uint8_t prev[8];
	memcpy(prev, iv, 8);

	for (int i = 0; i < out_len; i += 8) {
		uint8_t block[8];
		for (int j = 0; j < 8; j++)
			block[j] = (*out)[i + j] ^ prev[j];
		des_encrypt_block(ks, block, (*out) + i);
		memcpy(prev, (*out) + i, 8);
	}

	return out_len;
}

static int
ldes_decrypt_cbc(lua_State *L) {
	size_t klen, ilen, dlen;
	const char *key  = luaL_checklstring(L, 1, &klen);
	const char *iv   = luaL_checklstring(L, 2, &ilen);
	const char *data = luaL_checklstring(L, 3, &dlen);

	if (klen != 8 || ilen != 8)
		return luaL_error(L, "key and iv must be 8 bytes");
	if (dlen % 8 != 0)
		return luaL_error(L, "data length must be a multiple of 8");

	uint8_t *out = NULL;
	int out_len = des_cbc_decrypt((const uint8_t *)key, (const uint8_t *)iv,
	                              (const uint8_t *)data, (int)dlen, &out);
	if (out_len < 0 || !out)
		return luaL_error(L, "DES-CBC decryption failed");

	lua_pushlstring(L, (const char *)out, out_len);
	free(out);
	return 1;
}

static int
ldes_encrypt_cbc(lua_State *L) {
	size_t klen, ilen, dlen;
	const char *key  = luaL_checklstring(L, 1, &klen);
	const char *iv   = luaL_checklstring(L, 2, &ilen);
	const char *data = luaL_checklstring(L, 3, &dlen);

	if (klen != 8 || ilen != 8)
		return luaL_error(L, "key and iv must be 8 bytes");

	uint8_t *out = NULL;
	int out_len = des_cbc_encrypt((const uint8_t *)key, (const uint8_t *)iv,
	                              (const uint8_t *)data, (int)dlen, &out);
	if (out_len < 0 || !out)
		return luaL_error(L, "DES-CBC encryption failed");

	lua_pushlstring(L, (const char *)out, out_len);
	free(out);
	return 1;
}

int
luaopen_serialize(lua_State *L) {
	luaL_Reg l[] = {
		{ "pack", lpack },
		{ "unpack", lunpack },
		{ "append", lappend },
		{ "serialize", lserialize },
		{ "deserialize", ldeserialize },
		{ "serialize_string", seristring },
		{ "deseristring_string", deseristring },
		{ "serialize_string_lz4", seristring_lz4 },
		{ "deseristring_string_lz4", deseristring_lz4 },
		{ "des_decrypt_cbc", ldes_decrypt_cbc },
		{ "des_encrypt_cbc", ldes_encrypt_cbc },
		{ "dump", _dump },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);
	return 1;
}
